/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Collabora Ltd.
 *     Author: Nicolas Dufresne <nicolas.dufresne@collabora.com>
 *
 * gstlibcamerasrc.cpp - GStreamer Capture Element
 */

#include "gstlibcamera-utils.h"
#include "gstlibcamerapad.h"
#include "gstlibcamerasrc.h"

struct _GstLibcameraSrc {
	GstElement parent;
	GstPad *srcpad;
	gchar *camera_name;
};

enum {
	PROP_0,
	PROP_CAMERA_NAME
};

G_DEFINE_TYPE(GstLibcameraSrc, gst_libcamera_src, GST_TYPE_ELEMENT);

#define TEMPLATE_CAPS GST_STATIC_CAPS("video/x-raw; image/jpeg")

/* For the simple case, we have a src pad that is always present. */
GstStaticPadTemplate src_template = {
	"src", GST_PAD_SRC, GST_PAD_ALWAYS, TEMPLATE_CAPS
};

/* More pads can be requested in state < PAUSED */
GstStaticPadTemplate request_src_template = {
	"src_%s", GST_PAD_SRC, GST_PAD_REQUEST, TEMPLATE_CAPS
};

static void
gst_libcamera_src_set_property(GObject *object, guint prop_id,
			       const GValue *value, GParamSpec *pspec)
{
	GLibLocker lock(GST_OBJECT(object));
	GstLibcameraSrc *self = GST_LIBCAMERA_SRC(object);

	switch (prop_id) {
	case PROP_CAMERA_NAME:
		g_free(self->camera_name);
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
	GLibLocker lock(GST_OBJECT(object));
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

static void
gst_libcamera_src_finalize(GObject *object)
{
	GObjectClass *klass = G_OBJECT_CLASS(gst_libcamera_src_parent_class);
	GstLibcameraSrc *self = GST_LIBCAMERA_SRC(object);

	g_free(self->camera_name);

	return klass->finalize(object);
}

static void
gst_libcamera_src_init(GstLibcameraSrc *self)
{
	GstPadTemplate *templ = gst_element_get_pad_template(GST_ELEMENT(self), "src");

	self->srcpad = gst_pad_new_from_template(templ, "src");
	gst_element_add_pad(GST_ELEMENT(self), self->srcpad);
}

static void
gst_libcamera_src_class_init(GstLibcameraSrcClass *klass)
{
	GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->set_property = gst_libcamera_src_set_property;
	object_class->get_property = gst_libcamera_src_get_property;
	object_class->finalize = gst_libcamera_src_finalize;

	gst_element_class_set_metadata(element_class,
				       "libcamera Source", "Source/Video",
				       "Linux Camera source using libcamera",
				       "Nicolas Dufresne <nicolas.dufresne@collabora.com");
	gst_element_class_add_static_pad_template_with_gtype(element_class,
							     &src_template,
							     GST_TYPE_LIBCAMERA_PAD);
	gst_element_class_add_static_pad_template_with_gtype(element_class,
							     &request_src_template,
							     GST_TYPE_LIBCAMERA_PAD);

	GParamSpec *spec = g_param_spec_string("camera-name", "Camera Name",
					      "Select by name which camera to use.", nullptr,
					      (GParamFlags)(GST_PARAM_MUTABLE_READY
							    | G_PARAM_CONSTRUCT
							    | G_PARAM_READWRITE
							    | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_CAMERA_NAME, spec);
}
