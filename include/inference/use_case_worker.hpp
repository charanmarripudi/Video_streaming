#ifndef INFERENCE_USE_CASE_WORKER_HPP
#define INFERENCE_USE_CASE_WORKER_HPP

#include <string>
#include <thread>
#include <stop_token>
#include <memory>
#include <chrono>
#include "common/types.hpp"
#include "buffer/pre_event_ring.hpp"
#include "recorder/event_manager.hpp"
#include "inference/inference_service.hpp"

namespace engine::inference {

class UseCaseWorker {
public:
    UseCaseWorker(std::string camera_id,
                  UseCaseConfig config,
                  int camera_fps,
                  std::shared_ptr<LatestFrameSlot> latest_slot,
                  std::shared_ptr<buffer::PreEventRing> pre_event_ring,
                  std::shared_ptr<recorder::EventManager> event_mgr);
    ~UseCaseWorker();

    void start();
    void request_stop() noexcept;
    void join();
    void stop();

    const std::string& use_case_id() const { return config_.use_case_id; }
    const UseCaseConfig& config() const { return config_; }

private:
    void worker_loop(std::stop_token st);

    std::string camera_id_;
    UseCaseConfig config_;
    int inference_fps_;
    std::shared_ptr<LatestFrameSlot> latest_slot_;
    std::shared_ptr<buffer::PreEventRing> pre_event_ring_;
    std::shared_ptr<recorder::EventManager> event_mgr_;
    std::jthread worker_thread_;
};

} // namespace engine::inference

#endif // INFERENCE_USE_CASE_WORKER_HPP
