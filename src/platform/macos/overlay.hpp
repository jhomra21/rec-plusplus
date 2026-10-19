#pragma once

#include "rec/rec.hpp"

#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rec::mac {

// Where the main capture is on the desktop, for click marks: a display (or a region of it in
// its pixels) or a window.
struct ClickSource {
    std::uint32_t display = 0;  // CGDirectDisplayID
    std::uint32_t window = 0;   // CGWindowID
    std::optional<Region> region;
};

// Composites each frame with Core Image on the GPU, into IOSurface-backed NV12 buffers
// from a small pool: the main capture in its area, click marks, then the image, camera,
// display and window overlays. Each canvas (the main one and, with options.canvas2, a
// second one) has its own size, pool and layout of the same sources. The recording thread
// composes the main canvas and another thread may compose the second one at the same time;
// set(), set_screen(), set_layout() and set_style() may come from any thread. Each camera and each display or window stream
// delivers on its own queue and only replaces its newest retained frame. With
// options.clicks, compose() reads the system's mouse-down counters, so a press shorter than
// a frame still shows and no input permission is needed.
class Overlays {
public:
    Overlays();
    ~Overlays();
    Overlays(const Overlays&) = delete;
    Overlays& operator=(const Overlays&) = delete;

    // Uses options.overlays, screen_area, screen_visible, screen_crop, canvas2, cursor, clicks
    // and fps. width and height are the main canvas's.
    Result start(std::uint32_t width, std::uint32_t height, const Options& options, const ClickSource& clicks = {});
    // A retained frame of a canvas (kMainCanvas or kSecondCanvas) to encode: base in its
    // area with the visible overlays on top, or, for the main canvas, base itself (retained)
    // when it fills the frame and nothing else shows. Null when drawing failed or, for the
    // second canvas, when all its frames are still with its encoder. The main canvas reads the
    // clicks; the second one uses what it found last.
    CVPixelBufferRef compose(std::size_t canvas, CVPixelBufferRef base);
    // The main canvas: shows, hides or moves an overlay or the main capture, keeping its crop.
    Result set(std::size_t index, bool visible, const std::optional<Area>& area);
    Result set_screen(bool visible, const std::optional<Area>& area);
    // layer: an index into options.overlays, or kScreenLayer.
    Result set_layout(std::size_t canvas, std::size_t layer, const Layout& layout);
    // A camera's shape, border and shadow, in both canvases.
    Result set_style(std::size_t index, const Style& style);
    std::uint64_t camera_frames() const;
    bool camera_lost() const;  // A camera stopped delivering frames.
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

std::vector<Camera> cameras();

} // namespace rec::mac
