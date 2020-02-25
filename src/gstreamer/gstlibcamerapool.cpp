/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2020, Collabora Ltd.
 *     Author: Nicolas Dufresne <nicolas.dufresne@collabora.com>
 *
 * gstlibcamerapool.cpp - GStreamer Buffer Pool
 */

#include "gstlibcamera-utils.h"
#include "gstlibcamerapool.h"

#include <libcamera/stream.h>

using namespace libcamera;

struct _GstLibcameraPool {
	GstBufferPool parent;

	GstAtomicQueue *queue;
	GstLibcameraAllocator *allocator;
	Stream *stream;
};

G_DEFINE_TYPE(GstLibcameraPool, gst_libcamera_pool, GST_TYPE_BUFFER_POOL);

static GstFlowReturn
gst_libcamera_pool_acquire_buffer(GstBufferPool *pool, GstBuffer **buffer,
				  GstBufferPoolAcquireParams *params)
{
	GstLibcameraPool *self = GST_LIBCAMERA_POOL(pool);
	GstBuffer *buf = GST_BUFFER(gst_atomic_queue_pop(self->queue));
	if (!buf)
		return GST_FLOW_ERROR;

	if (!gst_libcamera_allocator_prepare_buffer(self->allocator, self->stream, buf))
		return GST_FLOW_ERROR;

	*buffer = buf;
	return GST_FLOW_OK;
}

static void
gst_libcamera_pool_reset_buffer(GstBufferPool *pool, GstBuffer *buffer)
{
	GstBufferPoolClass *klass = GST_BUFFER_POOL_CLASS(gst_libcamera_pool_parent_class);

	/* Clears all the memories and only pool the GstBuffer objects */
	gst_buffer_remove_all_memory(buffer);
	klass->reset_buffer(pool, buffer);
	GST_BUFFER_FLAGS(buffer) = 0;
}

static void
gst_libcamera_pool_release_buffer(GstBufferPool *pool, GstBuffer *buffer)
{
	GstLibcameraPool *self = GST_LIBCAMERA_POOL(pool);
	gst_atomic_queue_push(self->queue, buffer);
}

static void
gst_libcamera_pool_init(GstLibcameraPool *self)
{
	self->queue = gst_atomic_queue_new(4);
}

static void
gst_libcamera_pool_finalize(GObject *object)
{
	GstLibcameraPool *self = GST_LIBCAMERA_POOL(object);
	GstBuffer *buf;

	while ((buf = GST_BUFFER(gst_atomic_queue_pop(self->queue))))
		gst_buffer_unref(buf);

	gst_atomic_queue_unref(self->queue);
	g_object_unref(self->allocator);

	G_OBJECT_CLASS(gst_libcamera_pool_parent_class)->finalize(object);
}

static void
gst_libcamera_pool_class_init(GstLibcameraPoolClass *klass)
{
	auto *object_class = G_OBJECT_CLASS(klass);
	auto *pool_class = GST_BUFFER_POOL_CLASS(klass);

	object_class->finalize = gst_libcamera_pool_finalize;
	pool_class->start = nullptr;
	pool_class->acquire_buffer = gst_libcamera_pool_acquire_buffer;
	pool_class->reset_buffer = gst_libcamera_pool_reset_buffer;
	pool_class->release_buffer = gst_libcamera_pool_release_buffer;
}

GstLibcameraPool *
gst_libcamera_pool_new(GstLibcameraAllocator *allocator, Stream *stream)
{
	auto *pool = GST_LIBCAMERA_POOL(g_object_new(GST_TYPE_LIBCAMERA_POOL, nullptr));

	pool->allocator = GST_LIBCAMERA_ALLOCATOR(g_object_ref(allocator));
	pool->stream = stream;

	gsize pool_size = gst_libcamera_allocator_get_pool_size(allocator, stream);
	for (gsize i = 0; i < pool_size; i++) {
		GstBuffer *buffer = gst_buffer_new();
		gst_atomic_queue_push(pool->queue, buffer);
	}

	return pool;
}
