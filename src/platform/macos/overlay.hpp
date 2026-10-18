#pragma once

#include "rec/rec.hpp"

#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rec::mac {

// Composites each frame with Core Image on the GPU, into IOSurface-backed NV12 buffers
// from a small pool: the main capture in its area, then the image, camera, display and
// window overlays. The recording thread calls compose(); set() and set_screen() may come
// from any thread. Each camera and each display or window stream delivers on its own queue
// and only replaces its newest retained frame.
class Overlays {
public:
    Overlays();
    ~Overlays();
    Overlays(const Overlays&) = delete;
    Overlays& operator=(const Overlays&) = delete;

    // Uses options.overlays, screen_area, screen_visible, cursor and fps.
    Result start(std::uint32_t width, std::uint32_t height, const Options& options);
    // A retained frame to encode: base in its area with the visible overlays on top, or
    // base itself (retained) when it fills the frame and no overlay is visible. Null when
    // drawing failed.
    CVPixelBufferRef compose(CVPixelBufferRef base);
    Result set(std::size_t index, bool visible, const std::optional<Area>& area);
    Result set_screen(bool visible, const std::optional<Area>& area);
    std::uint64_t camera_frames() const;
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

std::vector<Camera> cameras();

} // namespace rec::mac
