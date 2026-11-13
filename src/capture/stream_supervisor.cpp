#include "capture/stream_supervisor.hpp"

namespace engine::capture {

StreamSupervisor::StreamSupervisor(std::string camera_id, int stall_timeout_s)
    : camera_id_(std::move(camera_id)),
      stall_timeout_s_(stall_timeout_s),
      rng_(std::random_device{}()) {}

void StreamSupervisor::transition_to(StreamState new_state) {
    StreamState old_state = state_.exchange(new_state);
    if (old_state != new_state) {
        std::cout << "[SUPERVISOR][" << camera_id_ << "] State Transition: "
                  << stream_state_to_string(old_state) << " -> "
                  << stream_state_to_string(new_state) << std::endl;
    }
}

void StreamSupervisor::on_pipeline_built() {
    transition_to(StreamState::CONNECTING);
}

void StreamSupervisor::on_frame_received(int64_t pts_ns, int64_t host_ns) {
    last_pts_ns_.store(pts_ns);
    last_host_ns_.store(host_ns);

    if (state_.load() != StreamState::STREAMING) {
        transition_to(StreamState::STREAMING);
        backoff_seconds_ = 1; // Reset backoff on stable stream
    }
}

int64_t StreamSupervisor::last_frame_age_ms() const {
    int64_t last = last_host_ns_.load();
    if (last == 0) return 999999;
    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    return (now_ns - last) / 1000000;
}

void StreamSupervisor::check_watchdog() {
    if (state_.load() == StreamState::STREAMING) {
        int64_t age_ms = last_frame_age_ms();
        if (age_ms > (stall_timeout_s_ * 1000)) {
            std::cerr << "[WATCHDOG][" << camera_id_ << "] Frame stall detected! Age: " 
                      << age_ms << "ms > " << (stall_timeout_s_ * 1000) << "ms timeout." << std::endl;
            transition_to(StreamState::DEGRADED);
        }
    } else if (state_.load() == StreamState::DEGRADED) {
        transition_to(StreamState::BACKOFF);
    }
}

bool StreamSupervisor::should_reconnect() {
    return state_.load() == StreamState::BACKOFF;
}

std::chrono::milliseconds StreamSupervisor::compute_backoff_delay() {
    reconnect_count_++;
    
    // Exponential backoff with random jitter: (backoff * 0.8 to 1.2)
    std::uniform_real_distribution<float> dist(0.8f, 1.2f);
    int delay_sec = static_cast<int>(backoff_seconds_ * dist(rng_));
    if (delay_sec < 1) delay_sec = 1;

    backoff_seconds_ = std::min(backoff_seconds_ * 2, 60); // Cap at 60 seconds
    std::cout << "[SUPERVISOR][" << camera_id_ << "] Scheduling reconnect #" << reconnect_count_.load()
              << " in " << delay_sec << " seconds..." << std::endl;

    return std::chrono::milliseconds(delay_sec * 1000);
}

} // namespace engine::capture

