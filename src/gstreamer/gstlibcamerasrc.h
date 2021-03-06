/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Collabora Ltd.
 *     Author: Nicolas Dufresne <nicolas.dufresne@collabora.com>
 *
 * gstlibcamerasrc.h - GStreamer Capture Element
 */

#include <gst/gst.h>

#ifndef __GST_LIBCAMERA_SRC_H__
#define __GST_LIBCAMERA_SRC_H__

G_BEGIN_DECLS

#define GST_TYPE_LIBCAMERA_SRC gst_libcamera_src_get_type()
G_DECLARE_FINAL_TYPE(GstLibcameraSrc, gst_libcamera_src,
		     GST_LIBCAMERA, SRC, GstElement)

G_END_DECLS

#endif /* __GST_LIBCAMERA_SRC_H__ */
