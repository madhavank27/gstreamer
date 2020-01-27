/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2020, Collabora Ltd.
 *     Author: Nicolas Dufresne <nicolas.dufresne@collabora.com>
 *
 * gstlibcameraallocator.cpp - GStreamer Custom Allocator
 */

#include "gstlibcameraallocator.h"
#include "gstlibcamera-utils.h"

#include <libcamera/camera.h>
#include <libcamera/stream.h>
#include <libcamera/framebuffer_allocator.h>

using namespace libcamera;

/***********************************************************************/
/* Internal object for tracking memories associated with a FrameBuffer */
/***********************************************************************/

static gboolean gst_libcamera_allocator_release(GstMiniObject *mini_object);

/* This internal object is used to track the outstanding GstMemory object that
 * are part of a FrameBuffer. The allocator will only re-use a FrameBuffer when
 * al all outstranging GstMemory have returned.
 */
struct FrameWrap {
	FrameWrap(GstAllocator *allocator, FrameBuffer *buffer,
		  gpointer stream);
	~FrameWrap();

	void AcquirePlane() { ++outstanding_planes_; }
	bool ReleasePlane() { return --outstanding_planes_ == 0; }

	gpointer stream_;
	FrameBuffer *buffer_;
	std::vector<GstMemory *> planes_;
	gint outstanding_planes_;
};

static GQuark
gst_libcamera_frame_quark(void)
{
	static gsize frame_quark = 0;

	if (g_once_init_enter(&frame_quark)) {
		GQuark quark = g_quark_from_string("GstLibCameraFrameWrap");
		g_once_init_leave(&frame_quark, quark);
	}

	return frame_quark;
}

FrameWrap::FrameWrap(GstAllocator *allocator, FrameBuffer *buffer,
		     gpointer stream)

	: stream_(stream),
	  buffer_(buffer),
	  outstanding_planes_(0)
{
	for (const FrameBuffer::Plane &plane : buffer->planes()) {
		GstMemory *mem = gst_fd_allocator_alloc(allocator,
							plane.fd.fd(),
							plane.length,
							GST_FD_MEMORY_FLAG_DONT_CLOSE);
		gst_mini_object_set_qdata(GST_MINI_OBJECT(mem),
					  gst_libcamera_frame_quark(),
					  this, NULL);
		GST_MINI_OBJECT(mem)->dispose = gst_libcamera_allocator_release;
		g_object_unref(mem->allocator);
		planes_.push_back(mem);
	}
}

FrameWrap::~FrameWrap()
{
	for (GstMemory *mem : planes_) {
		GST_MINI_OBJECT(mem)->dispose = nullptr;
		g_object_ref(mem->allocator);
		gst_memory_unref(mem);
	}
}

/***********************************/
/* The GstAllocator implementation */
/***********************************/
struct _GstLibcameraAllocator {
	GstDmaBufAllocator parent;
	FrameBufferAllocator *fb_allocator;
	/* A hash table using Stream pointer as key and returning a GQueue of
	 * FrameWrap. */
	GHashTable *pools;
};

G_DEFINE_TYPE(GstLibcameraAllocator, gst_libcamera_allocator,
	      GST_TYPE_DMABUF_ALLOCATOR);

static gboolean
gst_libcamera_allocator_release(GstMiniObject *mini_object)
{
	GstMemory *mem = GST_MEMORY_CAST(mini_object);
	GstLibcameraAllocator *self = GST_LIBCAMERA_ALLOCATOR(mem->allocator);
	GST_OBJECT_LOCKER(self);
	gpointer data = gst_mini_object_get_qdata(mini_object,
						  gst_libcamera_frame_quark());
	auto *frame = (FrameWrap *)data;

	gst_memory_ref(mem);

	if (frame->ReleasePlane()) {
		auto *pool = (GQueue *)g_hash_table_lookup(self->pools, frame->stream_);
		g_return_val_if_fail(pool, TRUE);
		g_queue_push_tail(pool, frame);
	}

	/* Keep last in case we are holding on the last allocator ref */
	g_object_unref(mem->allocator);

	/* Rreturns FALSE so that our mini object isn't freed */
	return FALSE;
}

static void
gst_libcamera_allocator_free_pool(gpointer data)
{
	GQueue *queue = (GQueue *)data;
	FrameWrap *frame;

	while ((frame = (FrameWrap *)g_queue_pop_head(queue))) {
		g_warn_if_fail(frame->outstanding_planes_ == 0);
		delete frame;
	}

	g_queue_free(queue);
}

static void
gst_libcamera_allocator_init(GstLibcameraAllocator *self)
{
	self->pools = g_hash_table_new_full(NULL, NULL, NULL,
					    gst_libcamera_allocator_free_pool);
	GST_OBJECT_FLAG_SET(self, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

static void
gst_libcamera_allocator_dispose(GObject *object)
{
	GstLibcameraAllocator *self = GST_LIBCAMERA_ALLOCATOR(object);

	if (self->pools) {
		g_hash_table_unref(self->pools);
		self->pools = NULL;
	}

	G_OBJECT_CLASS(gst_libcamera_allocator_parent_class)->dispose(object);
}

static void
gst_libcamera_allocator_finalize(GObject *object)
{
	GstLibcameraAllocator *self = GST_LIBCAMERA_ALLOCATOR(object);

	delete self->fb_allocator;

	G_OBJECT_CLASS(gst_libcamera_allocator_parent_class)->finalize(object);
}

static void
gst_libcamera_allocator_class_init(GstLibcameraAllocatorClass *klass)
{
	auto *allocator_class = GST_ALLOCATOR_CLASS(klass);
	auto *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = gst_libcamera_allocator_dispose;
	object_class->finalize = gst_libcamera_allocator_finalize;
	allocator_class->alloc = NULL;
}

GstLibcameraAllocator *
gst_libcamera_allocator_new(std::shared_ptr<Camera> camera)
{
	auto *self = (GstLibcameraAllocator *)g_object_new(GST_TYPE_LIBCAMERA_ALLOCATOR,
							   nullptr);

	self->fb_allocator = FrameBufferAllocator::create(camera);
	for (Stream *stream : camera->streams()) {
		gint ret;

		ret = self->fb_allocator->allocate(stream);
		if (ret == 0)
			return nullptr;

		GQueue *pool = g_queue_new();
		for (const std::unique_ptr<FrameBuffer> &buffer :
		     self->fb_allocator->buffers(stream)) {
			auto *fb = new FrameWrap(GST_ALLOCATOR(self),
						 buffer.get(), stream);
			g_queue_push_tail(pool, fb);
		}

		g_hash_table_insert(self->pools, stream, pool);
	}

	return self;
}

bool
gst_libcamera_allocator_prepare_buffer(GstLibcameraAllocator *self,
				       Stream *stream, GstBuffer *buffer)
{
	GST_OBJECT_LOCKER(self);

	auto *pool = (GQueue *)g_hash_table_lookup(self->pools, stream);
	g_return_val_if_fail(pool, false);

	auto *frame = (FrameWrap *)g_queue_pop_head(pool);
	if (!frame)
		return false;

	for (GstMemory *mem : frame->planes_) {
		frame->AcquirePlane();
		gst_buffer_append_memory(buffer, mem);
		g_object_ref(mem->allocator);
	}

	return true;
}

gsize
gst_libcamera_allocator_get_pool_size(GstLibcameraAllocator *self,
				      Stream *stream)
{
	GST_OBJECT_LOCKER(self);

	auto *pool = (GQueue *)g_hash_table_lookup(self->pools, stream);
	g_return_val_if_fail(pool, false);

	return pool->length;
}

FrameBuffer *
gst_libcamera_memory_get_frame_buffer(GstMemory *mem)
{
	gpointer data = gst_mini_object_get_qdata(GST_MINI_OBJECT_CAST(mem),
						  gst_libcamera_frame_quark());
	auto *frame = (FrameWrap *)data;
	return frame->buffer_;
}
