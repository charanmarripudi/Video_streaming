#ifndef ORCHESTRATOR_ORCHESTRATOR_HPP
#define ORCHESTRATOR_ORCHESTRATOR_HPP

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include "common/types.hpp"
#include "orchestrator/camera_context.hpp"

namespace engine::orchestrator {

struct CameraFingerprints {
    std::string capture_fp;
    std::string events_fp;
    std::string composite_hash;
};

class Orchestrator {
public:
    Orchestrator() = default;
    ~Orchestrator();

    void initialize(const GlobalConfig& initial_config);

    // v2 Granular Hot-Reload: Patches use-cases in-place, restarts capture only when needed!
    void reload(const GlobalConfig& updated_config);

    void shutdown();

    size_t active_camera_count() const;
    size_t total_active_workers() const;

private:
    mutable std::mutex orchestrator_mutex_;
    GlobalConfig current_config_;
    std::unordered_map<std::string, std::unique_ptr<CameraContext>> contexts_;
    std::unordered_map<std::string, CameraFingerprints> active_fingerprints_;
};

} // namespace engine::orchestrator

#endif // ORCHESTRATOR_ORCHESTRATOR_HPP
