/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2018, Google Inc.
 *
 * pipeline_handler.h - Pipeline handler infrastructure
 */
#ifndef __LIBCAMERA_PIPELINE_HANDLER_H__
#define __LIBCAMERA_PIPELINE_HANDLER_H__

#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <sys/sysmacros.h>
#include <vector>

#include <ipa/ipa_interface.h>
#include <libcamera/controls.h>
#include <libcamera/object.h>
#include <libcamera/stream.h>

namespace libcamera {

class Camera;
class CameraConfiguration;
class CameraManager;
class DeviceEnumerator;
class DeviceMatch;
class FrameBuffer;
class MediaDevice;
class PipelineHandler;
class Request;

class CameraData
{
public:
	explicit CameraData(PipelineHandler *pipe)
		: pipe_(pipe)
	{
	}
	virtual ~CameraData() {}

	Camera *camera_;
	PipelineHandler *pipe_;
	std::list<Request *> queuedRequests_;
	ControlInfoMap controlInfo_;
	std::unique_ptr<IPAInterface> ipa_;

private:
	CameraData(const CameraData &) = delete;
	CameraData &operator=(const CameraData &) = delete;
};

class PipelineHandler : public std::enable_shared_from_this<PipelineHandler>,
			public Object
{
public:
	PipelineHandler(CameraManager *manager);
	virtual ~PipelineHandler();

	virtual bool match(DeviceEnumerator *enumerator) = 0;
	MediaDevice *acquireMediaDevice(DeviceEnumerator *enumerator,
					const DeviceMatch &dm);

	bool lock();
	void unlock();

	const ControlInfoMap &controls(Camera *camera);

	virtual CameraConfiguration *generateConfiguration(Camera *camera,
		const StreamRoles &roles) = 0;
	virtual int configure(Camera *camera, CameraConfiguration *config) = 0;

	virtual int exportFrameBuffers(Camera *camera, Stream *stream,
				       std::vector<std::unique_ptr<FrameBuffer>> *buffers) = 0;
	virtual int importFrameBuffers(Camera *camera, Stream *stream) = 0;
	virtual void freeFrameBuffers(Camera *camera, Stream *stream) = 0;

	virtual int start(Camera *camera) = 0;
	virtual void stop(Camera *camera) = 0;

	int queueRequest(Camera *camera, Request *request);

	bool completeBuffer(Camera *camera, Request *request,
			    FrameBuffer *buffer);
	void completeRequest(Camera *camera, Request *request);

	const char *name() const { return name_; }

protected:
	void registerCamera(std::shared_ptr<Camera> camera,
			    std::unique_ptr<CameraData> data, dev_t devnum = 0);
	void hotplugMediaDevice(MediaDevice *media);

	virtual int queueRequestDevice(Camera *camera, Request *request) = 0;

	CameraData *cameraData(const Camera *camera);

	CameraManager *manager_;

private:
	void mediaDeviceDisconnected(MediaDevice *media);
	virtual void disconnect();

	std::vector<std::shared_ptr<MediaDevice>> mediaDevices_;
	std::vector<std::weak_ptr<Camera>> cameras_;
	std::map<const Camera *, std::unique_ptr<CameraData>> cameraData_;

	const char *name_;

	friend class PipelineHandlerFactory;
};

class PipelineHandlerFactory
{
public:
	PipelineHandlerFactory(const char *name);
	virtual ~PipelineHandlerFactory() {}

	std::shared_ptr<PipelineHandler> create(CameraManager *manager);

	const std::string &name() const { return name_; }

	static void registerType(PipelineHandlerFactory *factory);
	static std::vector<PipelineHandlerFactory *> &factories();

private:
	virtual PipelineHandler *createInstance(CameraManager *manager) = 0;

	std::string name_;
};

#define REGISTER_PIPELINE_HANDLER(handler)				\
class handler##Factory final : public PipelineHandlerFactory		\
{									\
public:									\
	handler##Factory() : PipelineHandlerFactory(#handler) {}	\
									\
private:								\
	PipelineHandler *createInstance(CameraManager *manager)		\
	{								\
		return new handler(manager);				\
	}								\
};									\
static handler##Factory global_##handler##Factory;

} /* namespace libcamera */

#endif /* __LIBCAMERA_PIPELINE_HANDLER_H__ */
