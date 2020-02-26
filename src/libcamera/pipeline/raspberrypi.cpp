/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * raspberrypi.cpp - Pipeline handler for Raspberry Pi devices
 */

#include <ipa/raspberrypi.h>

#include <libcamera/camera.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>

#include "camera_sensor.h"
#include "device_enumerator.h"
#include "ipa_manager.h"
#include "log.h"
#include "media_device.h"
#include "pipeline_handler.h"
#include "utils.h"
#include "v4l2_controls.h"
#include "v4l2_videodevice.h"

namespace libcamera {

LOG_DEFINE_CATEGORY(RPI)

class RPiCameraData : public CameraData
{
public:

	RPiCameraData(PipelineHandler *pipe)
		: CameraData(pipe), sensor_(nullptr), unicam_(nullptr),
                  isp_(nullptr)
        {
        }

        ~RPiCameraData()
        {
                delete sensor_;
        }

	void sensorReady(FrameBuffer *buffer);
	void ispOutputReady(FrameBuffer *buffer);
	void ispCaptureReady(FrameBuffer *buffer);

	int loadIPA();
	void queueFrameAction(unsigned int frame,
			      const IPAOperationData &action);

	void metadataReady(unsigned int frame, const ControlList &metadata);

	CameraSensor *sensor_;
	V4L2VideoDevice *unicam_;
	V4L2M2MDevice *isp_;
	Stream stream_;

	std::vector<std::unique_ptr<FrameBuffer>> rawBuffers_;
};

class RPiCameraConfiguration : public CameraConfiguration
{
public:
	RPiCameraConfiguration();

	Status validate() override;
};

class PipelineHandlerRPi : public PipelineHandler
{
public:
	PipelineHandlerRPi(CameraManager *manager);
	~PipelineHandlerRPi();

	CameraConfiguration *
	generateConfiguration(Camera *camera,
			      const StreamRoles &roles) override;
	int configure(Camera *camera,
		      CameraConfiguration *config) override;

	int exportFrameBuffers(Camera *camera, Stream *stream,
			       std::vector<std::unique_ptr<FrameBuffer>> *buffers) override;
	int importFrameBuffers(Camera *camera, Stream *stream) override;
	void freeFrameBuffers(Camera *camera, Stream *stream) override;

	int start(Camera *camera) override;
	void stop(Camera *camera) override;

	int queueRequestDevice(Camera *camera, Request *request) override;

	bool match(DeviceEnumerator *enumerator) override;

private:
	RPiCameraData *cameraData(const Camera *camera)
	{
		return static_cast<RPiCameraData *>(
			PipelineHandler::cameraData(camera));
	}

	std::shared_ptr<MediaDevice> unicam_;
	std::shared_ptr<MediaDevice> codec_;
};

RPiCameraConfiguration::RPiCameraConfiguration()
	: CameraConfiguration()
{
}

CameraConfiguration::Status RPiCameraConfiguration::validate()
{
	Status status = Valid;

	if (config_.empty())
		return Invalid;

	/* todo: Experiment with increased stream support through the ISP. */
	if (config_.size() > 1) {
		config_.resize(1);
		status = Adjusted;
	}

	StreamConfiguration &cfg = config_[0];

	/* todo: restrict to hardware capabilities. */

	cfg.bufferCount = 4;

	return status;
}

PipelineHandlerRPi::PipelineHandlerRPi(CameraManager *manager)
	: PipelineHandler(manager), unicam_(nullptr), codec_(nullptr)
{
}

PipelineHandlerRPi::~PipelineHandlerRPi()
{
	if (unicam_)
		unicam_->release();

	if (codec_)
		codec_->release();
}

CameraConfiguration *
PipelineHandlerRPi::generateConfiguration(Camera *camera,
					  const StreamRoles &roles)
{
	CameraConfiguration *config = new RPiCameraConfiguration();
	RPiCameraData *data = cameraData(camera);

	if (roles.empty())
		return config;

	StreamConfiguration cfg{};
	cfg.pixelFormat = V4L2_PIX_FMT_YUYV;
	cfg.size = { 1920, 1080 };

	LOG(RPI, Debug) << "Sensor Resolution is: " << data->sensor_->resolution().toString();

	cfg.size = { 320, 240 };

	cfg.bufferCount = 4;

	config->addConfiguration(cfg);

	config->validate();

	return config;
}

int PipelineHandlerRPi::configure(Camera *camera, CameraConfiguration *config)
{
	RPiCameraData *data = cameraData(camera);
	StreamConfiguration &cfg = config->at(0);
	uint32_t unicam_fourcc;
	int ret;

	Size sensorSize = { 1920, 1080 };
	Size outputSize = { 1920, 1088 };

	V4L2DeviceFormat format = {};
	format.size = sensorSize;

	LOG(RPI, Debug) << "Setting format to " << format.toString();

	ret = data->unicam_->setFormat(&format);
	if (ret)
		return ret;

	if (format.size != sensorSize) {
		LOG(RPI, Error)
			<< "Failed to set format on Video device: "
			<< format.toString();
		return -EINVAL;
	}

	format.size = outputSize;
	unicam_fourcc = format.fourcc;

	cfg.setStream(&data->stream_);

	return 0;
}

int PipelineHandlerRPi::exportFrameBuffers(Camera *camera, Stream *stream,
					      std::vector<std::unique_ptr<FrameBuffer>> *buffers)
{
	RPiCameraData *data = cameraData(camera);
	const StreamConfiguration &cfg = stream->configuration();
	int ret;
	unsigned int count = stream->configuration().bufferCount;


	/* Tie the unicam video buffers to the intermediate pool. */

	return data->unicam_->exportBuffers(count, buffers);
}

int PipelineHandlerRPi::importFrameBuffers(Camera *camera, Stream *stream)
{
	RPiCameraData *data = cameraData(camera);
	int ret;
	unsigned int count = stream->configuration().bufferCount;


	return data->unicam_->importBuffers(count);
}

void PipelineHandlerRPi::freeFrameBuffers(Camera *camera, Stream *stream)
{
	RPiCameraData *data = cameraData(camera);

	data->unicam_->releaseBuffers();
}

int PipelineHandlerRPi::start(Camera *camera)
{
	RPiCameraData *data = cameraData(camera);
	int ret;

	LOG(RPI, Warning) << "Using hard-coded exposure/gain defaults";

	ControlList controls(data->sensor_->controls());

	controls.set(V4L2_CID_EXPOSURE, 1700);
	controls.set(V4L2_CID_ANALOGUE_GAIN, 180);
	ret = data->sensor_->setControls(&controls);
	if (ret) {
		LOG(RPI, Error) << "Failed to set controls";
		return ret;
	}

	ret = data->unicam_->streamOn();
	if (ret)
		return ret;

	return 0;

}

void PipelineHandlerRPi::stop(Camera *camera)
{
	RPiCameraData *data = cameraData(camera);

	data->unicam_->streamOff();

	data->rawBuffers_.clear();
}

int PipelineHandlerRPi::queueRequestDevice(Camera *camera, Request *request)
{
	RPiCameraData *data = cameraData(camera);
	Stream *stream = &data->stream_;

	FrameBuffer *buffer = request->findBuffer(stream);
	if (!buffer) {
		LOG(RPI, Error)
			<< "Attempt to queue request with invalid stream";
		return -ENOENT;
	}

	/* Deliver the frame from the sensor */
        int ret = data->unicam_->queueBuffer(buffer);

        if (ret < 0)
        {
                LOG(RPI, Error) << "Unicam queue buffer error";
                return ret;
        }

	return 0;
}

bool PipelineHandlerRPi::match(DeviceEnumerator *enumerator)
{
	DeviceMatch unicam("unicam");
	DeviceMatch codec("bcm2835-codec");

	/* The video node is also named unicam. */
	unicam.add("unicam");

	/* We explicitly need the ISP device from the MMAL codec driver. */
	codec.add("bcm2835-codec-isp-source");

	unicam_ = enumerator->search(unicam);
	if (!unicam_)
		return false;

	codec_ = enumerator->search(codec);
	if (!codec_)
		return false;

	unicam_->acquire();
	codec_->acquire();

	std::unique_ptr<RPiCameraData> data = std::make_unique<RPiCameraData>(this);

	/* Locate and open the unicam video node. */
	data->unicam_ = new V4L2VideoDevice(unicam_->getEntityByName("unicam"));
	if (data->unicam_->open())
		return false;

	/* Locate the ISP M2M node */
	MediaEntity *isp = codec_->getEntityByName("bcm2835-codec-isp-source");
	if (!isp) {
		LOG(RPI, Error) << "Could not identify the ISP";
		return false;
	}

	data->unicam_->bufferReady.connect(data.get(), &RPiCameraData::sensorReady);

	/* Identify the sensor */
	for (MediaEntity *entity : unicam_->entities()) {
		if (entity->function() == MEDIA_ENT_F_CAM_SENSOR) {
			data->sensor_ = new CameraSensor(entity);
			break;
		}
	}

	if (!data->sensor_)
		return false;

	if (data->sensor_->init())
		return false;

	if (data->loadIPA()) {
		LOG(RPI, Error) << "Failed to load a suitable IPA library";
		return false;
	}

	/* Create and register the camera. */
	std::set<Stream *> streams{ &data->stream_ };
	std::shared_ptr<Camera> camera =
		Camera::create(this, data->sensor_->entity()->name(), streams);
	registerCamera(std::move(camera), std::move(data));

	return true;
}

void RPiCameraData::sensorReady(FrameBuffer *buffer)
{
	Request *request = buffer->request();

        pipe_->completeBuffer(camera_, request, buffer);
        pipe_->completeRequest(camera_, request);
}

void RPiCameraData::ispOutputReady(FrameBuffer *buffer)
{
	/* Return a completed buffer from the ISP back to the sensor. */
	unicam_->queueBuffer(buffer);
}

void RPiCameraData::ispCaptureReady(FrameBuffer *buffer)
{
	Request *request = buffer->request();

	pipe_->completeBuffer(camera_, request, buffer);
	pipe_->completeRequest(camera_, request);
}

int RPiCameraData::loadIPA()
{
	ipa_ = IPAManager::instance()->createIPA(pipe_, 1, 1);
	if (!ipa_)
		return -ENOENT;

	ipa_->queueFrameAction.connect(this,
				       &RPiCameraData::queueFrameAction);

	return 0;
}

void RPiCameraData::queueFrameAction(unsigned int frame,
				     const IPAOperationData &action)
{
	switch (action.operation) {
	case RPI_IPA_ACTION_V4L2_SET: {
#if 0 // disabled cargo-cult from RkISP1
		const ControlList &controls = action.controls[0];
		timeline_.scheduleAction(utils::make_unique<RkISP1ActionSetSensor>(frame,
										   sensor_,
										   controls));
#endif
		break;
	}
	case RPI_IPA_ACTION_PARAM_FILLED: {
#if 0 // disabled cargo-cult from RkISP1
		RPIFrameInfo *info = frameInfo_.find(frame);
		if (info)
			info->paramFilled = true;
#endif
		break;
	}
	case RPI_IPA_ACTION_METADATA:
		metadataReady(frame, action.controls[0]);
		break;
	default:
		LOG(RPI, Error) << "Unknown action " << action.operation;
		break;
	}
}

void RPiCameraData::metadataReady(unsigned int frame, const ControlList &metadata)
{
	LOG(RPI, Debug) << "Received some MetaData, but nothing I can do yet..";

#if 0 // disabled cargo-cult from RkISP1
	PipelineHandlerRPi *pipe = static_cast<PipelineHandlerRPi *>(pipe_);

	RkISP1FrameInfo *info = frameInfo_.find(frame);
	if (!info)
		return;

	info->request->metadata() = metadata;
	info->metadataProcessed = true;

	pipe->tryCompleteRequest(info->request);
#endif
}

REGISTER_PIPELINE_HANDLER(PipelineHandlerRPi);

} /* namespace libcamera */
