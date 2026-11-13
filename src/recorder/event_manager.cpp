#include "recorder/event_manager.hpp"
#include "json.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

namespace engine::recorder {

EventManager::EventManager(CameraConfig cam_config, int global_fps, uint64_t min_free_disk_bytes)
    : cam_config_(std::move(cam_config)),
      global_fps_(global_fps),
      effective_fps_(cam_config_.get_effective_fps(global_fps)),
      min_free_disk_bytes_(min_free_disk_bytes) {}

bool EventManager::check_disk_guard() const {
    try {
        auto space = std::filesystem::space("storage/events");
        if (space.available < min_free_disk_bytes_) {
            std::cerr << "[DISK-GUARD] Warning: Low disk space! Available: " 
                      << (space.available / (1024 * 1024)) << " MB < Min Required: "
                      << (min_free_disk_bytes_ / (1024 * 1024)) << " MB. Refusing new video write." << std::endl;
            return false;
        }
    } catch (...) {}
    return true;
}

void EventManager::on_detection(const Detection& d, const buffer::PreEventRing& ring) {
    std::lock_guard<std::mutex> lock(event_mutex_);
    auto now = std::chrono::steady_clock::now();

    // 1. Check Cooldown per use-case
    auto it = last_cooldowns_.find(d.use_case_id);
    if (it != last_cooldowns_.end()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
        if (elapsed < cam_config_.event_config.cooldown_s) {
            // In cooldown: ignore duplicate trigger flood
            return;
        }
    }
    last_cooldowns_[d.use_case_id] = now;

    int64_t post_duration_ns = static_cast<int64_t>(cam_config_.event_config.post_event_s) * 1000000000LL;
    int64_t max_clip_duration_ns = static_cast<int64_t>(cam_config_.event_config.max_clip_s) * 1000000000LL;

    // 2. Check if a clip is already open (Trigger Merge Rule)
    if (current_clip_) {
        // Extend existing clip rather than opening a duplicate file!
        int64_t new_end = d.pts_ns + post_duration_ns;
        current_clip_->end_pts_ns = std::min(new_end, current_clip_->max_allowed_pts_ns);
        current_clip_->trigger_reasons.push_back(d.use_case_id);

        std::cout << "[EVENT-MANAGER][" << cam_config_.id 
                  << "] MERGE TRIGGER: Extended current open clip with reason: " << d.use_case_id 
                  << " (Total reasons on clip: " << current_clip_->trigger_reasons.size() << ")" << std::endl;
        return;
    }

    // 3. Disk Guard Check
    if (!check_disk_guard()) {
        std::cerr << "[EVENT-MANAGER][" << cam_config_.id << "] Event refused by Disk Guard." << std::endl;
        return;
    }

    // 4. Open New Clip
    auto clip = std::make_unique<ActiveClip>();
    clip->camera_id = cam_config_.id;
    clip->start_pts_ns = d.pts_ns;
    clip->end_pts_ns = d.pts_ns + post_duration_ns;
    clip->max_allowed_pts_ns = d.pts_ns + max_clip_duration_ns;
    clip->trigger_reasons.push_back(d.use_case_id);
    clip->primary_detection = d;
    clip->output_fps = cam_config_.get_output_fps(global_fps_);
    clip->width = cam_config_.event_config.output_width;
    clip->height = cam_config_.event_config.output_height;

    // Copy pointers from pre-event ring (aligned to prior keyframe)
    clip->pre_event_samples = ring.snapshot_from(d.pts_ns);

    // Format output path
    auto sys_now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(sys_now);
    std::stringstream ss;
    ss << "storage/events/" << cam_config_.id << "_" << d.use_case_id << "_" 
       << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S")
       << "_clip." << cam_config_.event_config.output_format;
    clip->output_path = ss.str();

    std::cout << "\n=======================================================\n"
              << "[EVENT-MANAGER] OPENED NEW CLIP: " << clip->output_path << "\n"
              << "  -> Primary Reason : " << d.use_case_id << " (Score: " << d.score << ")\n"
              << "  -> Pre-Event Ring : " << clip->pre_event_samples.size() << " compressed samples\n"
              << "  -> Output FPS     : " << clip->output_fps << " FPS\n"
              << "=======================================================\n" << std::endl;

    current_clip_ = std::move(clip);
}

void EventManager::on_encoded_sample(const EncodedSample& sample) {
    std::unique_ptr<ActiveClip> to_flush;

    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (!current_clip_) return;

        current_clip_->post_event_samples.push_back(sample);

        // Check if clip end PTS has passed
        if (sample.pts_ns >= current_clip_->end_pts_ns) {
            to_flush = std::move(current_clip_);
        }
    }

    if (to_flush) {
        finalize_and_persist_clip(*to_flush);
    }
}

void EventManager::finalize_and_persist_clip(ActiveClip clip) {
    std::filesystem::create_directories(std::filesystem::path(clip.output_path).parent_path());

    size_t total_samples = clip.pre_event_samples.size() + clip.post_event_samples.size();
    double duration_sec = static_cast<double>(total_samples) / static_cast<double>(clip.output_fps);

    // 1. Write structured JSON metadata sidecar (_meta.json)
    std::string meta_path = clip.output_path;
    size_t ext_pos = meta_path.rfind('.');
    if (ext_pos != std::string::npos) {
        meta_path = meta_path.substr(0, ext_pos) + "_meta.json";
    } else {
        meta_path += "_meta.json";
    }

    try {
        json meta_json;
        meta_json["camera_id"] = clip.camera_id;
        meta_json["primary_reason"] = clip.primary_detection.use_case_id;
        meta_json["score"] = clip.primary_detection.score;
        meta_json["timestamp_ns"] = clip.primary_detection.pts_ns;
        meta_json["trigger_reasons"] = clip.trigger_reasons;
        meta_json["output_fps"] = clip.output_fps;
        meta_json["resolution"] = { {"width", clip.width}, {"height", clip.height} };
        meta_json["duration_seconds"] = duration_sec;
        meta_json["pre_event_samples"] = clip.pre_event_samples.size();
        meta_json["post_event_samples"] = clip.post_event_samples.size();
        meta_json["total_samples"] = total_samples;
        
        json boxes_arr = json::array();
        for (const auto& box : clip.primary_detection.boxes) {
            boxes_arr.push_back({
                {"label", box.label},
                {"confidence", box.confidence},
                {"box", {box.x1, box.y1, box.x2, box.y2}}
            });
        }
        meta_json["bounding_boxes"] = boxes_arr;

        std::ofstream meta_out(meta_path);
        meta_out << meta_json.dump(2);
    } catch (...) {}

    // 2. Dump raw pre-event and post-event frames for ISO MP4 muxer
    std::string raw_path = clip.output_path + ".raw";
    {
        std::ofstream out(raw_path, std::ios::binary | std::ios::trunc);
        for (const auto& s : clip.pre_event_samples) {
            if (s.payload && !s.payload->empty()) {
                out.write(reinterpret_cast<const char*>(s.payload->data()), s.payload->size());
            }
        }
        for (const auto& s : clip.post_event_samples) {
            if (s.payload && !s.payload->empty()) {
                out.write(reinterpret_cast<const char*>(s.payload->data()), s.payload->size());
            }
        }
        out.flush();
    }

    // 3. Seal into standard QuickTime / VLC playable ISO MP4 Base Media
    std::string mux_cmd = "/Users/charanmarripudi/Documents/rtsp_rpi/hpcl_env/bin/python scripts/clip_muxer.py \"" 
                        + clip.output_path + "\" " 
                        + std::to_string(clip.width) + " " 
                        + std::to_string(clip.height) + " " 
                        + std::to_string(clip.output_fps) + " " 
                        + std::to_string(total_samples) + " > /dev/null 2>&1";
    std::system(mux_cmd.c_str());

    std::cout << "[EVENT-MANAGER][" << clip.camera_id << "] CLIP SEALED & PERSISTED (Playable ISO MP4):\n"
              << "  -> Video Stream : " << clip.output_path << " (" << total_samples << " packets, " 
              << duration_sec << "s @ " << clip.output_fps << " FPS)\n"
              << "  -> Event Record : " << meta_path << " (" << clip.trigger_reasons.size() << " merged reasons)\n" << std::endl;
}

void EventManager::shutdown(std::chrono::milliseconds /*timeout*/) {
    std::unique_ptr<ActiveClip> to_flush;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (current_clip_) {
            to_flush = std::move(current_clip_);
        }
    }
    if (to_flush) {
        std::cout << "[EVENT-MANAGER] Finalizing open clip on shutdown..." << std::endl;
        finalize_and_persist_clip(*to_flush);
    }
}

} // namespace engine::recorder

