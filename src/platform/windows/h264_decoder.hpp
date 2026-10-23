#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rec::win {

// Decodes H.264 access units (Annex B) with the Media Foundation decoder, in low-latency mode,
// to BGRA, top-down (BT.601 studio range, which phone browsers send). One thread creates and
// uses it; COM and Media Foundation are started on that thread.
class H264Decoder {
public:
    using Picture = std::function<void(const std::uint8_t* bgra, std::uint32_t width, std::uint32_t height)>;

    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    std::string start();  // Empty on success.
    // False when the unit could not be decoded; the caller should ask for a keyframe.
    bool decode(const std::uint8_t* annexb, std::size_t size, const Picture& picture);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rec::win
