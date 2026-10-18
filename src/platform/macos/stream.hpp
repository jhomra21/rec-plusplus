#pragma once

#include "rec/rec.hpp"

#include <CoreVideo/CoreVideo.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rec::mac {

struct LiveSettings {
    std::string url;
    std::uint32_t width = 0, height = 0, fps = 60;
    std::uint32_t video_kbps = 6000, audio_kbps = 160;
};

struct LiveCounters {
    bool live = false;
    bool reconnecting = false;
    std::uint32_t reconnects = 0;
    bool hardware = false;
    std::uint64_t frames = 0, dropped = 0, bytes = 0;
};

// A second, independent encode of the recording timeline for RTMP: its own VideoToolbox
// H.264 session and AAC converter at the stream bitrate, so the file keeps its quality.
// video() and audio() only queue (four frames, two seconds of audio, oldest dropped on
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
    void stop();  // Drains the encoders, sends what is queued (up to 3 s), disconnects.
    LiveCounters counters() const;
    std::string failure() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rec::mac
