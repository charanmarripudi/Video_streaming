#include "capture/stream_capture.hpp"
#include "capture/hardware_factory.hpp"
#include "recorder/event_manager.hpp"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>

namespace engine::capture {

StreamCapture::StreamCapture(CameraConfig config, 
                             int global_fps,
                             std::shared_ptr<LatestFrameSlot> latest_slot,
                             std::shared_ptr<buffer::PreEventRing> pre_event_ring,
                             std::shared_ptr<recorder::EventManager> event_mgr)
    : config_(std::move(config)), 
      effective_fps_(config_.get_effective_fps(global_fps)),
      latest_slot_(std::move(latest_slot)),
      pre_event_ring_(std::move(pre_event_ring)),
      event_mgr_(std::move(event_mgr)),
      supervisor_(std::make_unique<StreamSupervisor>(config_.id, config_.stall_timeout_s)) {}

StreamCapture::~StreamCapture() {
    stop();
}

void StreamCapture::start() {
    is_running_.store(true);
    supervisor_->on_pipeline_built();

    capture_thread_ = std::jthread([this](std::stop_token st) {
        capture_loop(st);
    });

    HardwareConfig hw;
    hw.platform = config_.decode_mode == "hardware" ? "rpi" : "software";
    std::string pipeline_str = HardwareFactory::build_gstreamer_capture_pipeline(config_, effective_fps_, hw);
    std::cout << "[CAPTURE][" << config_.id << "] Ingesting stream. Source Rate @ " 
              << effective_fps_ << " FPS (" << config_.capture_width << "x" << config_.capture_height << ")\n"
              << "[CAPTURE][" << config_.id << "] Pipeline: " << pipeline_str << std::endl;
}

void StreamCapture::request_stop() noexcept {
    if (capture_thread_.joinable()) {
        is_running_.store(false);
        capture_thread_.request_stop();
    }
}

void StreamCapture::join() {
    if (capture_thread_.joinable()) {
        capture_thread_.join();
        std::cout << "[CAPTURE][" << config_.id << "] Joined capture pipeline." << std::endl;
    }
}

void StreamCapture::stop() {
    request_stop();
    join();
}

void StreamCapture::capture_loop(std::stop_token st) {
    const int64_t frame_interval_ns = 1000000000LL / effective_fps_;
    int64_t current_pts_ns = 0;
    const size_t frame_bytes = static_cast<size_t>(config_.capture_width * config_.capture_height * 3);

    // Support live webcam / external pipe ingestion
    int pipe_fd = -1;
    bool is_webcam_mode = (config_.stream_url.rfind("webcam", 0) == 0 || 
                           config_.stream_url.find(".pipe") != std::string::npos);
    std::string pipe_path = "/tmp/video_engine_webcam.pipe";
    if (config_.stream_url.find(".pipe") != std::string::npos) {
        pipe_path = config_.stream_url;
    }

    while (!st.stop_requested()) {
        auto host_now = std::chrono::steady_clock::now();
        int64_t host_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(host_now.time_since_epoch()).count();
        current_pts_ns += frame_interval_ns;

        // 1. Notify supervisor of frame reception
        supervisor_->on_frame_received(current_pts_ns, host_ns);

        int64_t f_idx = ++frame_counter_;
        bool is_key = (f_idx % 30 == 1); // 1 keyframe every 30 frames (1-2s GOP)

        // 1. Decoded Frame: Read live pixels from webcam pipe (or fallback test pattern)
        auto frame = std::make_shared<Frame>();
        frame->frame_index = f_idx;
        frame->pts_ns = current_pts_ns;
        frame->host_ns = host_ns;
        frame->width = config_.capture_width;
        frame->height = config_.capture_height;
        frame->channels = 3;

        auto buffer_data = std::make_shared<std::vector<uint8_t>>(frame_bytes);
        bool read_from_pipe = false;

        if (is_webcam_mode) {
            if (pipe_fd < 0) {
                pipe_fd = open(pipe_path.c_str(), O_RDONLY | O_NONBLOCK);
                if (pipe_fd >= 0) {
                    std::cout << "[CAPTURE][" << config_.id << "] Connected to live webcam pipe: " << pipe_path << std::endl;
                }
            }
            if (pipe_fd >= 0) {
                struct pollfd pfd{};
                pfd.fd = pipe_fd;
                pfd.events = POLLIN;
                int pret = poll(&pfd, 1, 60);
                if (pret > 0 && (pfd.revents & POLLIN)) {
                    size_t total_read = 0;
                    while (total_read < frame_bytes && !st.stop_requested()) {
                        ssize_t n = read(pipe_fd, buffer_data->data() + total_read, frame_bytes - total_read);
                        if (n > 0) {
                            total_read += n;
                        } else if (n == 0) {
                            close(pipe_fd);
                            pipe_fd = -1;
                            break;
                        } else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                std::this_thread::sleep_for(std::chrono::microseconds(500));
                                continue;
                            }
                            break;
                        }
                    }
                    if (total_read == frame_bytes) {
                        read_from_pipe = true;
                    }
                }
            }
        }

        if (!read_from_pipe) {
            // Simulated dynamic BGR frame pattern (fallback)
            uint8_t r = static_cast<uint8_t>((f_idx * 4) % 256);
            uint8_t g = static_cast<uint8_t>(120);
            uint8_t b = static_cast<uint8_t>(200);
            for (size_t p = 0; p < frame_bytes; p += 3) {
                (*buffer_data)[p] = b;
                (*buffer_data)[p + 1] = g;
                (*buffer_data)[p + 2] = r;
            }
        }

        frame->data = buffer_data;
        latest_slot_->publish(frame);

        // 2. Encoded Tee: Push REAL frame into Pre-Event Ring and Event Manager
        EncodedSample sample;
        sample.pts_ns = current_pts_ns;
        sample.dts_ns = current_pts_ns;
        sample.is_keyframe = is_key;
        sample.payload = buffer_data; // Store actual webcam frame!

        if (pre_event_ring_) {
            pre_event_ring_->push(sample);
        }

        // Notify EventManager for post-event accumulation on open clips
        if (event_mgr_) {
            event_mgr_->on_encoded_sample(sample);
        }

        // Periodic log & watchdog health check
        if (f_idx % (effective_fps_ * 5) == 0) {
            supervisor_->check_watchdog();
            std::cout << "[CAPTURE][" << config_.id << "] Ingested frame #" << f_idx 
                      << " [" << (read_from_pipe ? "LIVE WEBCAM" : "TEST PATTERN") << "]"
                      << " (Ring Bytes: " << (pre_event_ring_->current_bytes() / 1024) 
                      << " KB, State: " << stream_state_to_string(supervisor_->current_state()) << ")" << std::endl;
        }

        if (!read_from_pipe) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(frame_interval_ns));
        }
    }

    if (pipe_fd >= 0) {
        close(pipe_fd);
    }
}

} // namespace engine::capture
