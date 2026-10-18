#pragma once

#include "core/rtmp.hpp"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace rec::core {

// The last seconds of the stream encode (H.264 without B-frames, AAC-LC), kept in memory so
// they can be saved as an MP4 while recording continues. It takes the same FLV tag bodies the
// RTMP publisher sends. The window starts at a keyframe, so it holds up to one keyframe
// interval more than asked; past max_bytes the oldest keyframe interval is dropped.
class ReplayBuffer {
public:
    ReplayBuffer(std::uint32_t seconds, std::uint32_t width, std::uint32_t height, std::uint32_t fps);

    void video(std::uint32_t ms, bool key, bool header, const Bytes& body);
    void audio(std::uint32_t ms, bool header, const Bytes& body);
    // The AAC encoder's leading frames. Audio timestamps already account for them; a saved
    // file declares them in its edit list, as AVFoundation trims that much from AAC tracks
    // whose edit list does not.
    void set_priming(std::uint32_t frames);

    // Writes the buffered window to path (which must not exist). Empty on success; seconds is
    // the saved duration.
    std::string save(const std::string& path, double& seconds) const;
    double seconds() const;  // Video time held now.

private:
    struct Packet {
        std::uint32_t ms = 0;
        bool key = false;
        Bytes data;  // Length-prefixed NAL units, or one raw AAC frame.
    };
    void trim();

    const std::uint32_t window_ms_, width_, height_, fps_;
    const std::size_t max_bytes_;
    mutable std::mutex mutex_;
    Bytes avc_config_, aac_config_;  // AVCDecoderConfigurationRecord, AudioSpecificConfig.
    std::deque<Packet> video_, audio_;
    std::size_t bytes_ = 0;
    std::uint32_t priming_ = 0;
};

} // namespace rec::core
