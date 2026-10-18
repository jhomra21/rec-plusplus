#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rec {

enum class Codec { automatic, h264, hevc };

// Track layout when both system audio and a microphone are recorded.
// With a single audio source the movie has a single audio track.
enum class AudioTracks {
    both,  // Mix (default track), system stem, microphone stem.
    mix,   // Mix only.
    stems  // System and microphone stems, no mix.
};

// A rectangle of a display in its pixels, from the display's top-left corner.
struct Region {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct Options {
    std::string output = "rec.mov";
    std::uint32_t display = 1; // 1-based index from list_displays()
    std::uint64_t window = 0; // Capture this window (id from list_windows()) instead of a display.
    std::optional<Region> region; // Capture only this part of the display.
    std::uint32_t width = 1920; // bounding box, 0/0 selects native resolution
    std::uint32_t height = 1080;
    std::uint32_t fps = 60;
    std::uint32_t bitrate_mbps = 0; // 0 = selected from resolution
    Codec codec = Codec::automatic;
    bool cursor = true;
    bool test_pattern = false; // Synthetic flashing frame, for hardware-encoder E2E without screen permission.
    bool test_tone = false; // Synthetic beep as system audio, once a second with the flash (requires test_pattern).
    bool test_mic = false; // Synthetic quiet microphone source (requires test_pattern).
    bool system_audio = false; // Opt-in ScreenCaptureKit system audio, 48 kHz stereo AAC.
    bool microphone = false; // Opt-in ScreenCaptureKit microphone (macOS 15+).
    std::string microphone_device = "default"; // "default", unique ID, or name substring.
    AudioTracks audio_tracks = AudioTracks::both;
    double system_gain_db = 0; // Mix track only.
    double mic_gain_db = 0; // Mix track only.
    std::optional<double> system_audio_offset_ms; // Unset: default output device latency (0 for test tone).
    double mic_offset_ms = 0;
    double duration_seconds = 10.0; // 0 = until stop()
    // Live stream of the same timeline, encoded separately: rtmp[s]://host[:port]/app/stream-key.
    std::string stream_url;
    std::uint32_t stream_kbps = 6000; // Stream video bitrate; audio is 160 kb/s AAC.
    // Keep the last N seconds (5 to 300) of the stream encode in memory for save_replay();
    // 0 = off. Without stream_url the stream encoders run for the replay alone.
    std::uint32_t replay_seconds = 0;
};

struct Display {
    std::uint32_t id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string name;
};

struct Window {
    std::uint64_t id = 0;
    std::uint32_t width = 0; // Pixels.
    std::uint32_t height = 0;
    std::string title;
    std::string app;
};

struct AudioDevice {
    std::string id;
    std::string name;
};

struct Stats {
    std::uint64_t captured = 0;
    std::uint64_t written = 0;
    std::uint64_t dropped = 0;
    std::uint64_t audio_written = 0; // System audio buffers.
    std::uint64_t audio_dropped = 0;
    std::uint64_t mic_written = 0;
    std::uint64_t mic_dropped = 0;
    std::uint64_t mix_written = 0;
    // Linear peak (0 to 1) of each source since the previous stats() call, for level meters.
    float system_peak = 0;
    float mic_peak = 0;
    double system_audio_offset_ms = 0;
    bool paused = false;
    double paused_seconds = 0; // Total time paused, excluded from the movie.
    bool streaming = false; // Connected and sending.
    bool stream_reconnecting = false; // Lost the server and trying to reconnect.
    std::uint32_t stream_reconnects = 0; // Times the stream reconnected after a loss.
    bool stream_hardware = false; // The stream uses a hardware video encoder.
    std::uint64_t stream_frames = 0; // Video frames sent to the stream.
    std::uint64_t stream_dropped = 0; // Video frames the stream skipped to keep up.
    std::uint64_t stream_bytes = 0;
    double replay_seconds = 0; // Media the replay buffer holds now.
};

struct Probe {
    std::uint64_t frames = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double duration_seconds = 0;
    std::string codec;
    std::uint32_t audio_tracks = 0;
    std::string audio_codec; // First (default) audio track.
    std::uint64_t audio_buffers = 0;
    double audio_duration_seconds = 0;
    // Flash/beep pairs: a dark-to-bright video frame matched with an audio onset within 500 ms.
    std::uint32_t sync_events = 0;
    double av_offset_ms = 0; // Mean audio minus video; positive means audio is late.
    double av_offset_max_ms = 0; // Largest absolute offset.
};

struct Result {
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};

// A failure sets error and returns an empty list.
std::vector<Display> list_displays(std::string& error);
// Normal app windows that are on screen and can be captured, excluding rec's own.
std::vector<Window> list_windows(std::string& error);
std::vector<AudioDevice> list_microphones(std::string& error);
// Time between macOS mixing system audio and the default output device playing it. 0 if unknown.
double default_output_latency_ms() noexcept;
Result probe_file(const std::string& path, Probe& probe);

class Recorder {
public:
    explicit Recorder(Options options);
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    Result start();
    Result stop(); // Finalizes the output before returning.
    // The movie skips paused time: video and audio continue gaplessly after resume().
    Result pause();
    Result resume();
    bool running() const noexcept;
    bool finished() const noexcept; // Duration reached or capture failed.
    Stats stats() const noexcept;
    std::string failure() const;
    // Why the stream ended early. The recording continues without it.
    std::string stream_failure() const;
    // Writes the replay buffer to a new MP4 at path while recording continues: the last
    // replay_seconds, from a keyframe, so up to 2 s more. seconds receives the saved length.
    Result save_replay(const std::string& path, double* seconds = nullptr);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rec
