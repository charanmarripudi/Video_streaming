#ifndef BUFFER_PRE_EVENT_RING_HPP
#define BUFFER_PRE_EVENT_RING_HPP

#include <vector>
#include <deque>
#include <mutex>
#include <cstdint>
#include <iostream>
#include "common/types.hpp"

namespace engine::buffer {

class PreEventRing {
public:
    explicit PreEventRing(size_t max_bytes, int64_t window_ns)
        : max_bytes_(max_bytes), window_ns_(window_ns) {}

    // Ingests an encoded H.264/H.265 sample (single producer: capture thread)
    void push(EncodedSample sample) {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        current_bytes_ += sample.byte_size();
        samples_.push_back(std::move(sample));

        int64_t latest_pts = samples_.back().pts_ns;
        int64_t oldest_allowed_pts = latest_pts - window_ns_;

        // Evict expired samples by window time or memory budget
        while (samples_.size() > 1 && 
              (samples_.front().pts_ns < oldest_allowed_pts || current_bytes_ > max_bytes_)) {
            // Guarantee that we don't evict the only keyframe needed for decodability
            current_bytes_ -= samples_.front().byte_size();
            samples_.pop_front();
        }
    }

    // Takes a zero-copy pointer snapshot from the nearest keyframe prior to trigger PTS
    std::vector<EncodedSample> snapshot_from(int64_t trigger_pts_ns) const {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        std::vector<EncodedSample> snapshot;
        if (samples_.empty()) return snapshot;

        // 1. Find the sample index corresponding to or preceding trigger_pts - window_ns
        int64_t start_pts = trigger_pts_ns - window_ns_;
        size_t start_idx = 0;
        for (size_t i = 0; i < samples_.size(); ++i) {
            if (samples_[i].pts_ns >= start_pts) {
                start_idx = i;
                break;
            }
        }

        // 2. Walk backwards to the nearest preceding Keyframe (IDR frame)
        size_t keyframe_idx = start_idx;
        while (keyframe_idx > 0 && !samples_[keyframe_idx].is_keyframe) {
            keyframe_idx--;
        }

        // 3. Collect pointer references (Zero memory copied!)
        snapshot.reserve(samples_.size() - keyframe_idx);
        for (size_t i = keyframe_idx; i < samples_.size(); ++i) {
            snapshot.push_back(samples_[i]);
        }

        return snapshot;
    }

    size_t current_bytes() const {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        return current_bytes_;
    }

    size_t sample_count() const {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        return samples_.size();
    }

private:
    const size_t max_bytes_;
    const int64_t window_ns_;
    mutable std::mutex ring_mutex_;
    std::deque<EncodedSample> samples_;
    size_t current_bytes_{0};
};

} // namespace engine::buffer

#endif // BUFFER_PRE_EVENT_RING_HPP

