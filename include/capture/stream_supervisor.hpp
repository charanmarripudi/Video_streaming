#ifndef CAPTURE_STREAM_SUPERVISOR_HPP
#define CAPTURE_STREAM_SUPERVISOR_HPP

#include <string>
#include <chrono>
#include <atomic>
#include <random>
#include <iostream>
#include "common/types.hpp"

namespace engine::capture {

class StreamSupervisor {
public:
    explicit StreamSupervisor(std::string camera_id, int stall_timeout_s = 5);

    void on_pipeline_built();
    void on_frame_received(int64_t pts_ns, int64_t host_ns);
    void check_watchdog();

    StreamState current_state() const { return state_.load(); }
    int reconnect_count() const { return reconnect_count_.load(); }
    int64_t last_pts_ns() const { return last_pts_ns_.load(); }
    int64_t last_frame_age_ms() const;

    bool should_reconnect();
    std::chrono::milliseconds compute_backoff_delay();

private:
    void transition_to(StreamState new_state);

    std::string camera_id_;
    int stall_timeout_s_;
    std::atomic<StreamState> state_{StreamState::CONNECTING};
    std::atomic<int64_t> last_pts_ns_{0};
    std::atomic<int64_t> last_host_ns_{0};
    std::atomic<int> reconnect_count_{0};

    int backoff_seconds_{1};
    std::mt19937 rng_;
};

} // namespace engine::capture

#endif // CAPTURE_STREAM_SUPERVISOR_HPP

