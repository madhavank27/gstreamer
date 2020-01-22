/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2020, Collabora Ltd.
 *     Author: Nicolas Dufresne <nicolas.dufresne@collabora.com>
 *
 * gstlibcamera-utils.c - GStreamer Libcamera Utility Function
 */

#include "gstlibcamera-utils.h"
#include <linux/drm_fourcc.h>

using namespace libcamera;

static struct {
	GstVideoFormat gst_format;
	guint drm_fourcc;
} format_map[] = {
	{ GST_VIDEO_FORMAT_ENCODED, DRM_FORMAT_MJPEG },
	{ GST_VIDEO_FORMAT_RGB, DRM_FORMAT_BGR888 },
	{ GST_VIDEO_FORMAT_BGR, DRM_FORMAT_RGB888 },
	{ GST_VIDEO_FORMAT_ARGB, DRM_FORMAT_BGRA8888 },
	{ GST_VIDEO_FORMAT_NV12, DRM_FORMAT_NV12 },
	{ GST_VIDEO_FORMAT_NV21, DRM_FORMAT_NV21 },
	{ GST_VIDEO_FORMAT_NV16, DRM_FORMAT_NV16 },
	{ GST_VIDEO_FORMAT_NV61, DRM_FORMAT_NV61 },
	{ GST_VIDEO_FORMAT_NV24, DRM_FORMAT_NV24 },
	/* { GST_VIDEO_FORMAT_NV42,    DRM_FORMAT_NV42 }, */
	{ GST_VIDEO_FORMAT_UYVY, DRM_FORMAT_UYVY },
	{ GST_VIDEO_FORMAT_VYUY, DRM_FORMAT_VYUY },
	{ GST_VIDEO_FORMAT_YUY2, DRM_FORMAT_YUYV },
	{ GST_VIDEO_FORMAT_YVYU, DRM_FORMAT_YVYU },
};
#define FORMAT_MAP_SIZE G_N_ELEMENTS(format_map)

static inline GstVideoFormat
drm_to_gst_format(guint drm_fourcc)
{
	for (guint i = 0; i < FORMAT_MAP_SIZE; i++)
		if (format_map[i].drm_fourcc == drm_fourcc)
			return format_map[i].gst_format;
	return GST_VIDEO_FORMAT_UNKNOWN;
}

static inline guint
gst_format_to_drm(GstVideoFormat gst_format)
{
	if (gst_format == GST_VIDEO_FORMAT_ENCODED)
		return DRM_FORMAT_INVALID;

	for (guint i = 0; i < FORMAT_MAP_SIZE; i++)
		if (format_map[i].gst_format == gst_format)
			return format_map[i].drm_fourcc;
	return DRM_FORMAT_INVALID;
}

static GstStructure *
bare_structure_from_fourcc(guint fourcc)
{
	GstVideoFormat gst_format = drm_to_gst_format(fourcc);

	if (gst_format == GST_VIDEO_FORMAT_UNKNOWN)
		return NULL;

	if (gst_format != GST_VIDEO_FORMAT_ENCODED)
		return gst_structure_new("video/x-raw", "format", G_TYPE_STRING,
					 gst_video_format_to_string(gst_format), NULL);

	switch (fourcc) {
	case DRM_FORMAT_MJPEG:
		return gst_structure_new_empty("image/jpeg");
	default:
		break;
	}

	return NULL;
}

GstCaps *
gst_libcamera_stream_formats_to_caps(const StreamFormats &formats)
{
	GstCaps *caps = gst_caps_new_empty();

	for (unsigned int fourcc : formats.pixelformats()) {
		g_autoptr(GstStructure) bare_s = bare_structure_from_fourcc(fourcc);

		if (!bare_s) {
			GST_WARNING("Unsupported DRM format %" GST_FOURCC_FORMAT,
				    GST_FOURCC_ARGS(fourcc));
			continue;
		}

		for (const Size &size : formats.sizes(fourcc)) {
			GstStructure *s = gst_structure_copy(bare_s);
			gst_structure_set(s,
					  "width", G_TYPE_INT, size.width,
					  "height", G_TYPE_INT, size.height,
					  NULL);
			gst_caps_append_structure(caps, s);
		}

		const SizeRange &range = formats.range(fourcc);
		if (range.hStep && range.vStep) {
			GstStructure *s = gst_structure_copy(bare_s);

			gst_structure_set(s,
				"width", GST_TYPE_INT_RANGE, range.min.width, range.max.width, range.hStep,
				"height", GST_TYPE_INT_RANGE, range.min.height, range.max.height, range.vStep,
				NULL);
			gst_caps_append_structure(caps, s);
		}
	}

	return caps;
}

GstCaps *
gst_libcamera_stream_configuration_to_caps(const StreamConfiguration &stream_cfg)
{
	GstCaps *caps = gst_caps_new_empty();
	GstStructure *s = bare_structure_from_fourcc(stream_cfg.pixelFormat);

	gst_structure_set(s,
			  "width", G_TYPE_INT, stream_cfg.size.width,
			  "height", G_TYPE_INT, stream_cfg.size.height,
			  NULL);
	gst_caps_append_structure(caps, s);

	return caps;
}

void
gst_libcamera_configure_stream_from_caps(StreamConfiguration &stream_cfg,
					 GstCaps *caps)
{
	GstVideoFormat gst_format = drm_to_gst_format(stream_cfg.pixelFormat);

	/* First fixate the caps using default configuration value */
	g_assert(gst_caps_is_writable(caps));
	caps = gst_caps_truncate(caps);
	GstStructure *s = gst_caps_get_structure(caps, 0);

	gst_structure_fixate_field_nearest_int(s, "width", stream_cfg.size.width);
	gst_structure_fixate_field_nearest_int(s, "height", stream_cfg.size.height);

	if (gst_structure_has_name(s, "video/x-raw")) {
		const gchar *format = gst_video_format_to_string(gst_format);
		gst_structure_fixate_field_string(s, "format", format);
	}

	/* The configure the stream with the result. */
	if (gst_structure_has_name(s, "video/x-raw")) {
		const gchar *format = gst_structure_get_string(s, "format");
		gst_format = gst_video_format_from_string(format);
		stream_cfg.pixelFormat = gst_format_to_drm(gst_format);
	} else if (gst_structure_has_name(s, "image/jpeg")) {
		stream_cfg.pixelFormat = DRM_FORMAT_MJPEG;
	} else {
		g_critical("Unsupported media type: %s", gst_structure_get_name(s));
	}

	gst_structure_get_int(s, "width", (gint *)&stream_cfg.size.width);
	gst_structure_get_int(s, "height", (gint *)&stream_cfg.size.height);
}
