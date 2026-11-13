#include "inference/use_case_worker.hpp"
#include <iostream>

namespace engine::inference {

UseCaseWorker::UseCaseWorker(std::string camera_id,
                             UseCaseConfig config,
                             int camera_fps,
                             std::shared_ptr<LatestFrameSlot> latest_slot,
                             std::shared_ptr<buffer::PreEventRing> pre_event_ring,
                             std::shared_ptr<recorder::EventManager> event_mgr)
    : camera_id_(std::move(camera_id)),
      config_(std::move(config)),
      inference_fps_(config_.analysis_fps.value_or(camera_fps)),
      latest_slot_(std::move(latest_slot)),
      pre_event_ring_(std::move(pre_event_ring)),
      event_mgr_(std::move(event_mgr)) 
{
    // Register model with shared inference service (loaded once!)
    InferenceService::instance().register_model(config_);
}

UseCaseWorker::~UseCaseWorker() {
    stop();
}

void UseCaseWorker::start() {
    worker_thread_ = std::jthread([this](std::stop_token st) {
        worker_loop(st);
    });
    std::cout << "[WORKER][" << camera_id_ << "][" << config_.use_case_id 
              << "] Active lightweight task (Analysis FPS: " << inference_fps_ << ")" << std::endl;
}

void UseCaseWorker::request_stop() noexcept {
    if (worker_thread_.joinable()) {
        worker_thread_.request_stop();
    }
}

void UseCaseWorker::join() {
    if (worker_thread_.joinable()) {
        worker_thread_.join();
        std::cout << "[WORKER][" << camera_id_ << "][" << config_.use_case_id 
                  << "] Joined worker task cleanly." << std::endl;
    }
}

void UseCaseWorker::stop() {
    request_stop();
    join();
}

void UseCaseWorker::worker_loop(std::stop_token st) {
    int64_t last_processed_index = -1;
    const auto min_interval = std::chrono::microseconds(1000000 / inference_fps_);
    auto last_time = std::chrono::steady_clock::now();

    while (!st.stop_requested()) {
        // 1. Lock-Free Latest Frame Retrieval
        FramePtr frame = latest_slot_->get();

        if (!frame || !frame->is_valid() || frame->frame_index == last_processed_index) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        if ((now - last_time) < min_interval) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        last_time = now;
        last_processed_index = frame->frame_index;

        // 2. Submit to Shared Inference Service (replaces in-flight pending requests)
        auto future_res = InferenceService::instance().submit(
            config_.model_ref, camera_id_, frame, config_.priority
        );

        // Await prediction result
        auto status = future_res.wait_for(std::chrono::milliseconds(100));
        if (status == std::future_status::ready) {
            auto detections = future_res.get();
            if (!detections.empty()) {
                std::cout << "[DETECTION][" << camera_id_ << "][" << config_.use_case_id 
                          << "] Hits=" << detections.size() << " on frame #" << frame->frame_index << std::endl;

                // 3. Report detection to per-camera EventManager (central coordinator!)
                Detection det;
                det.camera_id = camera_id_;
                det.use_case_id = config_.use_case_id;
                det.pts_ns = frame->pts_ns;
                det.host_ns = frame->host_ns;
                det.score = detections[0].confidence;
                det.label = detections[0].label;
                det.boxes = detections;

                if (event_mgr_ && pre_event_ring_) {
                    event_mgr_->on_detection(det, *pre_event_ring_);
                }
            }
        }
    }
}

} // namespace engine::inference
