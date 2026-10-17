#pragma once

#include "rec/rec.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// The app layer shared by the macOS and Windows interfaces: persisted settings, stream
// destinations, and one recording session at a time. Platform shells own windows, menus,
// hotkeys, folders and secret storage; they never talk to rec::Recorder directly.
namespace rec::app {

struct Settings {
    std::string folder;               // Empty: the platform's movies folder.
#ifdef _WIN32
    std::string format = "mp4";       // mov or mp4.
#else
    std::string format = "mov";
#endif
    std::uint32_t display = 1;
    std::string resolution = "1080p"; // 720p, 1080p, 1440p, 4k, native.
    std::uint32_t fps = 60;
    std::string codec = "auto";       // auto, h264, hevc.
    bool cursor = true;
    bool system_audio = true;
    bool microphone = false;
    std::string microphone_device = "default";
    bool stream = false;              // Go live while recording.
    std::string destination = "twitch"; // twitch, youtube, custom.
    std::string server;               // Custom destination: rtmp://host[:port]/app.
    std::uint32_t stream_kbps = 6000;
    std::string last_recording;
};

// key=value lines; unknown keys are ignored and missing ones keep their defaults.
Settings load_settings(const std::string& path);
bool save_settings(const std::string& path, const Settings& settings);

// rtmp://host/app for the destination; the stream key is appended as the last segment.
std::string server_url(const Settings& settings);

enum class State { idle, starting, recording, paused, finishing };

struct Status {
    State state = State::idle;
    double seconds = 0;              // Recorded time, excluding pauses.
    std::uint64_t frames = 0, dropped = 0, audio_dropped = 0;
    float system_peak = 0, mic_peak = 0;  // Since the previous status() call.
    bool streaming = false;          // A stream was requested for this recording.
    bool live = false;               // Connected and sending.
    std::uint64_t stream_dropped = 0;
    std::string stream_error;        // Why the stream ended; the recording continues.
    std::string error;               // Why the last recording failed or ended early.
    std::string saved;               // Path of the last completed recording.
};

// One recording at a time, owned by a worker thread: start() checks the settings and
// returns at once; the worker starts capture (which can wait on permissions and the first
// frame), waits for stop(), then finalizes the movie. The change callback runs on the
// worker after each step; it must only post to the UI thread, never call back in. A
// recording that ends on its own (capture lost) is stopped when status() sees it.
// start(), wait() and the destructor belong to one thread, the UI thread.
class Session {
public:
    explicit Session(std::function<void()> changed);
    ~Session();  // Finalizes a running recording.
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // stream_key is used only when settings.stream is set. test_pattern records the
    // synthetic flash/beep sources instead of the screen, for end-to-end checks.
    Result start(const Settings& settings, const std::string& stream_key, bool test_pattern = false);
    void stop();
    Result pause();
    Result resume();
    Status status();
    void wait();  // Blocks until a stop in progress has saved the movie.

private:
    void run(std::unique_ptr<Recorder> recorder);

    std::function<void()> changed_;
    std::mutex mutex_;
    std::condition_variable wake_;
    Recorder* recorder_ = nullptr;  // Owned by the worker; set while capture runs.
    Options options_;
    State state_ = State::idle;
    bool stop_requested_ = false;
    std::thread worker_;
    std::string error_, stream_error_, saved_;
    bool streaming_ = false;
    Stats last_{};
};

// rec-YYYY-MM-DD-HHMMSS.<format> in folder, with -2, -3 ... if that file exists.
std::string next_output(const std::string& folder, const std::string& format);
std::string redact(const std::string& url);  // The URL without its stream key.

} // namespace rec::app
