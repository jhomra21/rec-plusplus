#pragma once

#include "rec/rec.hpp"
#include "core/shape.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rec::core {

// With the main capture, 16 layers. Each display or window layer is its own capture.
inline constexpr std::size_t kMaxOverlays = 15;
static_assert(kMaxOverlays == rec::kMaxOverlays);

// Where a picture lands in the frame: destination edges in output pixels and the part of
// the picture shown, in 0-1 texture coordinates.
struct Placement {
    double left = 0, top = 0, right = 0, bottom = 0;
    double u0 = 0, v0 = 0, u1 = 1, v1 = 1;
};

// place() for a picture size in pixels that may be fractional (a cropped picture).
inline Placement place_fitted(Overlay::Kind kind, const Area& area, std::uint32_t frame_width, std::uint32_t frame_height,
                       double picture_width, double picture_height) {
    const double x = area.x * frame_width, y = area.y * frame_height;
    const double w = area.width * frame_width, h = area.height * frame_height;
    Placement p{x, y, x + w, y + h};
    if (picture_width <= 0 || picture_height <= 0 || w <= 0 || h <= 0) return p;
    const double box = w / h, picture = picture_width / picture_height;
    if (kind == Overlay::Kind::camera) {
        if (picture > box) { p.u0 = (1 - box / picture) / 2; p.u1 = 1 - p.u0; }
        else { p.v0 = (1 - picture / box) / 2; p.v1 = 1 - p.v0; }
    } else if (picture > box) {
        const double shown = w / picture;
        p.top = y + (h - shown) / 2;
        p.bottom = p.top + shown;
    } else {
        const double shown = h * picture;
        p.left = x + (w - shown) / 2;
        p.right = p.left + shown;
    }
    return p;
}

// Shows the crop (fractions of the picture) of a picture. A camera is cropped further to fill
// the rectangle; any other picture keeps its aspect ratio inside it.
inline Placement place(Overlay::Kind kind, const Area& area, std::uint32_t frame_width, std::uint32_t frame_height,
                       std::uint32_t picture_width, std::uint32_t picture_height, const Area& crop = {}) {
    const double x = area.x * frame_width, y = area.y * frame_height;
    const double w = area.width * frame_width, h = area.height * frame_height;
    Placement p{x, y, x + w, y + h, crop.x, crop.y, crop.x + crop.width, crop.y + crop.height};
    if (!picture_width || !picture_height || w <= 0 || h <= 0) return p;
    p = place_fitted(kind, area, frame_width, frame_height, picture_width * crop.width, picture_height * crop.height);
    p.u0 = crop.x + p.u0 * crop.width;
    p.u1 = crop.x + p.u1 * crop.width;
    p.v0 = crop.y + p.v0 * crop.height;
    p.v1 = crop.y + p.v1 * crop.height;
    return p;
}

inline bool valid(const Area& a) {
    constexpr double slack = 1e-6;
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.width) && std::isfinite(a.height) &&
           a.x >= 0 && a.y >= 0 && a.width >= 0.01 && a.height >= 0.01 && a.x + a.width <= 1 + slack &&
           a.y + a.height <= 1 + slack;
}

inline std::string check_area(std::size_t index, const Area& a) {
    if (valid(a)) return {};
    return "overlay " + std::to_string(index + 1) + " must lie inside the frame: x, y, width and height are fractions of it";
}

inline std::string check_crop(const std::string& name, const Area& a) {
    if (valid(a)) return {};
    return name + ": the crop must lie inside the picture: x, y, width and height are fractions of it";
}

inline std::string layer_name(std::size_t layer) {
    return layer == SIZE_MAX ? std::string("the screen") : "overlay " + std::to_string(layer + 1);
}

inline std::string check_layout(std::size_t layer, const Layout& l) {
    if (!valid(l.area)) return layer_name(layer) + " must lie inside the canvas: x, y, width and height are fractions of it";
    return check_crop(layer_name(layer), l.crop);
}

// The second canvas's size, layouts and output; empty when it is off or fine.
inline std::string check_canvas(const Options& o) {
    const auto& c = o.canvas2;
    if (!c.width && !c.height) {
        if (!c.stream_urls.empty() || !c.output.empty()) return "the second canvas needs a size";
        return {};
    }
    if (c.width < 64 || c.height < 64 || c.width > 4096 || c.height > 4096 || c.width % 2 || c.height % 2)
        return "the second canvas must be even, 64 to 4096 pixels on each side";
    if (c.overlays.size() > o.overlays.size()) return "the second canvas places more overlays than the recording has";
    if (auto e = check_layout(SIZE_MAX, c.screen); !e.empty()) return "second canvas: " + e;
    for (std::size_t i = 0; i < c.overlays.size(); ++i)
        if (auto e = check_layout(i, c.overlays[i]); !e.empty()) return "second canvas: " + e;
    if (o.stream_urls.size() + c.stream_urls.size() > kMaxStreams)
        return "at most " + std::to_string(kMaxStreams) + " stream servers across both canvases";
    if (!c.output.empty()) {
        if (c.output.size() < 4 || c.output.substr(c.output.size() - 4) != ".mp4") return "the second canvas's movie must end with .mp4";
        if (c.output == o.output) return "the second canvas's movie needs its own file";
    }
    return {};
}

// The second canvas's layout of overlay index: its own, or by default the main canvas's.
inline Layout canvas_layout(const Options& o, std::size_t index) {
    if (index < o.canvas2.overlays.size()) return o.canvas2.overlays[index];
    const auto& overlay = o.overlays[index];
    return {overlay.visible, overlay.area, overlay.crop};
}

inline std::string check_overlays(const std::vector<Overlay>& overlays) {
    if (overlays.size() > kMaxOverlays) return "at most " + std::to_string(kMaxOverlays) + " overlays";
    for (std::size_t i = 0; i < overlays.size(); ++i) {
        const auto& o = overlays[i];
        const auto name = "overlay " + std::to_string(i + 1);
        switch (o.kind) {
        case Overlay::Kind::image:
            if (o.source.empty()) return name + " needs an image file";
            break;
        case Overlay::Kind::camera:
            if (o.source.empty()) return name + " needs a camera";
            break;
        case Overlay::Kind::display:
            if (!o.display) return name + " needs a display number";
            if (o.region && (o.region->x < 0 || o.region->y < 0 || o.region->width < 2 || o.region->height < 2))
                return name + ": the region must be at least 2x2 pixels inside the display";
            break;
        case Overlay::Kind::window:
            if (!o.window) return name + " needs a window";
            if (o.region) return name + ": a region applies to a display";
            break;
        }
        if (auto error = check_area(i, overlays[i].area); !error.empty()) return error;
        if (auto error = check_crop(name, overlays[i].crop); !error.empty()) return error;
        if (o.kind != Overlay::Kind::camera && !plain(o.style)) return name + ": shapes, borders and shadows are for a camera";
        if (auto error = check_style(o.style); !error.empty()) return name + ": " + error;
    }
    return {};
}

// BT.709 studio-range YCbCr code values to 8-bit RGB.
inline Probe::Color studio_rgb(double y, double cb, double cr) {
    const double l = (y - 16) / 219, u = (cb - 128) / 224, v = (cr - 128) / 224;
    auto byte = [](double c) { return static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0, 1.0) * 255)); };
    return {byte(l + 1.5748 * v), byte(l - 0.1873 * u - 0.4681 * v), byte(l + 1.8556 * u)};
}

// A synthetic camera frame: green with a white bar that crosses once a second, BGRA.
inline constexpr std::uint32_t kTestCameraWidth = 640, kTestCameraHeight = 360, kTestCameraFps = 30;
inline void test_camera_frame(std::uint64_t frame, std::uint8_t* bgra, std::size_t pitch) {
    const auto bar = static_cast<std::uint32_t>(frame % kTestCameraFps * kTestCameraWidth / kTestCameraFps);
    for (std::uint32_t y = 0; y < kTestCameraHeight; ++y) {
        auto* row = bgra + y * pitch;
        for (std::uint32_t x = 0; x < kTestCameraWidth; ++x) {
            const bool white = x >= bar && x < bar + 32;
            row[x * 4 + 0] = white ? 255 : 0;
            row[x * 4 + 1] = white ? 255 : 170;
            row[x * 4 + 2] = white ? 255 : 0;
            row[x * 4 + 3] = 255;
        }
    }
}

} // namespace rec::core
