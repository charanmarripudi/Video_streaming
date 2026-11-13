#ifndef RECORDER_EVENT_MANAGER_HPP
#define RECORDER_EVENT_MANAGER_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <chrono>
#include "common/types.hpp"
#include "buffer/pre_event_ring.hpp"

namespace engine::recorder {

struct ActiveClip {
    std::string camera_id;
    int64_t start_pts_ns{0};
    int64_t end_pts_ns{0};
    int64_t max_allowed_pts_ns{0};
    std::vector<std::string> trigger_reasons;
    Detection primary_detection;
    std::vector<EncodedSample> pre_event_samples;
    std::vector<EncodedSample> post_event_samples;
    int output_fps{15};
    int width{1280};
    int height{720};
    std::string output_path;
};

class EventManager {
public:
    EventManager(CameraConfig cam_config, int global_fps, uint64_t min_free_disk_bytes);
    ~EventManager() = default;

    // Called when any use case on this camera detects a violation
    void on_detection(const Detection& d, const buffer::PreEventRing& ring);

    // Ingests incoming live encoded samples & checks if open clips should close
    void on_encoded_sample(const EncodedSample& sample);

    void shutdown(std::chrono::milliseconds timeout);

    bool has_active_clip() const {
        std::lock_guard<std::mutex> lock(event_mutex_);
        return current_clip_ != nullptr;
    }

private:
    bool check_disk_guard() const;
    void finalize_and_persist_clip(ActiveClip clip);

    CameraConfig cam_config_;
    int global_fps_;
    int effective_fps_;
    uint64_t min_free_disk_bytes_;

    mutable std::mutex event_mutex_;
    std::unique_ptr<ActiveClip> current_clip_{nullptr};
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_cooldowns_;
};

} // namespace engine::recorder

#endif // RECORDER_EVENT_MANAGER_HPP

