/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Collabora Ltd.
 *     Author: Nicolas Dufresne <nicolas.dufresne@collabora.com>
 *
 * gstlibcamerasrc.cpp - GStreamer Capture Element
 */

/* TODO
 *  - Implement GstElement::send_event
 *    + Allowing application to send EOS
 *    + Allowing application to use FLUSH/FLUSH_STOP
 *    + Prevent the main thread from accessing streaming thread
 *  - Implement renegotiation (even if slow)
 *  - Implement GstElement::request-new-pad (multi stream)
 *    + Evaluate if a single streaming thread is fine
 *  - Add application driven request (snapshot)
 *  - Add framerate control
 *
 *  Requires new libcamera API:
 *  - Add framerate negotiation support
 *  - Add colorimetry support
 *  - Add timestamp support
 *  - Use unique names to select the camera
 *  - Add GstVideoMeta support (strides and offsets)
 *  - Add buffer importation support
 */

#include "gstlibcamerasrc.h"
#include "gstlibcamerapad.h"
#include "gstlibcameraallocator.h"
#include "gstlibcamerapool.h"
#include "gstlibcamera-utils.h"

#include <queue>
#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <gst/base/base.h>

using namespace libcamera;

GST_DEBUG_CATEGORY_STATIC(source_debug);
#define GST_CAT_DEFAULT source_debug

#define STREAM_LOCKER(obj) g_autoptr(GRecMutexLocker) stream_locker = g_rec_mutex_locker_new(&GST_LIBCAMERA_SRC(obj)->stream_lock)

struct RequestWrap {
	RequestWrap(Request *request);
	~RequestWrap();

	void AttachBuffer(GstBuffer *buffer);
	GstBuffer *DetachBuffer(Stream *stream);

	/* For ptr comparision only */
	Request *request_;
	std::map<Stream *, GstBuffer *> buffers_;
};

RequestWrap::RequestWrap(Request *request)
	: request_(request)
{
}

RequestWrap::~RequestWrap()
{
	for (std::pair<Stream * const, GstBuffer *> &item : buffers_) {
		if (item.second)
			gst_buffer_unref(item.second);
	}
}

void
RequestWrap::AttachBuffer(GstBuffer *buffer)
{
	FrameBuffer *fb = gst_libcamera_buffer_get_frame_buffer(buffer);
	Stream *stream = gst_libcamera_buffer_get_stream(buffer);

	request_->addBuffer(stream, fb);

	auto item = buffers_.find(stream);
	if (item != buffers_.end()) {
		gst_buffer_unref(item->second);
		item->second = buffer;
	} else {
		buffers_[stream] = buffer;
	}
}

GstBuffer *
RequestWrap::DetachBuffer(Stream *stream)
{
	GstBuffer *buffer = nullptr;

	auto item = buffers_.find(stream);
	if (item != buffers_.end()) {
		buffer = item->second;
		item->second = nullptr;
	}

	return buffer;
}

/* Used for C++ object with destructors and callbacks */
struct GstLibcameraSrcState {
	GstLibcameraSrc *src;

	std::shared_ptr<CameraManager> cm;
	std::shared_ptr<Camera> cam;
	std::unique_ptr<CameraConfiguration> config;
	std::vector<GstPad *> srcpads;
	std::queue<std::unique_ptr<RequestWrap>> requests;

	void requestCompleted(Request *request);
};

struct _GstLibcameraSrc {
	GstElement parent;

	GRecMutex stream_lock;
	GstTask *task;

	gchar *camera_name;

	GstLibcameraSrcState *state;
	GstLibcameraAllocator *allocator;
	GstFlowCombiner *flow_combiner;
};

enum {
	PROP_0,
	PROP_CAMERA_NAME
};

G_DEFINE_TYPE_WITH_CODE(GstLibcameraSrc, gst_libcamera_src, GST_TYPE_ELEMENT,
			GST_DEBUG_CATEGORY_INIT(source_debug, "libcamerasrc", 0,
						"LibCamera Source"));

#define TEMPLATE_CAPS GST_STATIC_CAPS("video/x-raw;image/jpeg")

/* For the simple case, we have a src pad that is always present. */
GstStaticPadTemplate src_template = {
	"src", GST_PAD_SRC, GST_PAD_ALWAYS, TEMPLATE_CAPS
};

/* More pads can be requested in state < PAUSED */
GstStaticPadTemplate request_src_template = {
	"src_%s", GST_PAD_SRC, GST_PAD_REQUEST, TEMPLATE_CAPS
};

void
GstLibcameraSrcState::requestCompleted(Request *request)
{
	GST_OBJECT_LOCKER(this->src);

	GST_DEBUG_OBJECT(this->src, "buffers are ready");

	std::unique_ptr<RequestWrap> wrap = std::move(this->requests.front());
	this->requests.pop();

	g_return_if_fail(wrap->request_ == request);

	if ((request->status() == Request::RequestCancelled)) {
		GST_DEBUG_OBJECT(this->src, "Request was cancelled");
		return;
	}

	GstBuffer *buffer;
	for (GstPad *srcpad : this->srcpads) {
		Stream *stream = gst_libcamera_pad_get_stream(srcpad);
		buffer = wrap->DetachBuffer(stream);
		gst_libcamera_pad_queue_buffer(srcpad, buffer);
	}

	{
		/* We only want to resume the task if it's paused */
		GstTask *task = this->src->task;
		GST_OBJECT_LOCKER(task);
		if (GST_TASK_STATE(task) == GST_TASK_PAUSED) {
			GST_TASK_STATE(task) = GST_TASK_STARTED;
			GST_TASK_SIGNAL(task);
		}
	}
}

static bool
gst_libcamera_src_open(GstLibcameraSrc *self)
{
	std::shared_ptr<CameraManager> cm = std::make_shared<CameraManager>();
	std::shared_ptr<Camera> cam = nullptr;
	gint ret = 0;
	g_autofree gchar *camera_name = nullptr;

	GST_DEBUG_OBJECT(self, "Opening camera device ...");

	ret = cm->start();
	if (ret) {
		GST_ELEMENT_ERROR(self, LIBRARY, INIT,
				  ("Failed listing cameras."),
				  ("libcamera::CameraMananger.start() failed: %s", g_strerror(-ret)));
		return false;
	}

	{
		GST_OBJECT_LOCKER(self);
		if (self->camera_name)
			camera_name = g_strdup(self->camera_name);
	}

	if (camera_name) {
		cam = cm->get(self->camera_name);
		if (!cam) {
			GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
					  ("Could not find a camera named '%s'.", self->camera_name),
					  ("libcamera::CameraMananger.get() returned NULL"));
			return false;
		}
	} else {
		if (cm->cameras().empty()) {
			GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND,
					  ("Could not find any supported camera on this system."),
					  ("libcamera::CameraMananger.cameras() is empty"));
			return false;
		}
		cam = cm->cameras()[0];
	}

	GST_INFO_OBJECT(self, "Using camera named '%s'", cam->name().c_str());

	ret = cam->acquire();
	if (ret) {
		GST_ELEMENT_ERROR(self, RESOURCE, BUSY,
				  ("Camera name '%s' is already in use.", cam->name().c_str()),
				  ("libcamera::Camera.acquire() failed: %s", g_strerror(ret)));
		return false;
	}

	cam->requestCompleted.connect(self->state, &GstLibcameraSrcState::requestCompleted);

	/* no need to lock here we didn't start our threads */
	self->state->cm = cm;
	self->state->cam = cam;

	return true;
}

static void
gst_libcamera_src_task_run(gpointer user_data)
{
	GstLibcameraSrc *self = GST_LIBCAMERA_SRC(user_data);
	GstLibcameraSrcState *state = self->state;

	Request *request = new Request(state->cam.get());
	auto wrap = std::make_unique<RequestWrap>(request);
	for (GstPad *srcpad : state->srcpads) {
		GstLibcameraPool *pool = gst_libcamera_pad_get_pool(srcpad);
		GstBuffer *buffer;
		GstFlowReturn ret;

		ret = gst_buffer_pool_acquire_buffer(GST_BUFFER_POOL(pool),
						     &buffer, nullptr);
		if (ret != GST_FLOW_OK) {
			/* RequestWrap does not take ownership, and we won't be
			 * queueing this one due to lack of buffers */
			delete request;
			request = NULL;
			break;
		}

		wrap->AttachBuffer(buffer);
	}

	if (request) {
		GST_OBJECT_LOCKER(self);
		GST_TRACE_OBJECT(self, "Requesting buffers");
		state->cam->queueRequest(request);
		state->requests.push(std::move(wrap));
	}

	GstFlowReturn ret = GST_FLOW_OK;
	gst_flow_combiner_reset(self->flow_combiner);
	for (GstPad *srcpad : state->srcpads) {
		ret = gst_libcamera_pad_push_pending(srcpad);
		ret = gst_flow_combiner_update_pad_flow(self->flow_combiner,
							srcpad, ret);
	}

	{
		/* Here we need to decide if we want to pause or stop the task. This
		 * needs to happend in lock step with the callback thread which may want
		 * to resume the task.
		 */
		GST_OBJECT_LOCKER(self);
		if (ret != GST_FLOW_OK) {
			if (ret == GST_FLOW_EOS) {
				g_autoptr(GstEvent) eos = gst_event_new_eos();
				guint32 seqnum = gst_util_seqnum_next();
				gst_event_set_seqnum(eos, seqnum);
				for (GstPad *srcpad : state->srcpads)
					gst_pad_push_event(srcpad, gst_event_ref(eos));
			} else if (ret != GST_FLOW_FLUSHING) {
				GST_ELEMENT_FLOW_ERROR(self, ret);
			}
			gst_task_stop(self->task);
			return;
		}

		gboolean do_pause = true;
		for (GstPad *srcpad : state->srcpads) {
			if (gst_libcamera_pad_has_pending(srcpad)) {
				do_pause = false;
				break;
			}
		}

		if (do_pause)
			gst_task_pause(self->task);
	}
}

static void
gst_libcamera_src_task_enter(GstTask *task, GThread *thread, gpointer user_data)
{
	STREAM_LOCKER(user_data);
	GstLibcameraSrc *self = GST_LIBCAMERA_SRC(user_data);
	GstLibcameraSrcState *state = self->state;
	GstFlowReturn flow_ret = GST_FLOW_OK;
	gint ret = 0;

	GST_DEBUG_OBJECT(self, "Streaming thread has started");

	guint group_id = gst_util_group_id_next();
	StreamRoles roles;
	for (GstPad *srcpad : state->srcpads) {
		/* Create stream-id and push stream-start */
		g_autofree gchar *stream_id = gst_pad_create_stream_id(srcpad, GST_ELEMENT(self), nullptr);
		GstEvent *event = gst_event_new_stream_start(stream_id);
		gst_event_set_group_id(event, group_id);
		gst_pad_push_event(srcpad, event);

		/* Collect the streams roles for the next iteration */
		roles.push_back(gst_libcamera_pad_get_role(srcpad));
	}

	/* Generate the stream configurations, there should be one per pad */
	state->config = state->cam->generateConfiguration(roles);
	g_assert(state->config->size() == state->srcpads.size());

	for (gsize i = 0; i < state->srcpads.size(); i++) {
		GstPad *srcpad = state->srcpads[i];
		StreamConfiguration &stream_cfg = state->config->at(i);

		/* Retreive the supported caps */
		g_autoptr(GstCaps) filter = gst_libcamera_stream_formats_to_caps(stream_cfg.formats());
		g_autoptr(GstCaps) caps = gst_pad_peer_query_caps(srcpad, filter);
		if (gst_caps_is_empty(caps)) {
			flow_ret = GST_FLOW_NOT_NEGOTIATED;
			break;
		}

		/* Fixate caps and configure the stream */
		caps = gst_caps_make_writable(caps);
		gst_libcamera_configure_stream_from_caps(stream_cfg, caps);
	}

	if (flow_ret != GST_FLOW_OK)
		goto done;

	/* Validate the configuration */
	if (state->config->validate() == CameraConfiguration::Invalid) {
		flow_ret = GST_FLOW_NOT_NEGOTIATED;
		goto done;
	}

	/* Regardless if it has been modified, create clean caps and push the
	 * caps event, downstream will decide if hte caps are acceptable */
	for (gsize i = 0; i < state->srcpads.size(); i++) {
		GstPad *srcpad = state->srcpads[i];
		const StreamConfiguration &stream_cfg = state->config->at(i);

		g_autoptr(GstCaps) caps = gst_libcamera_stream_configuration_to_caps(stream_cfg);
		if (!gst_pad_push_event(srcpad, gst_event_new_caps(caps))) {
			flow_ret = GST_FLOW_NOT_NEGOTIATED;
			break;
		}

		/* Send an open segment event with time format */
		GstSegment segment;
		gst_segment_init(&segment, GST_FORMAT_TIME);
		gst_pad_push_event(srcpad, gst_event_new_segment(&segment));
	}

	ret = state->cam->configure(state->config.get());
	if (ret) {
		GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
				  ("Failed to configure camera: %s", g_strerror(-ret)),
				  ("Camera.configure() failed with error code %i", ret));
		gst_task_stop(task);
		return;
	}

	self->allocator = gst_libcamera_allocator_new(state->cam);
	if (!self->allocator) {
		GST_ELEMENT_ERROR(self, RESOURCE, NO_SPACE_LEFT,
				  ("Failed to allocate memory"),
				  ("gst_libcamera_allocator_new() failed."));
		gst_task_stop(task);
		return;
	}

	self->flow_combiner = gst_flow_combiner_new();
	for (gsize i = 0; i < state->srcpads.size(); i++) {
		GstPad *srcpad = state->srcpads[i];
		const StreamConfiguration &stream_cfg = state->config->at(i);
		GstLibcameraPool *pool = gst_libcamera_pool_new(self->allocator,
								stream_cfg.stream());
		gst_libcamera_pad_set_pool(srcpad, pool);
		gst_flow_combiner_add_pad(self->flow_combiner, srcpad);
	}

	ret = state->cam->start();
	if (ret) {
		GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
				  ("Failed to start the camera: %s", g_strerror(-ret)),
				  ("Camera.start() failed with error code %i", ret));
		gst_task_stop(task);
		return;
	}

done:
	switch (flow_ret) {
	case GST_FLOW_NOT_NEGOTIATED:
		for (GstPad *srcpad : state->srcpads) {
			gst_pad_push_event(srcpad, gst_event_new_eos());
		}
		GST_ELEMENT_FLOW_ERROR(self, flow_ret);
		gst_task_stop(task);
		return;
	default:
		break;
	}
}

static void
gst_libcamera_src_task_leave(GstTask *task, GThread *thread, gpointer user_data)
{
	GstLibcameraSrc *self = GST_LIBCAMERA_SRC(user_data);
	GstLibcameraSrcState *state = self->state;

	GST_DEBUG_OBJECT(self, "Streaming thread is about to stop");
	state->cam->stop();

	for (GstPad *srcpad : state->srcpads)
		gst_libcamera_pad_set_pool(srcpad, NULL);

	g_clear_object(&self->allocator);
	g_clear_pointer(&self->flow_combiner,
			(GDestroyNotify)gst_flow_combiner_free);
}

static void
gst_libcamera_src_close(GstLibcameraSrc *self)
{
	GstLibcameraSrcState *state = self->state;
	gint ret;

	GST_DEBUG_OBJECT(self, "Releasing resources");

	ret = state->cam->release();
	if (ret) {
		GST_ELEMENT_WARNING(self, RESOURCE, BUSY,
				    ("Camera name '%s' is still in use.", state->cam->name().c_str()),
				    ("libcamera::Camera.release() failed: %s", g_strerror(-ret)));
	}

	state->cam.reset();
	state->cm->stop();
	state->cm.reset();
}

static void
gst_libcamera_src_set_property(GObject *object, guint prop_id,
			       const GValue *value, GParamSpec *pspec)
{
	GST_OBJECT_LOCKER(object);
	GstLibcameraSrc *self = GST_LIBCAMERA_SRC(object);

	switch (prop_id) {
	case PROP_CAMERA_NAME:
		g_free (self->camera_name);
		self->camera_name = g_value_dup_string(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gst_libcamera_src_get_property(GObject *object, guint prop_id, GValue *value,
			       GParamSpec *pspec)
{
	GST_OBJECT_LOCKER(object);
	GstLibcameraSrc *self = GST_LIBCAMERA_SRC(object);

	switch (prop_id) {
	case PROP_CAMERA_NAME:
		g_value_set_string(value, self->camera_name);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static GstStateChangeReturn
gst_libcamera_src_change_state(GstElement *element, GstStateChange transition)
{
	GstLibcameraSrc *self = GST_LIBCAMERA_SRC(element);
	GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
	GstElementClass *klass = GST_ELEMENT_CLASS(gst_libcamera_src_parent_class);

	ret = klass->change_state(element, transition);
	if (ret == GST_STATE_CHANGE_FAILURE)
		return ret;

	switch (transition) {
	case GST_STATE_CHANGE_NULL_TO_READY:
		if (!gst_libcamera_src_open(self))
			return GST_STATE_CHANGE_FAILURE;
		break;
	case GST_STATE_CHANGE_READY_TO_PAUSED:
		/* This needs to be called after pads activation */
		if (!gst_task_pause(self->task))
			return GST_STATE_CHANGE_FAILURE;
		ret = GST_STATE_CHANGE_NO_PREROLL;
		break;
	case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
		gst_task_start(self->task);
		break;
	case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
		ret = GST_STATE_CHANGE_NO_PREROLL;
		break;
	case GST_STATE_CHANGE_PAUSED_TO_READY:
		/* FIXME maybe this should happen before pad deactivation ? */
		gst_task_join(self->task);
		break;
	case GST_STATE_CHANGE_READY_TO_NULL:
		gst_libcamera_src_close(self);
		break;
	default:
		break;
	}

	return ret;
}

static void
gst_libcamera_src_finalize(GObject *object)
{
	GObjectClass *klass = G_OBJECT_CLASS(gst_libcamera_src_parent_class);
	GstLibcameraSrc *self = GST_LIBCAMERA_SRC(object);

	g_rec_mutex_clear(&self->stream_lock);
	g_clear_object(&self->task);
	g_free(self->camera_name);
	delete self->state;

	return klass->finalize(object);
}

static void
gst_libcamera_src_init(GstLibcameraSrc *self)
{
	GstLibcameraSrcState *state = new GstLibcameraSrcState();
	GstPadTemplate *templ = gst_element_get_pad_template(GST_ELEMENT(self), "src");

	g_rec_mutex_init(&self->stream_lock);
	self->task = gst_task_new(gst_libcamera_src_task_run, self, nullptr);
	gst_task_set_enter_callback(self->task, gst_libcamera_src_task_enter, self, nullptr);
	gst_task_set_leave_callback(self->task, gst_libcamera_src_task_leave, self, nullptr);
	gst_task_set_lock(self->task, &self->stream_lock);

	state->srcpads.push_back(gst_pad_new_from_template(templ, "src"));
	gst_element_add_pad(GST_ELEMENT(self), state->srcpads[0]);

	/* C-style friend */
	state->src = self;
	self->state = state;
}

static void
gst_libcamera_src_class_init(GstLibcameraSrcClass *klass)
{
	GstElementClass *element_class = (GstElementClass *)klass;
	GObjectClass *object_class = (GObjectClass *)klass;
	GParamSpec *spec;

	object_class->set_property = gst_libcamera_src_set_property;
	object_class->get_property = gst_libcamera_src_get_property;
	object_class->finalize = gst_libcamera_src_finalize;

	element_class->change_state = gst_libcamera_src_change_state;

	gst_element_class_set_metadata(element_class,
				       "LibCamera Source", "Source/Video",
				       "Linux Camera source using libcamera",
				       "Nicolas Dufresne <nicolas.dufresne@collabora.com");
	gst_element_class_add_static_pad_template_with_gtype(element_class,
							     &src_template,
							     GST_TYPE_LIBCAMERA_PAD);
	gst_element_class_add_static_pad_template_with_gtype(element_class,
							     &request_src_template,
							     GST_TYPE_LIBCAMERA_PAD);

	spec = g_param_spec_string("camera-name", "Camera Name",
				   "Select by name which camera to use.", nullptr,
				   (GParamFlags)(GST_PARAM_MUTABLE_READY
					   | G_PARAM_CONSTRUCT
					   | G_PARAM_READWRITE
					   | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_CAMERA_NAME, spec);
}
