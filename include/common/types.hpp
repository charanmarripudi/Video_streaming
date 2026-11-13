#ifndef COMMON_TYPES_HPP
#define COMMON_TYPES_HPP

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <atomic>
#include "sha256.hpp"

namespace engine {

// -----------------------------------------------------------------------------
// v2 Immutable Video Frame (Lock-Free Multi-Reader)
// -----------------------------------------------------------------------------
struct Frame {
    int64_t frame_index{0};
    int64_t pts_ns{0};      // Stream Presentation Timestamp (the true clock!)
    int64_t host_ns{0};     // Monotonic host receive time
    int width{0};
    int height{0};
    int channels{3};        // BGR
    std::shared_ptr<const std::vector<uint8_t>> data; // Immutable buffer

    bool is_valid() const {
        return data && !data->empty() && width > 0 && height > 0;
    }
};

using FramePtr = std::shared_ptr<const Frame>;

// -----------------------------------------------------------------------------
// Lock-Free Latest Frame Slot (Zero-Mutex Inference Hot-Path)
// -----------------------------------------------------------------------------
class LatestFrameSlot {
public:
    void publish(FramePtr f) noexcept {
        std::atomic_store_explicit(&slot_, std::move(f), std::memory_order_release);
    }

    FramePtr get() const noexcept {
        return std::atomic_load_explicit(&slot_, std::memory_order_acquire);
    }

private:
    FramePtr slot_{nullptr};
};

// -----------------------------------------------------------------------------
// Encoded Sample for Pre-Event Ring (Compressed GOP Storage, 300x Memory Saving!)
// -----------------------------------------------------------------------------
struct EncodedSample {
    std::shared_ptr<const std::vector<uint8_t>> payload;
    int64_t pts_ns{0};
    int64_t dts_ns{0};
    bool is_keyframe{false};
    size_t byte_size() const { return payload ? payload->size() : 0; }
};

// -----------------------------------------------------------------------------
// Stream Supervisor States
// -----------------------------------------------------------------------------
enum class StreamState {
    CONNECTING,
    STREAMING,
    DEGRADED,
    BACKOFF,
    FAILED
};

inline const char* stream_state_to_string(StreamState s) {
    switch (s) {
        case StreamState::CONNECTING: return "CONNECTING";
        case StreamState::STREAMING:  return "STREAMING";
        case StreamState::DEGRADED:   return "DEGRADED";
        case StreamState::BACKOFF:    return "BACKOFF";
        case StreamState::FAILED:     return "FAILED";
    }
    return "UNKNOWN";
}

// -----------------------------------------------------------------------------
// AI Inference & Event Types
// -----------------------------------------------------------------------------
struct BoundingBox {
    float x1{0.0f};
    float y1{0.0f};
    float x2{0.0f};
    float y2{0.0f};
    std::string label;
    float confidence{0.0f};
    bool in_roi{true};
};

struct Detection {
    std::string use_case_id;
    std::string camera_id;
    int64_t pts_ns{0};
    int64_t host_ns{0};
    float score{0.0f};
    std::string label;
    std::vector<BoundingBox> boxes;
};

// -----------------------------------------------------------------------------
// v2 Configuration Schema & Per-Component Fingerprints
// -----------------------------------------------------------------------------
struct UseCaseConfig {
    std::string use_case_id;
    bool enabled{true};
    std::string model_ref;             // e.g. "models/tt_crew_entering_below_tt.pt"
    float threshold{0.40f};
    int priority{1};                   // Higher priority kept during overload
    std::optional<int> analysis_fps;   // Decoupled inference rate
    std::optional<int> model_input_size;
    std::vector<std::string> enabled_classes;
    std::vector<std::pair<float, float>> roi_polygon;

    // Component Fingerprint: changing a model only restarts this use-case!
    std::string compute_fingerprint() const {
        std::ostringstream ss;
        ss << use_case_id << "|" << enabled << "|" << model_ref << "|" 
           << threshold << "|" << priority << "|" << analysis_fps.value_or(0);
        for (const auto& c : enabled_classes) ss << ":" << c;
        for (const auto& pt : roi_polygon) ss << ":" << pt.first << "," << pt.second;
        return crypto::SHA256::hash(ss.str());
    }
};

struct EventConfig {
    int pre_event_s{15};
    int post_event_s{15};
    int cooldown_s{30};                // Cooldown to prevent alert flooding
    int max_clip_s{120};               // Maximum duration for merged clips
    std::string output_format{"mp4"};
    std::optional<int> output_fps;
    int output_width{1280};
    int output_height{720};
    int output_bitrate_kbps{900};

    std::string compute_fingerprint() const {
        std::ostringstream ss;
        ss << pre_event_s << "|" << post_event_s << "|" << cooldown_s 
           << "|" << max_clip_s << "|" << output_format << "|" << output_fps.value_or(0);
        return crypto::SHA256::hash(ss.str());
    }
};

struct CameraConfig {
    std::string id;
    std::string name;
    bool enabled{true};
    std::string stream_url;
    std::string credentials_ref{"default"};
    std::string decode_mode{"hardware"}; // "hardware" or "software"
    int stall_timeout_s{5};

    std::optional<int> fps;
    int capture_width{1280};
    int capture_height{720};

    EventConfig event_config;
    std::vector<UseCaseConfig> assigned_use_cases;

    int get_effective_fps(int global_fps) const {
        return fps.value_or(global_fps);
    }

    int get_output_fps(int global_fps) const {
        return event_config.output_fps.value_or(get_effective_fps(global_fps));
    }

    // Capture Component Fingerprint
    std::string compute_capture_fingerprint(int global_fps) const {
        std::ostringstream ss;
        ss << id << "|" << enabled << "|" << stream_url << "|" << credentials_ref
           << "|" << decode_mode << "|" << get_effective_fps(global_fps)
           << "|" << capture_width << "x" << capture_height;
        return crypto::SHA256::hash(ss.str());
    }

    // Full Composite Hash
    std::string compute_hash(int global_fps) const {
        std::ostringstream ss;
        ss << compute_capture_fingerprint(global_fps) << "#RING#" 
           << event_config.compute_fingerprint();
        for (const auto& uc : assigned_use_cases) {
            if (uc.enabled) ss << "#UC#" << uc.compute_fingerprint();
        }
        return crypto::SHA256::hash(ss.str());
    }
};

struct HardwareConfig {
    std::string platform{"auto"};           // "auto", "rpi", "jetson", "software"
    std::string ai_accelerator{"cpu"};      // "tensorrt", "cuda", "openvino", "cpu"
    int device_id{0};
    bool enable_hardware_decode{true};
    bool enable_hardware_encode{true};
};

struct GlobalConfig {
    int global_fps{15};
    std::string storage_root{"storage/events"};
    size_t memory_budget_bytes{2ULL * 1024 * 1024 * 1024}; // 2 GB hard memory budget
    int max_inference_threads{4};                          // Bounded shared inference pool
    int max_writer_threads{2};                             // Bounded disk writer pool
    uint64_t min_free_disk_bytes{5ULL * 1024 * 1024 * 1024}; // 5 GB disk guard
    std::string default_encoder{"h264"};
    std::string platform{"auto"};
    HardwareConfig hardware;
    std::vector<CameraConfig> cameras;
};

} // namespace engine

#endif // COMMON_TYPES_HPP
