#ifndef CONFIG_CONFIG_MANAGER_HPP
#define CONFIG_CONFIG_MANAGER_HPP

#include <string>
#include <optional>
#include <mutex>
#include "common/types.hpp"

namespace engine::config {

class ConfigManager {
public:
    ConfigManager() = default;

    // Loads and validates configuration from disk
    bool load(const std::string& config_path);

    // Atomically persists configuration to disk (temp file + rename)
    bool save_atomic(const std::string& config_path);

    // Returns a copy of the current global configuration
    GlobalConfig get_config() const;

    // Returns effective configuration for a specific camera
    std::optional<CameraConfig> get_camera_config(const std::string& camera_id) const;

    // Updates camera settings in memory
    void update_camera(const CameraConfig& cam);

    // Raw JSON string export
    std::string to_json_string(bool pretty = true) const;

private:
    mutable std::mutex config_mutex_;
    GlobalConfig current_config_;
    std::string active_path_{"config.json"};
};

} // namespace engine::config

#endif // CONFIG_CONFIG_MANAGER_HPP

