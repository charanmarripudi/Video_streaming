#include "orchestrator/orchestrator.hpp"
#include "inference/inference_service.hpp"
#include <iostream>

namespace engine::orchestrator {

Orchestrator::~Orchestrator() {
    shutdown();
}

void Orchestrator::initialize(const GlobalConfig& initial_config) {
    std::lock_guard<std::mutex> lock(orchestrator_mutex_);
    current_config_ = initial_config;

    // 1. Initialize the Shared Bounded Inference Service
    inference::InferenceService::instance().initialize(current_config_.max_inference_threads);

    std::cout << "\n=======================================================\n"
              << "[ORCHESTRATOR v2] Initializing Production Video Engine\n"
              << "  -> Global Fallback FPS   : " << current_config_.global_fps << "\n"
              << "  -> Memory Budget         : " << (current_config_.memory_budget_bytes / (1024 * 1024)) << " MB\n"
              << "  -> Max Inference Threads : " << current_config_.max_inference_threads << "\n"
              << "  -> Configured Cameras    : " << current_config_.cameras.size() << "\n"
              << "=======================================================\n" << std::endl;

    for (const auto& cam : current_config_.cameras) {
        if (!cam.enabled) continue;

        CameraFingerprints fp;
        fp.capture_fp = cam.compute_capture_fingerprint(current_config_.global_fps);
        fp.events_fp = cam.event_config.compute_fingerprint();
        fp.composite_hash = cam.compute_hash(current_config_.global_fps);

        auto context = std::make_unique<CameraContext>(
            cam, current_config_.global_fps, current_config_.min_free_disk_bytes
        );
        context->start();

        active_fingerprints_[cam.id] = fp;
        contexts_[cam.id] = std::move(context);
    }

    std::cout << "[ORCHESTRATOR v2] Deployment Ready! Active Cameras: " << contexts_.size() 
              << ", Active Workers: " << total_active_workers() << "\n" << std::endl;
}

void Orchestrator::reload(const GlobalConfig& updated_config) {
    std::lock_guard<std::mutex> lock(orchestrator_mutex_);

    std::cout << "\n-------------------------------------------------------"
              << "\n[ORCHESTRATOR v2] Executing Granular Differential Reload..."
              << "\n-------------------------------------------------------" << std::endl;

    std::unordered_map<std::string, CameraConfig> new_cams;
    std::unordered_map<std::string, CameraFingerprints> new_fps;

    for (const auto& cam : updated_config.cameras) {
        if (cam.enabled) {
            new_cams[cam.id] = cam;
            CameraFingerprints fp;
            fp.capture_fp = cam.compute_capture_fingerprint(updated_config.global_fps);
            fp.events_fp = cam.event_config.compute_fingerprint();
            fp.composite_hash = cam.compute_hash(updated_config.global_fps);
            new_fps[cam.id] = fp;
        }
    }

    // 1. Process Removed Cameras
    for (auto it = contexts_.begin(); it != contexts_.end();) {
        const std::string& cam_id = it->first;
        if (new_fps.find(cam_id) == new_fps.end()) {
            std::cout << "[ORCHESTRATOR v2] Camera " << cam_id << " removed/disabled. Stopping..." << std::endl;
            it->second->stop();
            active_fingerprints_.erase(cam_id);
            it = contexts_.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Process Existing Cameras (Diff per component!)
    for (auto& [cam_id, ctx] : contexts_) {
        const auto& old_fp = active_fingerprints_[cam_id];
        const auto& new_fp = new_fps[cam_id];
        const auto& new_cam = new_cams[cam_id];

        if (old_fp.composite_hash == new_fp.composite_hash) {
            std::cout << "[ORCHESTRATOR v2] Camera " << cam_id << " UNCHANGED. Zero interruption!" << std::endl;
            continue;
        }

        // Check if ONLY use cases changed
        if (old_fp.capture_fp == new_fp.capture_fp && old_fp.events_fp == new_fp.events_fp) {
            std::cout << "[ORCHESTRATOR v2] Camera " << cam_id 
                      << " Use-Cases modified. IN-PLACE PATCH: Zero Frame Loss on Capture!" << std::endl;
            ctx->patch_use_cases(new_cam.assigned_use_cases);
            active_fingerprints_[cam_id] = new_fp;
        } else {
            // Capture or Ring Buffer changed: restart this camera context
            std::cout << "[ORCHESTRATOR v2] Camera " << cam_id << " capture/ring settings modified. Restarting context..." << std::endl;
            ctx->stop();
            auto new_ctx = std::make_unique<CameraContext>(new_cam, updated_config.global_fps, updated_config.min_free_disk_bytes);
            new_ctx->start();
            ctx = std::move(new_ctx);
            active_fingerprints_[cam_id] = new_fp;
        }
    }

    // 3. Process Newly Added Cameras
    for (const auto& [cam_id, new_fp] : new_fps) {
        if (contexts_.find(cam_id) == contexts_.end()) {
            std::cout << "[ORCHESTRATOR v2] Spawning NEW Camera Context: " << cam_id << std::endl;
            auto new_ctx = std::make_unique<CameraContext>(new_cams[cam_id], updated_config.global_fps, updated_config.min_free_disk_bytes);
            new_ctx->start();
            contexts_[cam_id] = std::move(new_ctx);
            active_fingerprints_[cam_id] = new_fp;
        }
    }

    current_config_ = updated_config;
    std::cout << "[ORCHESTRATOR v2] Reload Complete! Active Cameras: " << contexts_.size() 
              << ", Active Workers: " << total_active_workers() << "\n" << std::endl;
}

void Orchestrator::shutdown() {
    std::lock_guard<std::mutex> lock(orchestrator_mutex_);
    if (contexts_.empty()) return;

    std::cout << "\n[ORCHESTRATOR v2] Initiating Ordered Engine Shutdown..." << std::endl;
    // 1. Request stop in parallel across all camera contexts
    for (auto& [cam_id, ctx] : contexts_) {
        ctx->request_stop();
    }

    // 2. Join all camera contexts
    for (auto& [cam_id, ctx] : contexts_) {
        ctx->join();
    }
    contexts_.clear();
    active_fingerprints_.clear();

    inference::InferenceService::instance().shutdown();
    std::cout << "[ORCHESTRATOR v2] Shutdown complete. All resources destructed." << std::endl;
}

size_t Orchestrator::active_camera_count() const {
    return contexts_.size();
}

size_t Orchestrator::total_active_workers() const {
    size_t count = 0;
    for (const auto& [id, ctx] : contexts_) {
        count += ctx->worker_count();
    }
    return count;
}

} // namespace engine::orchestrator
