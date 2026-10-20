#pragma once

#include <CoreVideo/CoreVideo.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace rec::mac {

// Decodes H.264 access units (Annex B) with VideoToolbox to BGRA pixel buffers backed by
// IOSurface. A new SPS or PPS in the stream makes a new decompression session. One thread
// uses it; each picture is passed to the callback on that thread before decode() returns.
class H264Decoder {
public:
    using Picture = std::function<void(CVPixelBufferRef)>;

    explicit H264Decoder(Picture picture);
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // False when the unit could not be decoded; the caller should ask for a keyframe.
    bool decode(const std::uint8_t* annexb, std::size_t size);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rec::mac
