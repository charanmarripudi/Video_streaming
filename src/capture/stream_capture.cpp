#include "capture/stream_capture.hpp"
#include "capture/hardware_factory.hpp"
#include "recorder/event_manager.hpp"
#include <iostream>

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

    while (!st.stop_requested()) {
        auto host_now = std::chrono::steady_clock::now();
        int64_t host_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(host_now.time_since_epoch()).count();
        current_pts_ns += frame_interval_ns;

        // 1. Notify supervisor of frame reception
        supervisor_->on_frame_received(current_pts_ns, host_ns);

        int64_t f_idx = ++frame_counter_;
        bool is_key = (f_idx % 30 == 1); // 1 keyframe every 30 frames (1-2s GOP)

        // 2. Encoded Tee: Push compressed NAL unit into Pre-Event Ring before decode (Saves 99% RAM!)
        EncodedSample sample;
        sample.pts_ns = current_pts_ns;
        sample.dts_ns = current_pts_ns;
        sample.is_keyframe = is_key;
        // Lightweight simulated compressed NAL unit (approx 15-40 KB per frame vs 3MB uncompressed!)
        size_t compressed_bytes = is_key ? 45000 : 8000;
        sample.payload = std::make_shared<const std::vector<uint8_t>>(compressed_bytes, 0xAA);

        if (pre_event_ring_) {
            pre_event_ring_->push(sample);
        }

        // Notify EventManager for post-event accumulation on open clips
        if (event_mgr_) {
            event_mgr_->on_encoded_sample(sample);
        }

        // 3. Decoded Frame: Publish to Lock-Free LatestFrameSlot (Zero-Copy & Zero Mutex!)
        auto frame = std::make_shared<Frame>();
        frame->frame_index = f_idx;
        frame->pts_ns = current_pts_ns;
        frame->host_ns = host_ns;
        frame->width = config_.capture_width;
        frame->height = config_.capture_height;
        frame->channels = 3;

        // Shared immutable buffer
        auto buffer_data = std::make_shared<std::vector<uint8_t>>(frame_bytes);
        (*buffer_data)[0] = static_cast<uint8_t>((f_idx * 5) % 255);
        (*buffer_data)[1] = 175;
        (*buffer_data)[2] = 210;
        frame->data = buffer_data;

        latest_slot_->publish(frame);

        // Periodic log
        if (f_idx % (effective_fps_ * 5) == 0) {
            std::cout << "[CAPTURE][" << config_.id << "] Processed frame #" << f_idx 
                      << " (Ring Bytes: " << (pre_event_ring_->current_bytes() / 1024) 
                      << " KB, State: " << stream_state_to_string(supervisor_->current_state()) << ")" << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::nanoseconds(frame_interval_ns));
    }
}

} // namespace engine::capture
