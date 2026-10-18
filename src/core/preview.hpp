#pragma once

#include "rec/rec.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace rec::core {

constexpr std::uint32_t kPreviewRate = 30;  // Frames a second at most.

struct PreviewSize {
    std::uint32_t width = 0, height = 0;
};

// Preview frames keep the output's shape: max_width wide at most, both sides even.
inline PreviewSize preview_size(std::uint32_t width, std::uint32_t height, std::uint32_t max_width) {
    if (!width || !height || !max_width) return {};
    const std::uint32_t w = std::max<std::uint32_t>(2, std::min(width, max_width) & ~1u);
    const auto h = static_cast<std::uint32_t>((static_cast<std::uint64_t>(w) * height + width / 2) / width);
    return {w, std::max<std::uint32_t>(2, h & ~1u)};
}

// The newest preview frame, written by the recording thread and copied out by the UI. It
// holds one frame; a frame nobody takes is replaced by the next.
class PreviewBuffer {
public:
    // Before recording starts; a zero size turns the preview off.
    void reset(PreviewSize size) {
        std::lock_guard lock(mutex_);
        size_ = size;
        frame_ = {};
        last_tick_ = UINT64_MAX;
    }
    PreviewSize size() const { return size_; }
    void show(bool on) { on_.store(on); }
    // Called by the recording thread for each output slot: whether to make a preview frame.
    bool due(std::uint64_t slot, std::uint32_t fps) {
        if (!size_.width || !on_.load()) return false;
        const std::uint64_t tick = slot * kPreviewRate / fps;
        if (tick == last_tick_) return false;
        last_tick_ = tick;
        return true;
    }
    // size().height rows of BGRA, top first, stride bytes apart.
    void publish(const std::uint8_t* rows, std::size_t stride) {
        std::lock_guard lock(mutex_);
        const std::size_t row = static_cast<std::size_t>(size_.width) * 4;
        frame_.width = size_.width;
        frame_.height = size_.height;
        frame_.pixels.resize(row * size_.height);
        for (std::uint32_t y = 0; y < size_.height; ++y)
            std::memcpy(frame_.pixels.data() + y * row, rows + y * stride, row);
        ++frame_.number;
    }
    bool take(PreviewFrame& out) const {
        std::lock_guard lock(mutex_);
        if (!frame_.number || frame_.number == out.number) return false;
        out.width = frame_.width;
        out.height = frame_.height;
        out.number = frame_.number;
        out.pixels.assign(frame_.pixels.begin(), frame_.pixels.end());
        return true;
    }

private:
    mutable std::mutex mutex_;
    PreviewSize size_{};  // Set before recording starts.
    std::atomic_bool on_{true};
    std::uint64_t last_tick_ = UINT64_MAX;  // Recording thread only.
    PreviewFrame frame_;
};

} // namespace rec::core
