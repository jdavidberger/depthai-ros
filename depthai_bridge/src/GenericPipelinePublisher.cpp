#include <dynamic_reconfigure/server.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/Imu.h>

#include <vision_msgs/BoundingBox2D.h>

#include <depthai_bridge/BridgePublisher.hpp>
#include <depthai_bridge/DisparityConverter.hpp>
#include <depthai_bridge/GenericPipelinePublisher.hpp>
#include <depthai_bridge/ImageConverter.hpp>
#include <depthai_bridge/ImuConverter.hpp>

#include "depthai_ros_msgs/StereoDepthConfig.h"
#include "depthai_ros_msgs/CameraControlConfig.h"

namespace dai {
namespace ros {
void GenericPipelinePublisher::BuildPublisherFromPipeline(dai::Pipeline& pipeline) {
    auto connections = pipeline.getConnectionMap();
    if(!_device.isPipelineRunning()) {
        for(auto& node : pipeline.getAllNodes()) {
            addConfigNodes(pipeline, node);
        }

        _device.startPipeline(pipeline);

        for(auto& node : pipeline.getAllNodes()) {
            mapNode(pipeline, node);
        }
    } else {
        ROS_WARN("Device is running already, GenericPipelinePublisher can not add configuration servers");
    }

    for(auto& connection : connections) {
        auto node = pipeline.getNode(connection.first);
        if(auto xlinkOut = std::dynamic_pointer_cast<dai::node::XLinkOut>(node)) {
            for(auto& nodeConnection : connection.second) {
                auto otherNode = pipeline.getNode(nodeConnection.outputId);
                mapOutputStream(pipeline, xlinkOut, nodeConnection);
            }
        }
    }
}
GenericPipelinePublisher::GenericPipelinePublisher(::ros::NodeHandle& pnh,
                                                   dai::Device& device,
                                                   dai::Pipeline& pipeline,
                                                   const std::map<CameraBoardSocket, std::string>& frameNames,
                                                   const std::string& frame_prefix)
    : _pnh(pnh), _device(device), _frameNames(frameNames), _frame_prefix(frame_prefix) {
    _calibrationHandler = device.readCalibration();
    BuildPublisherFromPipeline(pipeline);
}

GenericPipelinePublisher::GenericPipelinePublisher(::ros::NodeHandle& pnh, dai::Device& device, dai::Pipeline& pipeline)
    : GenericPipelinePublisher(pnh, device, pipeline, "dai_" + device.getMxId()) {}
static std::map<CameraBoardSocket, std::string> default_frame_mapping() {
    std::map<CameraBoardSocket, std::string> frameNames;
    for(int i = (int)CameraBoardSocket::CAM_D; i < 8; i++) {
        auto name = std::string("CAM_") + std::to_string(i + 'A');
        frameNames[(CameraBoardSocket)i] = name;
    }
    frameNames[CameraBoardSocket::LEFT] = "left_camera_optical_frame";
    frameNames[CameraBoardSocket::RIGHT] = "right_camera_optical_frame";
    frameNames[CameraBoardSocket::RGB] = "rgb_camera_optical_frame";
    return frameNames;
}
GenericPipelinePublisher::GenericPipelinePublisher(::ros::NodeHandle& pnh, dai::Device& device, dai::Pipeline& pipeline, const std::string& frame_prefix)
    : GenericPipelinePublisher(pnh, device, pipeline, default_frame_mapping(), frame_prefix) {}

void GenericPipelinePublisher::mapOutputStream(Pipeline& pipeline, std::shared_ptr<dai::node::XLinkOut> xLinkOut, const Node::Connection& connection) {
    auto otherNode = pipeline.getNode(connection.outputId);
    if(!mapKnownInputNodeTypes<dai::node::ColorCamera, dai::node::IMU, dai::node::StereoDepth, dai::node::MonoCamera>(xLinkOut, otherNode, connection.outputName)) {
        ROS_WARN("Could not generate depthai publisher for %s(%s.%s)", xLinkOut->getStreamName().c_str(), otherNode->getName(), connection.outputName.c_str());
    }
}

bool GenericPipelinePublisher::mapKnownInputNodeType(std::shared_ptr<dai::node::XLinkOut> xLinkOut,
                                                     std::shared_ptr<dai::node::StereoDepth> stereo,
                                                     const std::string& inputName) {
    auto queue = _device.getOutputQueue(xLinkOut->getStreamName(), 30, false);
    auto alignSocket = stereo->properties.depthAlignCamera;
    if(alignSocket == CameraBoardSocket::AUTO) {
        alignSocket = CameraBoardSocket::RIGHT;
    }
    auto frame = _frameNames[alignSocket];

    std::shared_ptr<dai::rosBridge::ImageConverter> converter;
    std::shared_ptr<BridgePublisherBase> publisher;

    auto depthCameraInfo = ImageConverter::calibrationToCameraInfo(_calibrationHandler, alignSocket, 1280, 720);
    if(inputName == "depth") {
        converter = std::make_shared<ImageConverter>(_frame_prefix + frame, true);
        publisher = std::make_shared<dai::rosBridge::BridgePublisher<sensor_msgs::Image, dai::ImgFrame>>(
            queue,
            _pnh,
            std::string("stereo/depth"),
            std::bind(&dai::rosBridge::ImageConverter::toRosMsg,
                      converter.get(),  // since the converter has the same frame name
                      // and image type is also same we can reuse it
                      std::placeholders::_1,
                      std::placeholders::_2),
            30,
            depthCameraInfo,
            "stereo");
    } else if(inputName == "disparity") {
        auto converter =
                std::make_shared<dai::rosBridge::DisparityConverter>(_frame_prefix + frame, 880, 7.5, 20,
                                                                     2000);  // TODO(sachin): undo hardcoding of baseline
        publisher = std::make_shared<dai::rosBridge::BridgePublisher<stereo_msgs::DisparityImage, dai::ImgFrame>>(
                queue,
                _pnh,
                std::string("stereo/disparity"),
                std::bind(&dai::rosBridge::DisparityConverter::toRosMsg, converter.get(), std::placeholders::_1,
                          std::placeholders::_2),
                30,
                depthCameraInfo,
                "stereo");
        keep_alive.push_back(converter);
    } else if(inputName == "confidenceMap") {
        converter = std::make_shared<ImageConverter>(_frame_prefix + frame, true);
        publisher = std::make_shared<dai::rosBridge::BridgePublisher<sensor_msgs::Image, dai::ImgFrame>>(
                queue,
                _pnh,
                std::string("stereo/confidenceMap"),
                std::bind(&dai::rosBridge::ImageConverter::toRosMsg,
                          converter.get(),  // since the converter has the same frame name
                        // and image type is also same we can reuse it
                          std::placeholders::_1,
                          std::placeholders::_2),
                30,
                depthCameraInfo,
                "stereo");
    } else if(inputName == "rectifiedLeft" || inputName == "rectifiedRight" || inputName == "syncedRight" || inputName == "syncedLeft") {
        std::string side_name = (inputName == "rectifiedLeft" || inputName == "syncedLeft") ? "left" : "right";
        auto socket = side_name == "left" ? CameraBoardSocket::LEFT : CameraBoardSocket::RIGHT;
        converter = std::make_shared<dai::rosBridge::ImageConverter>(_frame_prefix + "_" + side_name + "_camera_optical_frame", true);
        std::string pub_name = side_name + ((inputName == "rectifiedLeft" || inputName == "rectifiedRight") ? "/image_rect" : "/image_raw");

        int monoWidth = 0, monoHeight = 0;
        auto pipeline = stereo->getParentPipeline();
        auto connections = pipeline.getConnectionMap();
        auto stereoConnections = connections[stereo->id];
        std::shared_ptr<dai::node::MonoCamera> monoNode;
        for(auto& connection : stereoConnections) {
            if(connection.inputName == side_name) {
                monoNode = std::dynamic_pointer_cast<dai::node::MonoCamera>(pipeline.getNode(connection.outputId));
            }
        }

        if(!monoNode) {
            ROS_WARN("Could not get input source for %s on stereo node", side_name.c_str());
            return true;
        }

        auto cameraInfo = ImageConverter::calibrationToCameraInfo(
            _calibrationHandler, monoNode->getBoardSocket(), monoNode->getResolutionWidth(), monoNode->getResolutionHeight());
        publisher = std::make_shared<dai::rosBridge::BridgePublisher<sensor_msgs::Image, dai::ImgFrame>>(
            queue,
            _pnh,
            pub_name,
            std::bind(&dai::rosBridge::ImageConverter::toRosMsg, converter.get(), std::placeholders::_1, std::placeholders::_2),
            30,
            cameraInfo,
            side_name);
    } else {
        ROS_WARN("Don't understand output named %s in StereoDepth", inputName.c_str());
    }
    if(converter) {
        keep_alive.push_back(converter);
    }
    if(publisher) {
        publisher->addPublisherCallback();
        publishers.emplace_back(publisher);
    }
    return true;
}
bool GenericPipelinePublisher::mapKnownInputNodeType(std::shared_ptr<dai::node::XLinkOut> xLinkOut,
                                                     std::shared_ptr<dai::node::IMU> inputNode,
                                                     const std::string& inputName) {
    auto imuConverter = std::make_shared<dai::rosBridge::ImuConverter>(_frame_prefix + "_imu_frame");
    auto queue = _device.getOutputQueue(xLinkOut->getStreamName(), 30, false);
    keep_alive.push_back(imuConverter);

    auto ImuPublish = std::make_shared<dai::rosBridge::BridgePublisher<sensor_msgs::Imu, dai::IMUData>>(
        queue,
        _pnh,
        std::string("imu"),
        std::bind(&dai::rosBridge::ImuConverter::toRosMsg, imuConverter.get(), std::placeholders::_1, std::placeholders::_2),
        30,
        "",
        "imu");

    ImuPublish->addPublisherCallback();
    keep_alive.push_back(ImuPublish);

    return true;
}

bool GenericPipelinePublisher::mapKnownInputNodeType(std::shared_ptr<dai::node::XLinkOut> xLinkOut, std::shared_ptr<dai::node::MonoCamera> inputNode, const std::string& inputName) {
    auto frame = _frameNames[inputNode->getBoardSocket()];
    auto queue = _device.getOutputQueue(xLinkOut->getStreamName(), 30, false);


    int width = 1280, height = 720;
    width = inputNode->getResolutionWidth();
    height = inputNode->getResolutionHeight();

    converters.emplace_back(std::make_shared<ImageConverter>(_frame_prefix + frame, true));
    auto& converter = converters.back();
    auto cameraInfo = converter->calibrationToCameraInfo(_calibrationHandler, inputNode->getBoardSocket(), width, height);
    auto publisher = std::make_shared<dai::rosBridge::BridgePublisher<sensor_msgs::Image, dai::ImgFrame>>(
            queue,
            _pnh,
            std::string(frame + "/image"),
            std::bind(&dai::rosBridge::ImageConverter::toRosMsg,
                      converter.get(),  // since the converter has the same frame name
                    // and image type is also same we can reuse it
                      std::placeholders::_1,
                      std::placeholders::_2),
            30,
            cameraInfo,
            "mono" + std::to_string((int)inputNode->getBoardSocket()));
    publishers.push_back(publisher);

    publisher->addPublisherCallback();
    return true;
}
bool GenericPipelinePublisher::mapKnownInputNodeType(std::shared_ptr<dai::node::XLinkOut> xLinkOut,
                                                     std::shared_ptr<dai::node::ColorCamera> inputNode,
                                                     const std::string& inputName) {
    auto frame = _frameNames[inputNode->getBoardSocket()];
    auto queue = _device.getOutputQueue(xLinkOut->getStreamName(), 30, false);

    converters.emplace_back(std::make_shared<ImageConverter>(_frame_prefix + frame, true));
    auto& rgbConverter = converters.back();

    int width = 1280, height = 720;
    if(inputName == "video") {
        width = inputNode->getVideoWidth();
        height = inputNode->getVideoHeight();
    } else if(inputName == "still") {
        width = inputNode->getStillWidth();
        height = inputNode->getStillHeight();
    } else if(inputName == "preview") {
        width = inputNode->getPreviewWidth();
        height = inputNode->getPreviewHeight();
    } else if(inputName == "isp") {
        height = inputNode->getIspHeight();
        width = inputNode->getIspWidth();
    } else {
        ROS_WARN("Don't understand output named %s in ColorCamera. Using default image size for intrinsics", inputName.c_str());
    }

    auto rgbCameraInfo = rgbConverter->calibrationToCameraInfo(_calibrationHandler, inputNode->getBoardSocket(), width, height);
    auto publisher = std::make_shared<dai::rosBridge::BridgePublisher<sensor_msgs::Image, dai::ImgFrame>>(
        queue,
        _pnh,
        std::string("color/image"),
        std::bind(&dai::rosBridge::ImageConverter::toRosMsg,
                  rgbConverter.get(),  // since the converter has the same frame name
                                       // and image type is also same we can reuse it
                  std::placeholders::_1,
                  std::placeholders::_2),
        30,
        rgbCameraInfo,
        "color");
    publishers.push_back(publisher);

    publisher->addPublisherCallback();
    return true;
}

template<typename T> void GenericPipelinePublisher::setupCameraControlQueue(std::shared_ptr<T> cam, const std::string& prefix) {
    auto configIn = cam->getParentPipeline().template create<dai::node::XLinkIn>();
    auto name = prefix + std::to_string((int)cam->getBoardSocket());
    configIn->setStreamName(name + "_inputControl");
    configIn->out.link(cam->inputControl);
}

template <typename T>
void GenericPipelinePublisher::setupCameraControlServer(std::shared_ptr<T> cam, const std::string& prefix) {
    auto name = prefix + std::to_string((int)cam->getBoardSocket());
    auto configQueue = _device.getInputQueue(name + "_inputControl");
    auto n = getNodeHandle(cam->getBoardSocket());
    auto server = std::make_shared<dynamic_reconfigure::Server<depthai_ros::CameraControlConfig>>(n);

    auto current_config = std::make_shared<depthai_ros::CameraControlConfig>();
    server->getConfigDefault(*current_config);
    keep_alive.push_back(current_config);

    auto triggger_update = [=](depthai_ros::CameraControlConfig& cfg, unsigned level) {
        dai::CameraControl dcfg;
        if(level == 0xffffffff || level & 7) dcfg.setStartStreaming();
        if(level & 1) dcfg.setAutoFocusMode(static_cast<CameraControl::AutoFocusMode>(cfg.autofocus_mode));
        if(level & 2) dcfg.setAutoFocusRegion(cfg.autofocus_startx, cfg.autofocus_starty, cfg.autofocus_width, cfg.autofocus_height);
        if(level & 2) dcfg.setAutoFocusLensRange(cfg.autofocus_min, cfg.autofocus_max);
        if(level & 4) dcfg.setManualFocus(cfg.manual_focus);
        if(level & 8) dcfg.setAutoExposureLock(cfg.autoexposure_lock);
        if(level & 16) dcfg.setAutoExposureRegion(cfg.autoexposure_startx, cfg.autoexposure_starty, cfg.autoexposure_width, cfg.autoexposure_height);

        if(level & 32) dcfg.setAutoExposureCompensation(cfg.autoexposure_compensation);
        if(level & 64) dcfg.setContrast(cfg.contrast);
        if(level & 128) dcfg.setBrightness(cfg.brightness);
        if(level & 256) dcfg.setSaturation(cfg.saturation);
        if(level & 512) dcfg.setSharpness(cfg.sharpness);
        if(level & 1024) dcfg.setChromaDenoise(cfg.chroma_denoise);

        *current_config = cfg;
        configQueue->send(dcfg);
    };

    server->setCallback([=](depthai_ros::CameraControlConfig& cfg, unsigned level) {
        triggger_update(cfg, level);
    });

    auto ae_subscriber = std::make_shared<::ros::Subscriber>
            (_pnh.subscribe<vision_msgs::BoundingBox2D>(
                    name + "/ae_bbox", 1,
                    [=](boost::shared_ptr<const vision_msgs::BoundingBox2D> bb){
                        current_config->autoexposure_startx = bb->center.x - bb->size_x / 2;
                        current_config->autoexposure_starty = bb->center.y - bb->size_y / 2;
                        current_config->autoexposure_width = bb->size_x;
                        current_config->autoexposure_height = bb->size_y;
                        server->updateConfig(*current_config);
                        triggger_update(*current_config, 16);
                    }
            ));
    keep_alive.push_back(ae_subscriber);

    auto af_subscriber = std::make_shared<::ros::Subscriber>
            (_pnh.subscribe<vision_msgs::BoundingBox2D>(
                    name + "/af_bbox", 1,
                    [=](boost::shared_ptr<const vision_msgs::BoundingBox2D> bb){
                        current_config->autofocus_startx = bb->center.x - bb->size_x / 2;
                        current_config->autofocus_starty = bb->center.y - bb->size_y / 2;
                        current_config->autofocus_width = bb->size_x;
                        current_config->autofocus_height = bb->size_y;
                        server->updateConfig(*current_config);
                        triggger_update(*current_config, 2);
                    }
            ));
    keep_alive.push_back(af_subscriber);

    keep_alive.push_back(server);
}

void GenericPipelinePublisher::mapNode(Pipeline& pipeline, std::shared_ptr<dai::Node> node) {
    if(auto stereo = std::dynamic_pointer_cast<dai::node::StereoDepth>(node)) {
        auto configQueue = _device.getInputQueue("stereoConfig");
        auto server = std::make_shared<dynamic_reconfigure::Server<depthai_ros::StereoDepthConfig>>(_pnh);
        depthai_ros::StereoDepthConfig def_config = { };
        def_config.left_right_check = stereo->initialConfig.getLeftRightCheckThreshold() > 0; // No getter for this; just check threshold??
        def_config.confidence = stereo->initialConfig.getConfidenceThreshold();
        def_config.bilateral_sigma = stereo->initialConfig.getBilateralFilterSigma();
        def_config.extended_disparity = stereo->initialConfig.get().algorithmControl.enableExtended;
        def_config.subpixel = stereo->initialConfig.get().algorithmControl.enableSubpixel;
        def_config.lr_check_threshold = stereo->initialConfig.getLeftRightCheckThreshold();
        server->setConfigDefault(def_config);

        server->setCallback([configQueue, stereo](depthai_ros::StereoDepthConfig& cfg, unsigned level) {
            dai::StereoDepthConfig dcfg = stereo->initialConfig;
            auto rawCfg = dcfg.get();
            rawCfg.postProcessing.thresholdFilter.maxRange = cfg.threshold_max;
            rawCfg.postProcessing.thresholdFilter.minRange = cfg.threshold_min;
            //rawCfg.postProcessing.decimationFilter.decimationFactor = cfg.decimation_factor;
            //rawCfg.postProcessing.decimationFilter.decimationMode = static_cast<RawStereoDepthConfig::PostProcessing::DecimationFilter::DecimationMode>(cfg.decimation_mode);
            dcfg.set(rawCfg);

            dcfg.setConfidenceThreshold(cfg.confidence);
            dcfg.setLeftRightCheckThreshold(cfg.lr_check_threshold);
            dcfg.setBilateralFilterSigma(cfg.bilateral_sigma);
            dcfg.setSubpixel(cfg.subpixel);
            dcfg.setLeftRightCheck(cfg.left_right_check);
            dcfg.setExtendedDisparity(cfg.extended_disparity);

            configQueue->send(dcfg);
        });
        keep_alive.push_back(server);
    }

    else if(auto rgb = std::dynamic_pointer_cast<dai::node::ColorCamera>(node)) {
        setupCameraControlServer(rgb, "rgb");
    }
    else if(auto mono = std::dynamic_pointer_cast<dai::node::MonoCamera>(node)) {
        setupCameraControlServer(mono, "mono");
    }
}

void GenericPipelinePublisher::addConfigNodes(Pipeline& pipeline, std::shared_ptr<dai::Node> node) {
    if(auto stereo = std::dynamic_pointer_cast<dai::node::StereoDepth>(node)) {
        auto configIn = pipeline.create<dai::node::XLinkIn>();
        configIn->setStreamName("stereoConfig");
        configIn->out.link(stereo->inputConfig);
    } else if(auto rgb = std::dynamic_pointer_cast<dai::node::ColorCamera>(node)) {
        setupCameraControlQueue(rgb, "rgb");
    }
    else if(auto mono = std::dynamic_pointer_cast<dai::node::MonoCamera>(node)) {
        setupCameraControlQueue(mono, "mono");
    }
}
}  // namespace ros
}  // namespace dai