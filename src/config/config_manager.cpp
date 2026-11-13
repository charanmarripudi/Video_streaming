#include "config/config_manager.hpp"
#include "json.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>

using json = nlohmann::json;

namespace engine::config {

bool ConfigManager::load(const std::string& config_path) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    active_path_ = config_path;

    if (!std::filesystem::exists(config_path)) {
        std::cerr << "[CONFIG] Error: Configuration file not found: " << config_path << std::endl;
        return false;
    }

    try {
        std::ifstream f(config_path);
        json j = json::parse(f);

        GlobalConfig cfg;
        if (j.contains("global_settings")) {
            auto& g = j["global_settings"];
            cfg.global_fps = g.value("global_fps", 15);
            cfg.storage_root = g.value("storage_root", "storage/events");
            cfg.default_encoder = g.value("default_encoder", "h264");
            cfg.max_inference_threads = g.value("max_inference_threads", 4);
            cfg.max_writer_threads = g.value("max_writer_threads", 2);
            cfg.memory_budget_bytes = g.value("memory_budget_bytes", 2147483648ULL);
            cfg.min_free_disk_bytes = g.value("min_free_disk_bytes", 5368709120ULL);
            cfg.platform = g.value("platform", "auto");
        }

        if (j.contains("cameras") && j["cameras"].is_array()) {
            for (const auto& c_json : j["cameras"]) {
                CameraConfig cam;
                cam.id = c_json.value("camera_id", "CAM_UNKNOWN");
                cam.name = c_json.value("name", cam.id);
                cam.enabled = c_json.value("enabled", true);
                cam.stream_url = c_json.value("stream_url", c_json.value("source_url", ""));
                cam.credentials_ref = c_json.value("credentials_ref", "default");
                cam.decode_mode = c_json.value("decode_mode", "hardware");
                cam.stall_timeout_s = c_json.value("stall_timeout_s", 5);

                if (c_json.contains("fps")) {
                    cam.fps = c_json["fps"].get<int>();
                }

                if (c_json.contains("capture_resolution")) {
                    cam.capture_width = c_json["capture_resolution"].value("width", 1280);
                    cam.capture_height = c_json["capture_resolution"].value("height", 720);
                }

                if (c_json.contains("event_config") || c_json.contains("buffer_config")) {
                    auto& b = c_json.contains("event_config") ? c_json["event_config"] : c_json["buffer_config"];
                    cam.event_config.pre_event_s = b.value("pre_event_s", b.value("pre_event_seconds", 15));
                    cam.event_config.post_event_s = b.value("post_event_s", b.value("post_event_seconds", 15));
                    cam.event_config.cooldown_s = b.value("cooldown_s", 30);
                    cam.event_config.max_clip_s = b.value("max_clip_s", 120);
                    cam.event_config.output_format = b.value("output_format", "mp4");
                    if (b.contains("output_fps")) {
                        cam.event_config.output_fps = b["output_fps"].get<int>();
                    }
                }

                if (c_json.contains("output_video")) {
                    auto& out = c_json["output_video"];
                    cam.event_config.output_width = out.value("width", 1280);
                    cam.event_config.output_height = out.value("height", 720);
                    cam.event_config.output_bitrate_kbps = out.value("bitrate_kbps", 900);
                }

                if (c_json.contains("assigned_use_cases") && c_json["assigned_use_cases"].is_array()) {
                    for (const auto& uc_json : c_json["assigned_use_cases"]) {
                        UseCaseConfig uc;
                        uc.use_case_id = uc_json.value("use_case_id", "");
                        uc.enabled = uc_json.value("enabled", true);
                        uc.model_ref = uc_json.value("model_ref", uc_json.value("model_path", ""));
                        uc.threshold = uc_json.value("threshold", uc_json.value("confidence", 0.40f));
                        uc.priority = uc_json.value("priority", 1);

                        if (uc_json.contains("analysis_fps") || uc_json.contains("inference_fps")) {
                            uc.analysis_fps = uc_json.value("analysis_fps", uc_json.value("inference_fps", 15));
                        }
                        if (uc_json.contains("model_input_size")) {
                            uc.model_input_size = uc_json["model_input_size"].get<int>();
                        }
                        if (uc_json.contains("enabled_classes") && uc_json["enabled_classes"].is_array()) {
                            for (const auto& cls : uc_json["enabled_classes"]) {
                                uc.enabled_classes.push_back(cls.get<std::string>());
                            }
                        }
                        if (uc_json.contains("roi_polygon") && uc_json["roi_polygon"].is_array()) {
                            for (const auto& pt : uc_json["roi_polygon"]) {
                                if (pt.is_array() && pt.size() >= 2) {
                                    uc.roi_polygon.push_back({pt[0].get<float>(), pt[1].get<float>()});
                                }
                            }
                        }
                        cam.assigned_use_cases.push_back(uc);
                    }
                }

                cfg.cameras.push_back(cam);
            }
        }

        current_config_ = cfg;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[CONFIG] Exception parsing JSON: " << e.what() << std::endl;
        return false;
    }
}

bool ConfigManager::save_atomic(const std::string& config_path) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    std::string temp_path = config_path + ".tmp";

    try {
        std::string json_str = to_json_string(true);
        {
            std::ofstream out(temp_path, std::ios::trunc);
            if (!out.is_open()) return false;
            out << json_str;
            out.flush();
        }
        std::filesystem::rename(temp_path, config_path);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[CONFIG] Error in atomic save: " << e.what() << std::endl;
        return false;
    }
}

GlobalConfig ConfigManager::get_config() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return current_config_;
}

std::optional<CameraConfig> ConfigManager::get_camera_config(const std::string& camera_id) const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    for (const auto& cam : current_config_.cameras) {
        if (cam.id == camera_id) return cam;
    }
    return std::nullopt;
}

void ConfigManager::update_camera(const CameraConfig& cam) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    for (auto& existing : current_config_.cameras) {
        if (existing.id == cam.id) {
            existing = cam;
            return;
        }
    }
    current_config_.cameras.push_back(cam);
}

std::string ConfigManager::to_json_string(bool pretty) const {
    json j;
    j["global_settings"] = {
        {"global_fps", current_config_.global_fps},
        {"storage_root", current_config_.storage_root},
        {"memory_budget_bytes", current_config_.memory_budget_bytes},
        {"max_inference_threads", current_config_.max_inference_threads},
        {"max_writer_threads", current_config_.max_writer_threads},
        {"min_free_disk_bytes", current_config_.min_free_disk_bytes},
        {"default_encoder", current_config_.default_encoder},
        {"platform", current_config_.platform}
    };

    json cams = json::array();
    for (const auto& cam : current_config_.cameras) {
        json c;
        c["camera_id"] = cam.id;
        c["name"] = cam.name;
        c["enabled"] = cam.enabled;
        c["stream_url"] = cam.stream_url;
        c["credentials_ref"] = cam.credentials_ref;
        c["decode_mode"] = cam.decode_mode;
        c["stall_timeout_s"] = cam.stall_timeout_s;
        if (cam.fps) c["fps"] = *cam.fps;

        c["capture_resolution"] = {
            {"width", cam.capture_width},
            {"height", cam.capture_height}
        };

        json b = {
            {"pre_event_s", cam.event_config.pre_event_s},
            {"post_event_s", cam.event_config.post_event_s},
            {"cooldown_s", cam.event_config.cooldown_s},
            {"max_clip_s", cam.event_config.max_clip_s},
            {"output_format", cam.event_config.output_format}
        };
        if (cam.event_config.output_fps) b["output_fps"] = *cam.event_config.output_fps;
        c["event_config"] = b;

        c["output_video"] = {
            {"width", cam.event_config.output_width},
            {"height", cam.event_config.output_height},
            {"bitrate_kbps", cam.event_config.output_bitrate_kbps}
        };

        json ucs = json::array();
        for (const auto& uc : cam.assigned_use_cases) {
            json u = {
                {"use_case_id", uc.use_case_id},
                {"enabled", uc.enabled},
                {"model_ref", uc.model_ref},
                {"threshold", uc.threshold},
                {"priority", uc.priority},
                {"enabled_classes", uc.enabled_classes}
            };
            if (uc.analysis_fps) u["analysis_fps"] = *uc.analysis_fps;
            if (uc.model_input_size) u["model_input_size"] = *uc.model_input_size;
            ucs.push_back(u);
        }
        c["assigned_use_cases"] = ucs;
        cams.push_back(c);
    }

    j["cameras"] = cams;
    return pretty ? j.dump(2) : j.dump();
}

} // namespace engine::config
