#ifndef RECORDER_EVENT_RECORDER_HPP
#define RECORDER_EVENT_RECORDER_HPP

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <condition_variable>
#include "common/types.hpp"
#include "buffer/circular_ring_buffer.hpp"

namespace engine::recorder {

struct RecordingTask {
    std::string camera_id;
    std::string use_case_id;
    int64_t trigger_frame_index;
    std::vector<RawFrame> pre_event_frames;
    int post_event_remaining;
    std::vector<RawFrame> post_event_frames;
    int output_fps;
    int width;
    int height;
    std::string output_path;
};

class EventRecorder {
public:
    EventRecorder(CameraConfig cam_config, int global_fps, HardwareConfig hardware);
    ~EventRecorder();

    void start();
    void stop();

    // Triggered by an AI Use-Case Worker on violation
    void trigger_event(const std::string& use_case_id, 
                      const RawFrame& trigger_frame,
                      const std::shared_ptr<buffer::CircularRingBuffer>& ring_buffer);

    // Feeds incoming live frames to accumulate post-event duration
    void on_frame_arrived(const RawFrame& frame);

private:
    void writer_loop(std::stop_token st);
    void flush_task_to_disk(const RecordingTask& task);

    CameraConfig cam_config_;
    int global_fps_;
    int effective_fps_;
    int output_fps_;
    HardwareConfig hardware_;

    std::mutex task_mutex_;
    std::vector<std::shared_ptr<RecordingTask>> active_tasks_;
    std::queue<RecordingTask> completed_queue_;
    std::condition_variable_any cv_writer_;
    std::jthread writer_thread_;
    std::atomic<bool> is_running_{false};
};

} // namespace engine::recorder

#endif // RECORDER_EVENT_RECORDER_HPP

