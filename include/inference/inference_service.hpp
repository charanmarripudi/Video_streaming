#ifndef INFERENCE_INFERENCE_SERVICE_HPP
#define INFERENCE_INFERENCE_SERVICE_HPP

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <deque>
#include <future>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <stop_token>
#include <algorithm>
#include "common/types.hpp"
#include "inference/model_runner.hpp"

namespace engine::inference {

struct InferenceJob {
    std::string model_id;
    std::string camera_id;
    FramePtr frame;
    int priority{1};
    std::shared_ptr<std::promise<std::vector<BoundingBox>>> promise;
};

class InferenceService {
public:
    static InferenceService& instance();

    void initialize(int max_threads);
    void shutdown();

    void register_model(const UseCaseConfig& config);

    std::future<std::vector<BoundingBox>> submit(const std::string& model_id,
                                                const std::string& camera_id,
                                                FramePtr frame,
                                                int priority = 1);

    size_t active_models_count() const;

private:
    InferenceService() = default;
    ~InferenceService() { shutdown(); }

    void worker_loop(std::stop_token st);

    mutable std::mutex models_mutex_;
    std::unordered_map<std::string, std::shared_ptr<ModelRunner>> models_;

    std::mutex queue_mutex_;
    std::condition_variable cv_queue_;
    std::deque<InferenceJob> task_queue_;
    std::vector<std::jthread> pool_threads_;
    std::atomic<bool> is_running_{false};
};

} // namespace engine::inference

#endif // INFERENCE_INFERENCE_SERVICE_HPP
