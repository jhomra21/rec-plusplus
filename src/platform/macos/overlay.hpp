#pragma once

#include "rec/rec.hpp"

#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rec::mac {

// Composites image and camera overlays over each frame with Core Image on the GPU, into
// IOSurface-backed NV12 buffers from a small pool. The recording thread calls compose();
// set() may come from any thread. Each camera delivers on its own queue and only replaces
// its newest retained frame.
class Overlays {
public:
    Overlays();
    ~Overlays();
    Overlays(const Overlays&) = delete;
    Overlays& operator=(const Overlays&) = delete;

    Result start(std::uint32_t width, std::uint32_t height, const std::vector<Overlay>& overlays);
    // A retained frame to encode: base with the visible overlays on top, or base itself
    // (retained) when none is visible. Null when drawing failed.
    CVPixelBufferRef compose(CVPixelBufferRef base);
    Result set(std::size_t index, bool visible, const std::optional<Area>& area);
    std::uint64_t camera_frames() const;
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

std::vector<Camera> cameras();

} // namespace rec::mac
