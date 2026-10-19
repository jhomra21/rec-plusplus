#pragma once

#include "core/rtmp.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
    // The AAC encoder's leading frames. Audio timestamps already account for them, but a
    // saved clip starts its audio at least this much before the video and skips it in a
    // single edit, the layout AVAssetWriter uses: AVFoundation trims this much from AAC
    // tracks that do not, and it misreads an AAC track that starts with an empty edit.
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
    std::uint32_t priming_ms_ = 0;
};

// The stream encode (the FLV tag bodies ReplayBuffer takes) written to an MP4 as it arrives,
// for the second canvas's movie. It writes a fragment for each keyframe interval of at least a
// second and flushes it, so a crash leaves a playable movie; finish() then writes a complete
// movie header at the end and frees the fragment headers, as consolidate_fragments() does.
// One thread calls everything but frames().
class MovieWriter {
public:
    MovieWriter(std::uint32_t width, std::uint32_t height, std::uint32_t fps);
    ~MovieWriter();
    MovieWriter(const MovieWriter&) = delete;
    MovieWriter& operator=(const MovieWriter&) = delete;

    std::string open(const std::string& path);  // The file must not exist. Empty on success.
    void video(std::uint32_t ms, bool key, bool header, const Bytes& body);
    void audio(std::uint32_t ms, bool header, const Bytes& body);
    void set_priming(std::uint32_t frames);  // As ReplayBuffer::set_priming().
    // Writes what is buffered and the complete movie header. Empty on success.
    std::string finish();
    std::uint64_t frames() const { return frames_.load(); }  // Video frames taken.
    const std::string& path() const { return path_; }

private:
    struct Packet {
        std::uint32_t ms = 0;
        bool key = false;
        Bytes data;
    };
    struct Written {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> chunks;
        std::vector<std::uint32_t> sizes, deltas, keys;
    };
    bool begin();
    void fragment(std::size_t video_count, std::uint32_t next_ms, bool last);
    void put(const Bytes& b);

    const std::uint32_t width_, height_, fps_;
    std::string path_, error_;
    std::unique_ptr<std::fstream> file_;
    std::uint64_t at_ = 0;  // Bytes written.
    Bytes avc_config_, aac_config_;
    std::deque<Packet> video_, audio_;
    bool begun_ = false, with_audio_ = false;
    std::uint32_t first_ms_ = 0, priming_ms_ = 0, delay_ms_ = 0, skip_ = 0;
    std::uint64_t video_time_ = 0, audio_time_ = 0;  // Decode time of the next fragment, per track.
    std::uint32_t sequence_ = 0;
    std::uint64_t moov_at_ = 0;
    std::vector<std::uint64_t> moofs_;
    Written video_out_, audio_out_;
    std::atomic<std::uint64_t> frames_{0};
};
} // namespace rec::core
