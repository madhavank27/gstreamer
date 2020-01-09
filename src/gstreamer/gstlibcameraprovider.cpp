/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2020, Collabora Ltd.
 *     Author: Nicolas Dufresne <nicolas.dufresne@collabora.com>
 *
 * gstlibcameraprovider.c - GStreamer Device Provider
 */

#include "gstlibcamera-utils.h"
#include "gstlibcameraprovider.h"
#include "gstlibcamerasrc.h"

#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>

using namespace libcamera;

GST_DEBUG_CATEGORY_STATIC(provider_debug);
#define GST_CAT_DEFAULT provider_debug

/**************************************/
/* GstLibcameraDevice internal Object */
/**************************************/

enum {
	PROP_DEVICE_NAME = 1,
};

#define GST_TYPE_LIBCAMERA_DEVICE gst_libcamera_device_get_type()
G_DECLARE_FINAL_TYPE(GstLibcameraDevice, gst_libcamera_device,
		     GST_LIBCAMERA, DEVICE, GstDevice);

struct _GstLibcameraDevice {
	GstDevice parent;
	gchar *name;
};

G_DEFINE_TYPE(GstLibcameraDevice, gst_libcamera_device, GST_TYPE_DEVICE);

static GstElement *
gst_libcamera_device_create_element(GstDevice *device, const gchar *name)
{
	GstElement *source;

	source = gst_element_factory_make("libcamerasrc", name);
	g_assert(source);

	g_object_set(source, "camera-name", GST_LIBCAMERA_DEVICE(device)->name, NULL);

	return source;
}

static gboolean
gst_libcamera_device_reconfigure_element(GstDevice *device,
					 GstElement *element)
{
	if (!GST_LIBCAMERA_IS_SRC(element))
		return FALSE;

	g_object_set(element, "camera-name", GST_LIBCAMERA_DEVICE(device)->name, NULL);

	return TRUE;
}

static void
gst_libcamera_device_set_property(GObject *object, guint prop_id,
				  const GValue *value, GParamSpec *pspec)
{
	GstLibcameraDevice *device;

	device = GST_LIBCAMERA_DEVICE(object);

	switch (prop_id) {
	case PROP_DEVICE_NAME:
		device->name = g_value_dup_string(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gst_libcamera_device_init(GstLibcameraDevice *self)
{
}

static void
gst_libcamera_device_finalize(GObject *object)
{
	GstLibcameraDevice *self = GST_LIBCAMERA_DEVICE(object);
	gpointer klass = gst_libcamera_device_parent_class;

	g_free(self->name);

	G_OBJECT_GET_CLASS(klass)->finalize(object);
}

static void
gst_libcamera_device_class_init(GstLibcameraDeviceClass *klass)
{
	GParamSpec *pspec;
	GstDeviceClass *device_class = (GstDeviceClass *)klass;
	GObjectClass *object_class = (GObjectClass *)klass;

	device_class->create_element = gst_libcamera_device_create_element;
	device_class->reconfigure_element = gst_libcamera_device_reconfigure_element;

	object_class->set_property = gst_libcamera_device_set_property;
	object_class->finalize = gst_libcamera_device_finalize;

	pspec = g_param_spec_string("name", "Name",
				    "The name of the camera device", "",
				    (GParamFlags)(G_PARAM_STATIC_STRINGS | G_PARAM_WRITABLE |
						  G_PARAM_CONSTRUCT_ONLY));
	g_object_class_install_property(object_class, PROP_DEVICE_NAME, pspec);
}

static GstDevice *
gst_libcamera_device_new(const std::shared_ptr<Camera> &camera)
{
	g_autoptr(GstCaps) caps = gst_caps_new_empty();
	g_autoptr(GstStructure) props = NULL;
	const gchar *name = camera->name().c_str();
	StreamRoles roles;

	roles.push_back(StreamRole::VideoRecording);
	std::unique_ptr<CameraConfiguration> config = camera->generateConfiguration(roles);

	for (const StreamConfiguration &stream_cfg : *config) {
		GstCaps *sub_caps = gst_libcamera_stream_formats_to_caps(stream_cfg.formats());
		if (sub_caps)
			gst_caps_append(caps, sub_caps);
	}

	return (GstDevice *)g_object_new(GST_TYPE_LIBCAMERA_DEVICE,
					 /* FIXME not sure name is unique */
					 "name", name,
					 "display-name", name,
					 "caps", caps,
					 "device-class", "Source/Video",
					 "properties", props,
					 NULL);
}

/*************************************/
/* GstLibcameraDeviceProvider Object */
/*************************************/

struct _GstLibcameraProvider {
	GstDeviceProvider parent;
	CameraManager *cm;
};

G_DEFINE_TYPE_WITH_CODE(GstLibcameraProvider, gst_libcamera_provider,
			GST_TYPE_DEVICE_PROVIDER,
			GST_DEBUG_CATEGORY_INIT(provider_debug, "libcamera-provider", 0,
						"LibCamera Device Provider"));

static GList *
gst_libcamera_provider_probe(GstDeviceProvider *provider)
{
	GstLibcameraProvider *self = GST_LIBCAMERA_PROVIDER(provider);
	CameraManager *cm = self->cm;
	GList *devices = NULL;
	gint ret;

	GST_INFO_OBJECT(self, "Probing cameras using LibCamera");

	/* FIXME as long as the manager isn't able to handle hot-plug, we need to
	 * cycle start/stop here in order to ensure wthat we have an up to date
	 * list */
	ret = cm->start();
	if (ret) {
		GST_ERROR_OBJECT(self, "Failed to retrieve device list: %s",
				 g_strerror(-ret));
		return NULL;
	}

	for (const std::shared_ptr<Camera> &camera : cm->cameras()) {
		GST_INFO_OBJECT(self, "Found camera '%s'", camera->name().c_str());
		devices = g_list_append(devices,
					g_object_ref_sink(gst_libcamera_device_new(camera)));
	}

	cm->stop();

	return devices;
}

static void
gst_libcamera_provider_init(GstLibcameraProvider *self)
{
	GstDeviceProvider *provider = GST_DEVICE_PROVIDER(self);

	self->cm = new CameraManager();

	/* Avoid devices being duplicated */
	gst_device_provider_hide_provider(provider, "v4l2deviceprovider");
}

static void
gst_libcamera_provider_finalize(GObject *object)
{
	GstLibcameraProvider *self = GST_LIBCAMERA_PROVIDER(object);
	gpointer klass = gst_libcamera_provider_parent_class;

	delete self->cm;

	return G_OBJECT_GET_CLASS(klass)->finalize(object);
}

static void
gst_libcamera_provider_class_init(GstLibcameraProviderClass *klass)
{
	GstDeviceProviderClass *provider_class = (GstDeviceProviderClass *)klass;
	GObjectClass *object_class = (GObjectClass *)klass;

	provider_class->probe = gst_libcamera_provider_probe;
	object_class->finalize = gst_libcamera_provider_finalize;

	gst_device_provider_class_set_metadata(provider_class,
					       "LibCamera Device Provider",
					       "Source/Video",
					       "List camera device using LibCamera",
					       "Nicolas Dufresne <nicolas.dufresne@collabora.com>");
}
