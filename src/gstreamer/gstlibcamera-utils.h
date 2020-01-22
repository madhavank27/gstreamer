/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2020, Collabora Ltd.
 *     Author: Nicolas Dufresne <nicolas.dufresne@collabora.com>
 *
 * gstlibcamera-utils.h - GStreamer Libcamera Utility Functions
 */

#include <gst/gst.h>
#include <gst/video/video.h>

#include <libcamera/stream.h>

#ifndef __GST_LIBCAMERA_UTILS_H_

#define GST_OBJECT_LOCKER(obj) g_autoptr(GMutexLocker) locker = g_mutex_locker_new(GST_OBJECT_GET_LOCK(obj))

GstCaps *gst_libcamera_stream_formats_to_caps(const libcamera::StreamFormats &formats);
GstCaps *gst_libcamera_stream_configuration_to_caps(const libcamera::StreamConfiguration &stream_cfg);
void gst_libcamera_configure_stream_from_caps(libcamera::StreamConfiguration &stream_cfg,
					      GstCaps *caps);

#endif /* __GST_LIBCAMERA_UTILS_H_ */
