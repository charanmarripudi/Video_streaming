#ifndef INFERENCE_MODEL_RUNNER_HPP
#define INFERENCE_MODEL_RUNNER_HPP

#include <vector>
#include <string>
#include <iostream>
#include "common/types.hpp"

namespace engine::inference {

class ModelRunner {
public:
    explicit ModelRunner(UseCaseConfig config) : config_(std::move(config)) {
        std::cout << "[AI-LOAD] Initialized model session: " << config_.use_case_id 
                  << " (" << config_.model_ref << "), Threshold=" << config_.threshold << std::endl;
    }

    // Runs inference on an immutable FramePtr
    std::vector<BoundingBox> predict(const FramePtr& frame) {
        std::vector<BoundingBox> detections;
        if (!frame || !frame->is_valid()) return detections;

        // Simulated AI detection rule (triggers periodically for testing event recording)
        // In full production, this binds to ONNX Runtime Ort::Session::Run()
        if (config_.use_case_id == "tt_crew_entering_below_tt") {
            // Trigger sample detection every 150 frames
            if (frame->frame_index > 0 && frame->frame_index % 150 == 0) {
                detections.push_back({100.0f, 200.0f, 400.0f, 600.0f, "person", 0.88f});
                detections.push_back({50.0f, 150.0f, 800.0f, 650.0f, "truck", 0.94f});
            }
        } 
        else if (config_.use_case_id == "tt_crew_non_availability") {
            if (frame->frame_index > 0 && frame->frame_index % 250 == 0) {
                detections.push_back({80.0f, 180.0f, 750.0f, 620.0f, "no-TT_Crew_Availability", 0.91f});
            }
        }

        return detections;
    }

    const UseCaseConfig& config() const { return config_; }

private:
    UseCaseConfig config_;
};

} // namespace engine::inference

#endif // INFERENCE_MODEL_RUNNER_HPP
