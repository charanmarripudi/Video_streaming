#include "orchestrator/camera_context.hpp"
#include <iostream>

namespace engine::orchestrator {

CameraContext::CameraContext(CameraConfig config, int global_fps, uint64_t min_free_disk_bytes)
    : config_(std::move(config)), 
      global_fps_(global_fps), 
      min_free_disk_bytes_(min_free_disk_bytes) 
{
    // 1. Lock-free Latest Frame Slot
    latest_slot_ = std::make_shared<LatestFrameSlot>();

    // 2. Pre-Event Ring Buffer (stores up to 15s pre-event window)
    size_t ring_max_bytes = 350 * 1024 * 1024; // 350 MB budget for local frames
    int64_t window_ns = static_cast<int64_t>(config_.event_config.pre_event_s) * 1000000000LL;
    pre_event_ring_ = std::make_shared<buffer::PreEventRing>(ring_max_bytes, window_ns);

    // 3. Unified Per-Camera Event Manager
    event_mgr_ = std::make_shared<recorder::EventManager>(config_, global_fps_, min_free_disk_bytes_);

    // 4. Stream Capture & Supervisor
    capture_ = std::make_unique<capture::StreamCapture>(
        config_, global_fps_, latest_slot_, pre_event_ring_, event_mgr_
    );

    // 5. Worker Tasks (Lightweight, submit to shared Inference Service)
    int eff_fps = config_.get_effective_fps(global_fps_);
    for (const auto& uc : config_.assigned_use_cases) {
        if (!uc.enabled) continue;
        auto worker = std::make_unique<inference::UseCaseWorker>(
            config_.id, uc, eff_fps, latest_slot_, pre_event_ring_, event_mgr_
        );
        worker_fingerprints_[uc.use_case_id] = uc.compute_fingerprint();
        workers_[uc.use_case_id] = std::move(worker);
    }
}

CameraContext::~CameraContext() {
    stop();
}

void CameraContext::start() {
    std::cout << "[CONTEXT][" << config_.id << "] Bootstrapping v2 Context (Ring Budget: 16 MB, Workers: " 
              << workers_.size() << ")..." << std::endl;

    for (auto& [id, w] : workers_) {
        w->start();
    }
    capture_->start();
}

void CameraContext::request_stop() noexcept {
    if (capture_) {
        capture_->request_stop();
    }
    for (auto& [id, w] : workers_) {
        w->request_stop();
    }
}

void CameraContext::join() {
    if (capture_) {
        capture_->join();
    }
    for (auto& [id, w] : workers_) {
        w->join();
    }
    if (event_mgr_) {
        event_mgr_->shutdown(std::chrono::milliseconds(500));
    }
}

void CameraContext::stop() {
    std::cout << "[CONTEXT][" << config_.id << "] Tearing down v2 Context..." << std::endl;
    request_stop();
    join();
    std::cout << "[CONTEXT][" << config_.id << "] Teardown complete." << std::endl;
}

void CameraContext::patch_use_cases(const std::vector<UseCaseConfig>& new_use_cases) {
    int eff_fps = config_.get_effective_fps(global_fps_);
    std::unordered_map<std::string, UseCaseConfig> new_map;
    std::unordered_map<std::string, std::string> new_fps;

    for (const auto& uc : new_use_cases) {
        if (uc.enabled) {
            new_map[uc.use_case_id] = uc;
            new_fps[uc.use_case_id] = uc.compute_fingerprint();
        }
    }

    // 1. Remove obsolete or modified workers
    for (auto it = workers_.begin(); it != workers_.end();) {
        const std::string& uc_id = it->first;
        if (new_fps.find(uc_id) == new_fps.end() || new_fps[uc_id] != worker_fingerprints_[uc_id]) {
            std::cout << "[CONTEXT][" << config_.id << "] IN-PLACE PATCH: Stopping worker [" << uc_id 
                      << "] without touching capture!" << std::endl;
            it->second->stop();
            worker_fingerprints_.erase(uc_id);
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Add newly enabled or updated workers
    for (const auto& [uc_id, uc] : new_map) {
        if (workers_.find(uc_id) == workers_.end()) {
            std::cout << "[CONTEXT][" << config_.id << "] IN-PLACE PATCH: Spawning worker [" << uc_id 
                      << "] without touching capture!" << std::endl;
            auto worker = std::make_unique<inference::UseCaseWorker>(
                config_.id, uc, eff_fps, latest_slot_, pre_event_ring_, event_mgr_
            );
            worker->start();
            worker_fingerprints_[uc_id] = new_fps[uc_id];
            workers_[uc_id] = std::move(worker);
        }
    }
}

} // namespace engine::orchestrator
