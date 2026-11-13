#ifndef CAPTURE_HARDWARE_FACTORY_HPP
#define CAPTURE_HARDWARE_FACTORY_HPP

#include <string>
#include <sstream>
#include "common/types.hpp"

namespace engine::capture {

class HardwareFactory {
public:
    static std::string build_gstreamer_capture_pipeline(const CameraConfig& cam, 
                                                        int effective_fps,
                                                        const HardwareConfig& hw) 
    {
        std::ostringstream ss;

        if (hw.platform == "rpi") {
            // Raspberry Pi Hardware Accelerated Capture (VideoCore V4L2)
            ss << "rtspsrc location=\"" << cam.stream_url << "\" protocols=tcp latency=0 timeout=10000000 do-retransmission=true ! "
               << "rtph264depay ! h264parse ! "
               << "v4l2h264dec capture-io-mode=4 ! "
               << "v4l2convert ! "
               << "video/x-raw,width=" << cam.capture_width << ",height=" << cam.capture_height << ",format=BGR ! "
               << "videorate ! video/x-raw,framerate=" << effective_fps << "/1 ! "
               << "appsink name=sink drop=true max-buffers=1";
        } 
        else if (hw.platform == "jetson") {
            // NVIDIA Jetson Hardware Accelerated Capture (NVMM GPU)
            ss << "rtspsrc location=\"" << cam.stream_url << "\" protocols=tcp latency=0 timeout=10000000 ! "
               << "rtph264depay ! h264parse ! "
               << "nvv4l2decoder enable-max-performance=1 ! "
               << "nvvidconv ! video/x-raw(memory:NVMM),width=" << cam.capture_width << ",height=" << cam.capture_height << " ! "
               << "nvvidconv ! video/x-raw,format=BGRx ! "
               << "videorate ! video/x-raw,framerate=" << effective_fps << "/1 ! "
               << "appsink name=sink drop=true max-buffers=1";
        } 
        else {
            // Cross-Platform Software Fallback (Mac / Generic x86 Linux)
            ss << "rtspsrc location=\"" << cam.stream_url << "\" protocols=tcp latency=0 timeout=10000000 do-retransmission=true ! "
               << "rtph264depay ! h264parse ! avdec_h264 ! "
               << "videoconvert ! videoscale ! videorate ! "
               << "video/x-raw,width=" << cam.capture_width << ",height=" << cam.capture_height 
               << ",framerate=" << effective_fps << "/1,format=BGR ! "
               << "appsink name=sink drop=true max-buffers=1";
        }
        return ss.str();
    }

    static std::string build_gstreamer_recorder_pipeline(const std::string& output_path,
                                                         int width, int height, int fps,
                                                         int bitrate_kbps,
                                                         const HardwareConfig& hw) 
    {
        std::ostringstream ss;
        if (hw.platform == "rpi") {
            // Raspberry Pi Hardware H.264 Encoder
            ss << "appsrc name=eventsrc is-live=false block=true format=time "
               << "caps=video/x-raw,format=BGR,width=" << width << ",height=" << height 
               << ",framerate=" << fps << "/1 ! "
               << "v4l2convert ! "
               << "v4l2h264enc extra-controls=\"controls,video_bitrate=" << (bitrate_kbps * 1000) << "\" ! "
               << "h264parse ! mp4mux ! filesink location=\"" << output_path << "\"";
        } else {
            // Software x264enc (Low-latency preset)
            ss << "appsrc name=eventsrc is-live=false block=true format=time "
               << "caps=video/x-raw,format=BGR,width=" << width << ",height=" << height 
               << ",framerate=" << fps << "/1 ! "
               << "videoconvert ! "
               << "x264enc bitrate=" << bitrate_kbps << " speed-preset=ultrafast tune=zerolatency ! "
               << "h264parse ! mp4mux ! filesink location=\"" << output_path << "\"";
        }
        return ss.str();
    }
};

} // namespace engine::capture

#endif // CAPTURE_HARDWARE_FACTORY_HPP

