#pragma once

#include "rec/rec.hpp"

#include <CoreVideo/CoreVideo.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rec::mac {

struct LiveSettings {
    std::vector<std::string> urls;  // None: encode only for the replay buffer or the movie.
    std::uint32_t replay_seconds = 0;
    std::string movie;              // Also write the encode to this MP4 (core::MovieWriter).
    std::uint32_t width = 0, height = 0, fps = 60;
    std::uint32_t video_kbps = 6000, audio_kbps = 160;
};

struct LiveCounters {
    bool live = false;
    bool reconnecting = false;
    std::uint32_t reconnects = 0;
    bool hardware = false;
    std::uint64_t frames = 0, dropped = 0, bytes = 0;
    std::uint64_t encoder_dropped = 0;  // The part of dropped the encoder fell behind on.
    std::uint32_t queued_ms = 0;
    double replay_seconds = 0;
    std::uint64_t movie_frames = 0;
    std::array<StreamStats, kMaxStreams> servers{};
    std::uint32_t server_count = 0;
};

// A second, independent encode of the recording timeline for RTMP and the replay buffer:
// its own VideoToolbox H.264 session and AAC converter at the stream bitrate, so the file
// keeps its quality. video() and audio() only queue (four frames, two seconds of audio, oldest dropped on
// overflow); one thread feeds the encoders and packages their output.
class LiveStream {
public:
    LiveStream();
    ~LiveStream();
    LiveStream(const LiveStream&) = delete;
    LiveStream& operator=(const LiveStream&) = delete;

    Result start(const LiveSettings& settings);  // Connects before returning.
    void video(CVPixelBufferRef frame, std::uint64_t slot);  // Retained; slot is 1/fps units.
    // 48 kHz stereo float at a recording-timeline sample index; null data is silence.
    void audio(std::int64_t start, const float* stereo, std::size_t frames);
    // Drains the encoders, sends what is queued (up to 3 s), disconnects and finishes the
    // movie. Returns why the movie could not be finished, if it could not.
    std::string stop();
    LiveCounters counters() const;
    std::string failure() const;
    Result save_replay(const std::string& path, double& seconds) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rec::mac
