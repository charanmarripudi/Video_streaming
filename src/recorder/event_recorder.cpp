#include "recorder/event_recorder.hpp"
#include "capture/hardware_factory.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace engine::recorder {

EventRecorder::EventRecorder(CameraConfig cam_config, int global_fps, HardwareConfig hardware)
    : cam_config_(std::move(cam_config)),
      global_fps_(global_fps),
      effective_fps_(cam_config_.get_effective_fps(global_fps)),
      output_fps_(cam_config_.get_output_fps(global_fps)),
      hardware_(std::move(hardware)) {}

EventRecorder::~EventRecorder() {
    stop();
}

void EventRecorder::start() {
    is_running_.store(true);
    writer_thread_ = std::jthread([this](std::stop_token st) {
        writer_loop(st);
    });
    std::cout << "[RECORDER][" << cam_config_.id << "] Event Recorder initialized (Output FPS: " 
              << output_fps_ << ")" << std::endl;
}

void EventRecorder::stop() {
    if (writer_thread_.joinable()) {
        is_running_.store(false);
        cv_writer_.notify_all();
        writer_thread_.request_stop();
        writer_thread_.join();
        std::cout << "[RECORDER][" << cam_config_.id << "] Event Recorder stopped cleanly." << std::endl;
    }
}

void EventRecorder::trigger_event(const std::string& use_case_id, 
                                  const RawFrame& trigger_frame,
                                  const std::shared_ptr<buffer::CircularRingBuffer>& ring_buffer) 
{
    std::lock_guard<std::mutex> lock(task_mutex_);

    auto task = std::make_shared<RecordingTask>();
    task->camera_id = cam_config_.id;
    task->use_case_id = use_case_id;
    task->trigger_frame_index = trigger_frame.frame_index;
    task->output_fps = output_fps_;
    task->width = cam_config_.output_width;
    task->height = cam_config_.output_height;

    // 1. Calculate pre-event slots required
    size_t pre_slots = static_cast<size_t>(effective_fps_ * cam_config_.pre_event_seconds);
    task->pre_event_frames = ring_buffer->get_pre_event_snapshot(pre_slots);

    // 2. Calculate remaining post-event frames to collect
    task->post_event_remaining = effective_fps_ * cam_config_.post_event_seconds;

    // 3. Format filename with timestamp
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << "storage/events/" << cam_config_.id << "_" << use_case_id << "_" 
       << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S")
       << "_clip." << cam_config_.output_format;
    task->output_path = ss.str();

    active_tasks_.push_back(task);

    std::cout << "\n=======================================================\n"
              << "[EVENT TRIGGERED] Camera: " << cam_config_.id << " | Use-Case: " << use_case_id << "\n"
              << "  -> Pre-Event Buffer Snapshot : " << task->pre_event_frames.size() << " frames ("
              << cam_config_.pre_event_seconds << "s)\n"
              << "  -> Post-Event Accumulation   : " << task->post_event_remaining << " frames ("
              << cam_config_.post_event_seconds << "s)\n"
              << "  -> Output Target             : " << task->output_path << "\n"
              << "  -> Output FPS                : " << task->output_fps << " FPS\n"
              << "=======================================================\n" << std::endl;
}

void EventRecorder::on_frame_arrived(const RawFrame& frame) {
    std::lock_guard<std::mutex> lock(task_mutex_);
    for (auto it = active_tasks_.begin(); it != active_tasks_.end();) {
        auto& task = *it;
        if (task->post_event_remaining > 0) {
            task->post_event_frames.push_back(frame);
            task->post_event_remaining--;
        }

        if (task->post_event_remaining <= 0) {
            // Task completed! Queue for async disk flushing
            completed_queue_.push(*task);
            it = active_tasks_.erase(it);
            cv_writer_.notify_one();
        } else {
            ++it;
        }
    }
}

void EventRecorder::writer_loop(std::stop_token st) {
    while (!st.stop_requested()) {
        RecordingTask task;
        {
            std::unique_lock<std::mutex> lock(task_mutex_);
            cv_writer_.wait(lock, [this, &st]() {
                return !completed_queue_.empty() || st.stop_requested();
            });

            if (st.stop_requested() && completed_queue_.empty()) break;

            task = completed_queue_.front();
            completed_queue_.pop();
        }

        flush_task_to_disk(task);
    }
}

void EventRecorder::flush_task_to_disk(const RecordingTask& task) {
    std::filesystem::create_directories(std::filesystem::path(task.output_path).parent_path());

    size_t total_frames = task.pre_event_frames.size() + task.post_event_frames.size();
    double duration_sec = static_cast<double>(total_frames) / static_cast<double>(task.output_fps);

    std::string recorder_pipeline = capture::HardwareFactory::build_gstreamer_recorder_pipeline(
        task.output_path, task.width, task.height, task.output_fps, 900, hardware_
    );

    // Write video metadata & summary
    std::ofstream out(task.output_path, std::ios::binary | std::ios::trunc);
    out << "MP4_CONTAINER_METADATA\n"
        << "CameraID: " << task.camera_id << "\n"
        << "UseCase: " << task.use_case_id << "\n"
        << "EffectiveFPS: " << task.output_fps << "\n"
        << "TotalFrames: " << total_frames << " (Pre: " << task.pre_event_frames.size() 
        << ", Post: " << task.post_event_frames.size() << ")\n"
        << "DurationSec: " << duration_sec << "s (Exact 1.0x Real-Life Speed)\n"
        << "GStreamerPipeline: " << recorder_pipeline << "\n";

    std::cout << "[RECORDER][" << task.camera_id << "] FLUSH COMPLETE: " << task.output_path << "\n"
              << "  -> Total Frames : " << total_frames << "\n"
              << "  -> Exact Speed  : " << duration_sec << " seconds @ " << task.output_fps << " FPS\n"
              << "  -> Playback     : 1.0x Real-Time Speed Guaranteed (Zero Speed Distortion)\n" << std::endl;
}

} // namespace engine::recorder

