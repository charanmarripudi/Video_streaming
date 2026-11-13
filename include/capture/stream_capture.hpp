#ifndef CAPTURE_STREAM_CAPTURE_HPP
#define CAPTURE_STREAM_CAPTURE_HPP

#include <string>
#include <thread>
#include <stop_token>
#include <atomic>
#include <memory>
#include <chrono>
#include "common/types.hpp"
#include "buffer/pre_event_ring.hpp"
#include "capture/stream_supervisor.hpp"

namespace engine::recorder {
    class EventManager;
}

namespace engine::capture {

class StreamCapture {
public:
    StreamCapture(CameraConfig config, 
                  int global_fps,
                  std::shared_ptr<LatestFrameSlot> latest_slot,
                  std::shared_ptr<buffer::PreEventRing> pre_event_ring,
                  std::shared_ptr<recorder::EventManager> event_mgr);
    ~StreamCapture();

    void start();
    void request_stop() noexcept;
    void join();
    void stop();

    bool is_running() const { return is_running_.load(); }
    int64_t captured_frames() const { return frame_counter_.load(); }
    StreamState state() const { return supervisor_->current_state(); }

private:
    void capture_loop(std::stop_token st);

    CameraConfig config_;
    int effective_fps_;
    std::shared_ptr<LatestFrameSlot> latest_slot_;
    std::shared_ptr<buffer::PreEventRing> pre_event_ring_;
    std::shared_ptr<recorder::EventManager> event_mgr_;
    std::unique_ptr<StreamSupervisor> supervisor_;

    std::jthread capture_thread_;
    std::atomic<bool> is_running_{false};
    std::atomic<int64_t> frame_counter_{0};
};

} // namespace engine::capture

#endif // CAPTURE_STREAM_CAPTURE_HPP
