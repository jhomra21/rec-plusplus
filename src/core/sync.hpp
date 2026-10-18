#pragma once

#include "rec/rec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Synthetic test signals and their measurement, shared by every platform so CI
// results are comparable.
namespace rec::core {

inline constexpr double kTwoPi = 6.283185307179586;

// The test pattern is bright for the first 100 ms (at least one frame) of every second.
inline bool test_flash(std::uint64_t slot, std::uint32_t fps) {
    const auto flash_slots = std::max<std::uint64_t>(1, fps / 10);
    return slot % fps < flash_slots;
}

// A 100 ms 440 Hz beep starting with every one-second flash, at 48 kHz.
inline float test_tone(std::uint64_t sample) {
    const auto index = sample % 48000;
    if (index >= 4800) return 0.0f;
    return static_cast<float>(std::sin(kTwoPi * 440.0 * static_cast<double>(index) / 48000.0) * (8000.0 / 32768.0));
}

// A continuous quiet 1 kHz "microphone", far below the onset threshold.
inline float test_mic(std::uint64_t sample) {
    return static_cast<float>(std::sin(kTwoPi * 1000.0 * static_cast<double>(sample) / 48000.0) * (300.0 / 32768.0));
}

// Flash onsets: a frame brighter than 80% after a darker frame.
class FlashDetector {
public:
    void feed(double time, double brightness) {
        const bool now = brightness > 0.8;
        if (now && !bright_) onsets.push_back(time);
        bright_ = now;
    }
    std::vector<double> onsets;

private:
    bool bright_ = false;
};

// Beep onsets: the first sample above 0.1 after at least 200 ms without one.
class OnsetDetector {
public:
    void feed(double time, float peak) {
        if (peak < 0.1f) return;
        if (time - last_loud_ >= 0.2) onsets.push_back(time);
        last_loud_ = time;
    }
    std::vector<double> onsets;

private:
    double last_loud_ = -1e9;
};

// Pairs each flash with the nearest beep within 500 ms and fills the probe's sync fields.
inline void measure_sync(const std::vector<double>& flashes, const std::vector<double>& beeps, Probe& probe) {
    probe.flashes = flashes;
    probe.beeps = beeps;
    double total = 0;
    for (const double flash : flashes) {
        double best = 1e9;
        for (const double beep : beeps)
            if (std::fabs(beep - flash) < std::fabs(best)) best = beep - flash;
        if (std::fabs(best) > 0.5) continue;
        ++probe.sync_events;
        total += best * 1000.0;
        probe.av_offset_max_ms = std::max(probe.av_offset_max_ms, std::fabs(best) * 1000.0);
    }
    if (probe.sync_events) probe.av_offset_ms = total / probe.sync_events;
}

} // namespace rec::core
