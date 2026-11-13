#include "config/config_watcher.hpp"
#include "common/sha256.hpp"
#include <fstream>
#include <iostream>

namespace engine::config {

ConfigWatcher::ConfigWatcher(std::shared_ptr<ConfigManager> manager, 
                             std::string config_path,
                             ChangeCallback callback)
    : manager_(manager), config_path_(std::move(config_path)), callback_(std::move(callback)) {}

ConfigWatcher::~ConfigWatcher() {
    stop();
}

void ConfigWatcher::start() {
    if (std::filesystem::exists(config_path_)) {
        last_write_time_ = std::filesystem::last_write_time(config_path_);
        std::ifstream f(config_path_);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        last_file_hash_ = crypto::SHA256::hash(content);
    }

    watcher_thread_ = std::jthread([this](std::stop_token st) {
        watch_loop(st);
    });
    std::cout << "[WATCHER] Background Config Watcher started for: " << config_path_ << std::endl;
}

void ConfigWatcher::stop() {
    if (watcher_thread_.joinable()) {
        watcher_thread_.request_stop();
        watcher_thread_.join();
    }
}

void ConfigWatcher::watch_loop(std::stop_token st) {
    while (!st.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (st.stop_requested()) break;

        if (!std::filesystem::exists(config_path_)) {
            continue;
        }

        try {
            auto current_time = std::filesystem::last_write_time(config_path_);
            if (current_time != last_write_time_) {
                // 1. Debounce stabilization window (200ms) to ensure file write finishes
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

                std::ifstream f(config_path_);
                std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                std::string current_hash = crypto::SHA256::hash(content);

                if (current_hash != last_file_hash_) {
                    std::cout << "\n[WATCHER] Detected file change on " << config_path_ << " (Hash diff detected)!" << std::endl;
                    last_write_time_ = current_time;
                    last_file_hash_ = current_hash;

                    if (manager_->load(config_path_)) {
                        if (callback_) {
                            callback_(manager_->get_config());
                        }
                    } else {
                        std::cerr << "[WATCHER] Error: Failed to parse updated config file. Changes rejected." << std::endl;
                    }
                } else {
                    last_write_time_ = current_time; // Timestamp changed, but content identical
                }
            }
        } catch (const std::exception& e) {
            // Ignore transient read errors during atomic replacement
        }
    }
}

} // namespace engine::config

