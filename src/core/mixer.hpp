#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rec::core {

inline constexpr std::int32_t kMixRate = 48000;

// Raises a level meter to the peak of interleaved samples. The reader takes it with exchange(0).
inline void raise_peak(std::atomic<float>& meter, const float* samples, std::size_t count) {
    float peak = 0;
    for (std::size_t i = 0; i < count; ++i) peak = std::max(peak, std::fabs(samples[i]));
    float seen = meter.load(std::memory_order_relaxed);
    while (peak > seen && !meter.compare_exchange_weak(seen, peak, std::memory_order_relaxed)) {}
}

inline float gain_of(double db, bool muted) { return muted ? 0.0f : static_cast<float>(std::pow(10.0, db / 20.0)); }

// A source's live volume. set() may run on any thread; next() belongs to the recording thread
// and returns the gain to ramp across the buffer it is about to place, so a change never
// steps mid-signal (no click).
class Fader {
public:
    struct Ramp {
        float from = 1, to = 1;
        bool unity() const { return from == 1 && to == 1; }
        // Scales interleaved stereo in place.
        void apply(float* stereo, std::size_t frames) const {
            const float step = frames ? (to - from) / static_cast<float>(frames) : 0;
            for (std::size_t i = 0; i < frames; ++i) {
                const float g = from + step * static_cast<float>(i + 1);
                stereo[i * 2] *= g;
                stereo[i * 2 + 1] *= g;
            }
        }
    };
    void reset(float gain) { target_.store(gain); current_ = gain; }
    void set(float gain) { target_.store(gain); }
    Ramp next() {
        const float target = target_.load();
        const Ramp ramp{current_, target};
        current_ = target;
        return ramp;
    }

private:
    std::atomic<float> target_{1};
    float current_ = 1;
};

// Sums two sources on a 48 kHz stereo timeline whose sample 0 is the recording origin,
// then limits the result below -1 dBFS instead of clipping. Not thread-safe; the
// recording thread owns it.
class Mixer {
public:
    void reset(std::size_t capacity) {
        buffer_.assign(capacity * 2, 0.0f);
        capacity_ = static_cast<std::int64_t>(capacity);
        emitted_ = 0;
        ends_ = {0, 0};
        gain_ = 1;
    }
    std::int64_t emitted() const { return emitted_; }
    std::int64_t capacity() const { return capacity_; }
    std::int64_t end(std::size_t source) const { return ends_[source]; }

    // Frames before the emitted point or beyond the window are discarded.
    void add(std::size_t source, std::int64_t start, const float* stereo, std::size_t frames) {
        const auto stop = start + static_cast<std::int64_t>(frames);
        ends_[source] = std::max(ends_[source], stop);
        const auto first = std::max(start, emitted_);
        const auto last = std::min(stop, emitted_ + capacity_);
        for (auto i = first; i < last; ++i) {
            const auto slot = static_cast<std::size_t>(i % capacity_) * 2;
            const auto from = static_cast<std::size_t>(i - start) * 2;
            buffer_[slot] += stereo[from];
            buffer_[slot + 1] += stereo[from + 1];
        }
    }

    void take(std::size_t frames, std::vector<float>& out) {
        out.resize(frames * 2);
        for (std::size_t i = 0; i < frames; ++i) {
            const auto slot = static_cast<std::size_t>((emitted_ + static_cast<std::int64_t>(i)) % capacity_) * 2;
            const float left = buffer_[slot], right = buffer_[slot + 1];
            const float peak = std::max(std::fabs(left), std::fabs(right));
            const float needed = peak > kCeiling ? kCeiling / peak : 1.0f;
            gain_ = needed < gain_ ? needed : std::min(needed, gain_ + (1.0f - gain_) * kRelease);
            out[i * 2] = left * gain_;
            out[i * 2 + 1] = right * gain_;
            buffer_[slot] = buffer_[slot + 1] = 0;
        }
        emitted_ += static_cast<std::int64_t>(frames);
    }

private:
    static constexpr float kCeiling = 0.891f;  // -1 dBFS
    static constexpr float kRelease = 1.0f / (0.05f * kMixRate);  // About 50 ms back to unity.
    std::vector<float> buffer_;
    std::int64_t capacity_ = 1;
    std::int64_t emitted_ = 0;
    std::array<std::int64_t, 2> ends_{};
    float gain_ = 1;
};

} // namespace rec::core
