#include "inference/inference_service.hpp"
#include <iostream>

namespace engine::inference {

InferenceService& InferenceService::instance() {
    static InferenceService s_instance;
    return s_instance;
}

void InferenceService::initialize(int max_threads) {
    if (is_running_.load()) return;
    is_running_.store(true);

    int thread_count = (max_threads > 0) ? max_threads : 4;
    pool_threads_.reserve(thread_count);

    for (int i = 0; i < thread_count; ++i) {
        pool_threads_.emplace_back([this](std::stop_token st) {
            worker_loop(st);
        });
    }

    std::cout << "[INFERENCE-SERVICE] Initialized Shared Bounded Pool (" 
              << thread_count << " worker threads)" << std::endl;
}

void InferenceService::shutdown() {
    if (!is_running_.load()) return;
    is_running_.store(false);
    cv_queue_.notify_all();

    for (auto& th : pool_threads_) {
        if (th.joinable()) {
            th.request_stop();
        }
    }
    cv_queue_.notify_all();

    for (auto& th : pool_threads_) {
        if (th.joinable()) {
            th.join();
        }
    }
    pool_threads_.clear();

    std::lock_guard<std::mutex> lock(models_mutex_);
    models_.clear();
    std::cout << "[INFERENCE-SERVICE] Shared Inference Pool stopped cleanly." << std::endl;
}

void InferenceService::register_model(const UseCaseConfig& config) {
    std::lock_guard<std::mutex> lock(models_mutex_);
    if (models_.find(config.model_ref) == models_.end()) {
        std::cout << "[INFERENCE-SERVICE] Loading shared model session once into memory: " 
                  << config.model_ref << " (Priority " << config.priority << ")" << std::endl;
        models_[config.model_ref] = std::make_shared<ModelRunner>(config);
    } else {
        std::cout << "[INFERENCE-SERVICE] Model already in memory: " << config.model_ref 
                  << ". Sharing existing weights across cameras!" << std::endl;
    }
}

std::future<std::vector<BoundingBox>> InferenceService::submit(const std::string& model_id,
                                                              const std::string& camera_id,
                                                              FramePtr frame,
                                                              int priority) 
{
    InferenceJob job;
    job.model_id = model_id;
    job.camera_id = camera_id;
    job.frame = std::move(frame);
    job.priority = priority;
    job.promise = std::make_shared<std::promise<std::vector<BoundingBox>>>();

    auto future = job.promise->get_future();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        // Replace pending job for the same camera and model (Latest-frame policy)
        for (auto& pending : task_queue_) {
            if (pending.camera_id == job.camera_id && pending.model_id == job.model_id) {
                pending.frame = job.frame;
                pending.promise = job.promise;
                cv_queue_.notify_one();
                return future;
            }
        }
        task_queue_.push_back(std::move(job));
    }
    cv_queue_.notify_one();
    return future;
}

void InferenceService::worker_loop(std::stop_token st) {
    while (!st.stop_requested()) {
        InferenceJob job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_queue_.wait_for(lock, std::chrono::milliseconds(20), [this, &st]() {
                return !task_queue_.empty() || st.stop_requested();
            });

            if (st.stop_requested() && task_queue_.empty()) break;
            if (task_queue_.empty()) continue;

            // Highest priority job first
            auto highest_it = std::max_element(
                task_queue_.begin(), task_queue_.end(),
                [](const InferenceJob& a, const InferenceJob& b) {
                    return a.priority < b.priority;
                }
            );

            job = std::move(*highest_it);
            task_queue_.erase(highest_it);
        }

        std::shared_ptr<ModelRunner> runner;
        {
            std::lock_guard<std::mutex> lock(models_mutex_);
            auto it = models_.find(job.model_id);
            if (it != models_.end()) {
                runner = it->second;
            }
        }

        if (runner && job.frame && job.frame->is_valid()) {
            auto results = runner->predict(job.frame);
            if (job.promise) {
                job.promise->set_value(std::move(results));
            }
        } else {
            if (job.promise) {
                job.promise->set_value({});
            }
        }
    }
}

size_t InferenceService::active_models_count() const {
    std::lock_guard<std::mutex> lock(models_mutex_);
    return models_.size();
}

} // namespace engine::inference
