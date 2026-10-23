#pragma once

#include "core/overlay.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rec::core {

// A rectangle on the desktop, in the units the platform reports the pointer in.
struct DesktopRect {
    double x = 0, y = 0, width = 0, height = 0;
};

// A click mark to draw: its center and diameter in output pixels, its opacity (0 to 1,
// applied to premultiplied pixels) and the button.
struct ClickMark {
    double x = 0, y = 0, diameter = 0, alpha = 0;
    bool right = false;
};

// Recent mouse presses, recorded from an input thread and drawn by the compositor. Fixed
// size: a ninth press replaces the oldest.
class Clicks {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kMax = 8;

    // x and y are desktop coordinates.
    void press(Clock::time_point at, double x, double y, bool right) {
        std::lock_guard lock(mutex_);
        auto& c = presses_[next_++ % kMax];
        c = {at, Clock::time_point::max(), x, y, right, true};
    }

    // Ends the newest held press of that button.
    void release(Clock::time_point at, bool right) {
        std::lock_guard lock(mutex_);
        Press* newest = nullptr;
        for (auto& c : presses_)
            if (c.used && c.right == right && c.released == Clock::time_point::max() && (!newest || c.pressed > newest->pressed))
                newest = &c;
        if (newest) newest->released = std::max(at, newest->pressed);
    }

    // The marks shown at time at for a capture of source (desktop units) that is fitted into a
    // width x height picture (centered, keeping its shape), whose crop is placed in area of a
    // canvas_width x canvas_height canvas. Returns how many were written to out.
    std::size_t marks(Clock::time_point at, const DesktopRect& source, std::uint32_t width, std::uint32_t height,
                      std::uint32_t canvas_width, std::uint32_t canvas_height, const Area& area, const Area& crop,
                      std::array<ClickMark, kMax>& out) const {
        if (source.width <= 0 || source.height <= 0 || !width || !height) return 0;
        const double scale = std::min(width / source.width, height / source.height);
        const double fit_w = source.width * scale, fit_h = source.height * scale;
        const double fit_x = (width - fit_w) / 2, fit_y = (height - fit_h) / 2;
        const auto p = place(Overlay::Kind::image, area, canvas_width, canvas_height, width, height, crop);
        if (p.u1 <= p.u0 || p.v1 <= p.v0) return 0;
        const double diameter = std::max(16.0, 0.06 * (p.bottom - p.top) / (p.v1 - p.v0));
        std::size_t count = 0;
        std::lock_guard lock(mutex_);
        for (const auto& c : presses_) {
            if (!c.used || at < c.pressed) continue;
            const double age = std::chrono::duration<double>(at - c.pressed).count();
            double grow = 0.75 + 0.25 * std::min(1.0, age / kGrowSeconds), alpha = 1;
            if (at >= c.released) {
                const double fade = std::chrono::duration<double>(at - c.released).count() / kFadeSeconds;
                if (fade >= 1) continue;
                alpha = 1 - fade;
                grow *= 1 + 0.25 * fade;
            }
            const double u = (c.x - source.x) / source.width, v = (c.y - source.y) / source.height;
            if (u < 0 || v < 0 || u > 1 || v > 1) continue;
            // Where the press lands in the shown part of the picture.
            const double su = ((fit_x + u * fit_w) / width - p.u0) / (p.u1 - p.u0);
            const double sv = ((fit_y + v * fit_h) / height - p.v0) / (p.v1 - p.v0);
            if (su < 0 || sv < 0 || su > 1 || sv > 1) continue;
            out[count++] = {p.left + su * (p.right - p.left), p.top + sv * (p.bottom - p.top), diameter * grow, alpha, c.right};
        }
        return count;
    }

private:
    static constexpr double kGrowSeconds = 0.1, kFadeSeconds = 0.35;
    struct Press {
        Clock::time_point pressed{}, released{};
        double x = 0, y = 0;
        bool right = false, used = false;
    };
    mutable std::mutex mutex_;
    std::array<Press, kMax> presses_{};
    std::size_t next_ = 0;
};

// The click mark picture, size x size premultiplied BGRA: a translucent disc inside a solid
// ring, yellow for the left button and blue for the right.
inline std::vector<std::uint8_t> click_mark_pixels(std::uint32_t size, bool right) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size) * size * 4);
    const double r = right ? 30 : 255, g = right ? 144 : 200, b = right ? 255 : 0;
    const double outer = size / 2.0, ring = size * 0.09, half = size / 2.0;
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const double d = std::hypot(x + 0.5 - half, y + 0.5 - half);
            // One pixel of antialiasing at each edge.
            const double inside = std::clamp(outer - d, 0.0, 1.0);
            const double in_ring = inside * std::clamp(d - (outer - ring) + 0.5, 0.0, 1.0);
            const double a = std::clamp(in_ring * 0.95 + (inside - in_ring) * 0.5, 0.0, 1.0);
            auto* px = &pixels[(static_cast<std::size_t>(y) * size + x) * 4];
            px[0] = static_cast<std::uint8_t>(std::lround(b * a));
            px[1] = static_cast<std::uint8_t>(std::lround(g * a));
            px[2] = static_cast<std::uint8_t>(std::lround(r * a));
            px[3] = static_cast<std::uint8_t>(std::lround(255 * a));
        }
    return pixels;
}

} // namespace rec::core
