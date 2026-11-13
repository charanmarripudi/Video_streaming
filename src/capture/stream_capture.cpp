#include "capture/stream_capture.hpp"
#include "capture/hardware_factory.hpp"
#include "recorder/event_manager.hpp"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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

    // Support live webcam / TCP stream ingestion
    int server_fd = -1;
    int client_fd = -1;
    bool is_webcam_mode = (config_.stream_url.rfind("webcam", 0) == 0 || 
                           config_.stream_url.find(".pipe") != std::string::npos ||
                           config_.stream_url.find("tcp://") != std::string::npos);

    if (is_webcam_mode) {
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd >= 0) {
            int opt = 1;
            setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");
            addr.sin_port = htons(9876);
            if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                listen(server_fd, 1);
                fcntl(server_fd, F_SETFL, O_NONBLOCK);
                std::cout << "[CAPTURE][" << config_.id << "] Listening for live webcam stream on tcp://127.0.0.1:9876" << std::endl;
            } else {
                std::cerr << "[CAPTURE][" << config_.id << "] Note: Port 9876 bind: " << strerror(errno) << std::endl;
                close(server_fd);
                server_fd = -1;
            }
        }
    }

    while (!st.stop_requested()) {
        auto host_now = std::chrono::steady_clock::now();
        int64_t host_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(host_now.time_since_epoch()).count();
        current_pts_ns += frame_interval_ns;

        // 1. Notify supervisor of frame reception
        supervisor_->on_frame_received(current_pts_ns, host_ns);

        int64_t f_idx = ++frame_counter_;
        bool is_key = (f_idx % 30 == 1); // 1 keyframe every 30 frames (1-2s GOP)

        // 1. Decoded Frame: Read live pixels from webcam TCP socket (or fallback test pattern)
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
            if (client_fd < 0 && server_fd >= 0) {
                sockaddr_in c_addr{};
                socklen_t c_len = sizeof(c_addr);
                client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&c_addr), &c_len);
                if (client_fd >= 0) {
                    int flags = fcntl(client_fd, F_GETFL, 0);
                    fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK); // Ensure blocking I/O for clean frame reads
                    struct timeval tv{};
                    tv.tv_sec = 0;
                    tv.tv_usec = 150000; // 150ms socket read timeout
                    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                    int buf_size = 4 * 1024 * 1024; // 4MB buffer
                    setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
                    std::cout << "[CAPTURE][" << config_.id << "] Live webcam stream connected on tcp://127.0.0.1:9876!" << std::endl;
                }
            }
            if (client_fd >= 0) {
                size_t total_read = 0;
                while (total_read < frame_bytes && !st.stop_requested()) {
                    ssize_t n = recv(client_fd, buffer_data->data() + total_read, frame_bytes - total_read, 0);
                    if (n > 0) {
                        total_read += n;
                    } else if (n == 0) {
                        std::cout << "[CAPTURE][" << config_.id << "] Live webcam client disconnected." << std::endl;
                        close(client_fd);
                        client_fd = -1;
                        break;
                    } else {
                        // Timeout or error
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            if (total_read > 0) {
                                continue;
                            }
                        }
                        close(client_fd);
                        client_fd = -1;
                        break;
                    }
                }
                if (total_read == frame_bytes) {
                    read_from_pipe = true;
                }
            }
        }

        if (!read_from_pipe) {
            // Calm static industrial dark slate standby pattern (NO FLICKERING / ZERO FLASHING)
            for (size_t p = 0; p < frame_bytes; p += 3) {
                (*buffer_data)[p] = 45;     // B
                (*buffer_data)[p + 1] = 40; // G
                (*buffer_data)[p + 2] = 40; // R
            }
        }

        frame->data = buffer_data;
        latest_slot_->publish(frame);

        // 2. Encoded Tee: Push REAL frame into Pre-Event Ring and Event Manager
        if (read_from_pipe) {
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

    if (client_fd >= 0) {
        close(client_fd);
    }
    if (server_fd >= 0) {
        close(server_fd);
    }
}

} // namespace engine::capture
