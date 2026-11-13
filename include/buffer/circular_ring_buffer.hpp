#ifndef BUFFER_CIRCULAR_RING_BUFFER_HPP
#define BUFFER_CIRCULAR_RING_BUFFER_HPP

#include <vector>
#include <shared_mutex>
#include <mutex>
#include <algorithm>
#include <iostream>
#include "common/types.hpp"

namespace engine::buffer {

class CircularRingBuffer {
public:
    explicit CircularRingBuffer(size_t capacity)
        : capacity_(std::max<size_t>(capacity, 10)), buffer_(capacity_) {}

    // Single Capture Thread writes newly acquired frames
    void push(const RawFrame& frame) {
        std::unique_lock<std::shared_mutex> lock(rw_mutex_);
        buffer_[head_] = frame;
        head_ = (head_ + 1) % capacity_;
        ++total_pushed_;
    }

    // Multiple Use-Case Workers read the latest frame without deep-copying memory
    RawFrame get_latest() const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        if (total_pushed_ == 0) return {};
        size_t latest_idx = (head_ == 0) ? (capacity_ - 1) : (head_ - 1);
        return buffer_[latest_idx];
    }

    // Event Recorder snapshots historical pre-event frames on violation trigger
    std::vector<RawFrame> get_pre_event_snapshot(size_t count) const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        std::vector<RawFrame> snapshot;
        
        size_t available = std::min<size_t>(total_pushed_, capacity_);
        size_t actual_to_fetch = std::min<size_t>(count, available);
        snapshot.reserve(actual_to_fetch);

        if (actual_to_fetch == 0) return snapshot;

        // Traverse backwards from the latest frame to collect historical order
        size_t start_idx = (head_ + capacity_ - actual_to_fetch) % capacity_;
        for (size_t i = 0; i < actual_to_fetch; ++i) {
            size_t idx = (start_idx + i) % capacity_;
            if (buffer_[idx].is_valid()) {
                snapshot.push_back(buffer_[idx]);
            }
        }
        return snapshot;
    }

    size_t capacity() const { return capacity_; }
    int64_t total_pushed() const {
        std::shared_lock<std::shared_mutex> lock(rw_mutex_);
        return total_pushed_;
    }

private:
    const size_t capacity_;
    std::vector<RawFrame> buffer_;
    size_t head_{0};
    int64_t total_pushed_{0};
    mutable std::shared_mutex rw_mutex_;
};

} // namespace engine::buffer

#endif // BUFFER_CIRCULAR_RING_BUFFER_HPP

