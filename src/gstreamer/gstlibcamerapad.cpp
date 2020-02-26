/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Collabora Ltd.
 *     Author: Nicolas Dufresne <nicolas.dufresne@collabora.com>
 *
 * gstlibcamerapad.cpp - GStreamer Capture Pad
 */

#include "gstlibcamerapad.h"
#include "gstlibcamera-utils.h"

#include <libcamera/stream.h>

using namespace libcamera;

struct _GstLibcameraPad {
	GstPad parent;
	StreamRole role;
	GstLibcameraPool *pool;
	GQueue pending_buffers;
	GstClockTime latency;
};

enum {
	PROP_0,
	PROP_STREAM_ROLE
};

G_DEFINE_TYPE(GstLibcameraPad, gst_libcamera_pad, GST_TYPE_PAD);

static void
gst_libcamera_pad_set_property(GObject *object, guint prop_id,
			       const GValue *value, GParamSpec *pspec)
{
	auto *self = GST_LIBCAMERA_PAD(object);
	GLibLocker lock(GST_OBJECT(self));

	switch (prop_id) {
	case PROP_STREAM_ROLE:
		self->role = (StreamRole)g_value_get_enum(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gst_libcamera_pad_get_property(GObject *object, guint prop_id, GValue *value,
			       GParamSpec *pspec)
{
	auto *self = GST_LIBCAMERA_PAD(object);
	GLibLocker lock(GST_OBJECT(self));

	switch (prop_id) {
	case PROP_STREAM_ROLE:
		g_value_set_enum(value, self->role);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gst_libcamera_pad_init(GstLibcameraPad *self)
{
}

static GType
gst_libcamera_stream_role_get_type(void)
{
	static GType type = 0;
	static const GEnumValue values[] = {
		{ StillCapture, "libcamera::StillCapture", "still-capture" },
		{ VideoRecording, "libcamera::VideoRecording", "video-recording" },
		{ Viewfinder, "libcamera::Viewfinder", "view-finder" },
		{ 0, NULL, NULL }
	};

	if (!type)
		type = g_enum_register_static("GstLibcameraStreamRole", values);

	return type;
}

static void
gst_libcamera_pad_class_init(GstLibcameraPadClass *klass)
{
	auto *object_class = G_OBJECT_CLASS(klass);

	object_class->set_property = gst_libcamera_pad_set_property;
	object_class->get_property = gst_libcamera_pad_get_property;

	auto *spec = g_param_spec_enum("stream-role", "Stream Role",
				       "The selected stream role",
				       gst_libcamera_stream_role_get_type(),
				       VideoRecording,
				       (GParamFlags)(GST_PARAM_MUTABLE_READY
					             | G_PARAM_CONSTRUCT
						     | G_PARAM_READWRITE
						     | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_STREAM_ROLE, spec);
}

StreamRole
gst_libcamera_pad_get_role(GstPad *pad)
{
	auto *self = GST_LIBCAMERA_PAD(pad);
	GLibLocker lock(GST_OBJECT(self));
	return self->role;
}

GstLibcameraPool *
gst_libcamera_pad_get_pool(GstPad *pad)
{
	auto *self = GST_LIBCAMERA_PAD(pad);
	return self->pool;
}

void
gst_libcamera_pad_set_pool(GstPad *pad, GstLibcameraPool *pool)
{
	auto *self = GST_LIBCAMERA_PAD(pad);

	if (self->pool)
		g_object_unref(self->pool);
	self->pool = pool;
}

Stream *
gst_libcamera_pad_get_stream(GstPad *pad)
{
	auto *self = GST_LIBCAMERA_PAD(pad);

	if (self->pool)
		return gst_libcamera_pool_get_stream(self->pool);

	return nullptr;
}

void
gst_libcamera_pad_queue_buffer(GstPad *pad, GstBuffer *buffer)
{
	auto *self = GST_LIBCAMERA_PAD(pad);
	GLibLocker lock(GST_OBJECT(self));

	g_queue_push_head(&self->pending_buffers, buffer);
}

GstFlowReturn
gst_libcamera_pad_push_pending(GstPad *pad)
{
	auto *self = GST_LIBCAMERA_PAD(pad);
	GstBuffer *buffer;

	{
		GLibLocker lock(GST_OBJECT(self));
		buffer = GST_BUFFER(g_queue_pop_tail(&self->pending_buffers));
	}

	if (!buffer)
		return GST_FLOW_OK;

	return gst_pad_push(pad, buffer);
}

bool
gst_libcamera_pad_has_pending(GstPad *pad)
{
	auto *self = GST_LIBCAMERA_PAD(pad);
	GLibLocker lock(GST_OBJECT(self));
	return (self->pending_buffers.length > 0);
}
