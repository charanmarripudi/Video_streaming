#ifndef ORCHESTRATOR_CAMERA_CONTEXT_HPP
#define ORCHESTRATOR_CAMERA_CONTEXT_HPP

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include "common/types.hpp"
#include "buffer/pre_event_ring.hpp"
#include "capture/stream_capture.hpp"
#include "recorder/event_manager.hpp"
#include "inference/use_case_worker.hpp"

namespace engine::orchestrator {

class CameraContext {
public:
    CameraContext(CameraConfig config, int global_fps, uint64_t min_free_disk_bytes);
    ~CameraContext();

    void start();
    void request_stop() noexcept;
    void join();
    void stop();

    // v2: Granular in-place patching of use-cases without stopping capture!
    void patch_use_cases(const std::vector<UseCaseConfig>& new_use_cases);

    const std::string& camera_id() const { return config_.id; }
    const CameraConfig& config() const { return config_; }
    size_t worker_count() const { return workers_.size(); }

private:
    CameraConfig config_;
    int global_fps_;
    uint64_t min_free_disk_bytes_;

    std::shared_ptr<LatestFrameSlot> latest_slot_;
    std::shared_ptr<buffer::PreEventRing> pre_event_ring_;
    std::shared_ptr<recorder::EventManager> event_mgr_;
    std::unique_ptr<capture::StreamCapture> capture_;

    std::unordered_map<std::string, std::unique_ptr<inference::UseCaseWorker>> workers_;
    std::unordered_map<std::string, std::string> worker_fingerprints_;
};

} // namespace engine::orchestrator

#endif // ORCHESTRATOR_CAMERA_CONTEXT_HPP
