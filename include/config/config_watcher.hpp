#ifndef CONFIG_CONFIG_WATCHER_HPP
#define CONFIG_CONFIG_WATCHER_HPP

#include <string>
#include <functional>
#include <thread>
#include <stop_token>
#include <chrono>
#include <filesystem>
#include "common/types.hpp"
#include "config/config_manager.hpp"

namespace engine::config {

class ConfigWatcher {
public:
    using ChangeCallback = std::function<void(const GlobalConfig&)>;

    explicit ConfigWatcher(std::shared_ptr<ConfigManager> manager, 
                           std::string config_path,
                           ChangeCallback callback);
    ~ConfigWatcher();

    void start();
    void stop();

private:
    void watch_loop(std::stop_token st);

    std::shared_ptr<ConfigManager> manager_;
    std::string config_path_;
    ChangeCallback callback_;
    std::jthread watcher_thread_;
    std::string last_file_hash_;
    std::filesystem::file_time_type last_write_time_;
};

} // namespace engine::config

#endif // CONFIG_CONFIG_WATCHER_HPP

