#include "app/session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <utility>

namespace rec::app {
namespace {
std::uint32_t number(const std::string& text, std::uint32_t fallback) {
    try {
        return static_cast<std::uint32_t>(std::stoul(text));
    } catch (...) {
        return fallback;
    }
}

bool resolution(const std::string& name, Options& o) {
    if (name == "native") { o.width = o.height = 0; return true; }
    if (name == "720p") { o.width = 1280; o.height = 720; return true; }
    if (name == "1080p") { o.width = 1920; o.height = 1080; return true; }
    if (name == "1440p") { o.width = 2560; o.height = 1440; return true; }
    if (name == "4k") { o.width = 3840; o.height = 2160; return true; }
    return false;
}

std::filesystem::path utf8_path(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
} // namespace

Settings load_settings(const std::string& path) {
    Settings s;
    std::ifstream in(utf8_path(path), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals), value = line.substr(equals + 1);
        const bool yes = value == "1" || value == "true";
        if (key == "folder") s.folder = value;
        else if (key == "format" && (value == "mov" || value == "mp4")) s.format = value;
        else if (key == "display") s.display = std::max(1u, number(value, 1));
        else if (key == "resolution") { Options o; if (resolution(value, o)) s.resolution = value; }
        else if (key == "fps") {
            const auto fps = number(value, 60);
            if (fps == 24 || fps == 25 || fps == 30 || fps == 50 || fps == 60 || fps == 120) s.fps = fps;
        } else if (key == "codec" && (value == "auto" || value == "h264" || value == "hevc")) s.codec = value;
        else if (key == "cursor") s.cursor = yes;
        else if (key == "system_audio") s.system_audio = yes;
        else if (key == "microphone") s.microphone = yes;
        else if (key == "microphone_device" && !value.empty()) s.microphone_device = value;
        else if (key == "stream") s.stream = yes;
        else if (key == "destination" && (value == "twitch" || value == "youtube" || value == "custom")) s.destination = value;
        else if (key == "server") s.server = value;
        else if (key == "stream_kbps") s.stream_kbps = std::clamp(number(value, 6000), 500u, 50000u);
        else if (key == "last_recording") s.last_recording = value;
    }
    return s;
}

bool save_settings(const std::string& path, const Settings& s) {
    std::error_code error;
    const auto file = utf8_path(path);
    std::filesystem::create_directories(file.parent_path(), error);
    auto temporary = file;
    temporary += ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "folder=" << s.folder << "\nformat=" << s.format << "\ndisplay=" << s.display
            << "\nresolution=" << s.resolution << "\nfps=" << s.fps << "\ncodec=" << s.codec
            << "\ncursor=" << s.cursor << "\nsystem_audio=" << s.system_audio
            << "\nmicrophone=" << s.microphone << "\nmicrophone_device=" << s.microphone_device
            << "\nstream=" << s.stream << "\ndestination=" << s.destination << "\nserver=" << s.server
            << "\nstream_kbps=" << s.stream_kbps << "\nlast_recording=" << s.last_recording << '\n';
        if (!out) return false;
    }
    // Replace in one step so a crash never leaves half a settings file.
    std::filesystem::rename(temporary, file, error);
    return !error;
}

std::string server_url(const Settings& s) {
    if (s.destination == "twitch") return "rtmp://live.twitch.tv/app";
    if (s.destination == "youtube") return "rtmp://a.rtmp.youtube.com/live2";
    std::string url = s.server;
    while (!url.empty() && (url.back() == '/' || url.back() == ' ')) url.pop_back();
    return url;
}

std::string redact(const std::string& url) {
    const auto last = url.rfind('/');
    return last == std::string::npos ? url : url.substr(0, last + 1) + "****";
}

std::string next_output(const std::string& folder, const std::string& format) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "rec-%Y-%m-%d-%H%M%S", &local);
    std::string base = folder;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base += '/';
    base += stamp;
    std::string path = base + "." + format;
    std::error_code error;
    for (int n = 2; std::filesystem::exists(utf8_path(path), error); ++n)
        path = base + "-" + std::to_string(n) + "." + format;
    return path;
}

Session::Session(std::function<void()> changed) : changed_(std::move(changed)) {}

Session::~Session() {
    stop();
    wait();
}

Result Session::start(const Settings& s, const std::string& stream_key, bool test_pattern) {
    {
        std::lock_guard lock(mutex_);
        if (state_ != State::idle) return {"a recording is already running"};
    }
    wait();
    Options o;
    o.duration_seconds = 0;
    o.display = s.display;
    if (!resolution(s.resolution, o)) return {"unknown resolution: " + s.resolution};
    o.fps = s.fps;
    o.codec = s.codec == "h264" ? Codec::h264 : s.codec == "hevc" ? Codec::hevc : Codec::automatic;
    o.cursor = s.cursor;
    if (test_pattern) {
        o.test_pattern = true;
        o.test_tone = s.system_audio;
        o.test_mic = s.microphone;
    } else {
        o.system_audio = s.system_audio;
        o.microphone = s.microphone;
        o.microphone_device = s.microphone_device;
    }
    if (s.folder.empty()) return {"choose a folder for recordings"};
    std::error_code error;
    std::filesystem::create_directories(utf8_path(s.folder), error);
    if (error) return {"cannot use the recordings folder: " + error.message()};
    o.output = next_output(s.folder, s.format);
    if (s.stream) {
        const auto server = server_url(s);
        if (server.empty()) return {"enter the stream server URL"};
        if (stream_key.empty()) return {"enter the stream key"};
        o.stream_url = server + "/" + stream_key;
        o.stream_kbps = s.stream_kbps;
    }
    {
        std::lock_guard lock(mutex_);
        options_ = o;
        state_ = State::starting;
        stop_requested_ = false;
        streaming_ = s.stream;
        error_.clear();
        stream_error_.clear();
        last_ = {};
    }
    worker_ = std::thread([this, recorder = std::make_unique<Recorder>(o)]() mutable { run(std::move(recorder)); });
    return {};
}

void Session::run(std::unique_ptr<Recorder> recorder) {
    const auto started = recorder->start();
    {
        std::lock_guard lock(mutex_);
        if (!started) {
            error_ = started.error;
            state_ = State::idle;
        } else {
            recorder_ = recorder.get();
            state_ = State::recording;
        }
    }
    if (changed_) changed_();
    if (!started) return;
    {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return stop_requested_; });
        state_ = State::finishing;
    }
    if (changed_) changed_();
    // Finalizing can take seconds; it happens here, never on the UI thread.
    const auto stopped = recorder->stop();
    {
        std::lock_guard lock(mutex_);
        last_ = recorder->stats();
        error_ = !stopped ? stopped.error : recorder->failure();
        stream_error_ = recorder->stream_failure();
        std::error_code ignored;
        if (std::filesystem::exists(utf8_path(options_.output), ignored)) saved_ = options_.output;
        recorder_ = nullptr;
        state_ = State::idle;
    }
    if (changed_) changed_();
}

void Session::stop() {
    {
        std::lock_guard lock(mutex_);
        stop_requested_ = true;
    }
    wake_.notify_all();
}

Result Session::pause() {
    std::lock_guard lock(mutex_);
    if (state_ != State::recording || stop_requested_) return {"not recording"};
    auto paused = recorder_->pause();
    if (paused) state_ = State::paused;
    return paused;
}

Result Session::resume() {
    std::lock_guard lock(mutex_);
    if (state_ != State::paused || stop_requested_) return {"not paused"};
    auto resumed = recorder_->resume();
    if (resumed) state_ = State::recording;
    return resumed;
}

Status Session::status() {
    bool ended = false;
    Status s;
    {
        std::lock_guard lock(mutex_);
        const bool active = recorder_ && (state_ == State::recording || state_ == State::paused);
        if (active) {
            last_ = recorder_->stats();
            if (stream_error_.empty()) stream_error_ = recorder_->stream_failure();
            ended = recorder_->finished() && !stop_requested_;
        }
        s.state = state_;
        s.error = error_;
        s.stream_error = stream_error_;
        s.saved = saved_;
        s.streaming = streaming_;
        const Stats& st = last_;
        s.frames = st.written;
        s.seconds = options_.fps ? static_cast<double>(st.written) / options_.fps : 0;
        s.dropped = st.dropped;
        s.audio_dropped = st.audio_dropped + st.mic_dropped;
        s.system_peak = active ? st.system_peak : 0;
        s.mic_peak = active ? st.mic_peak : 0;
        s.live = active && st.streaming;
        s.stream_dropped = st.stream_dropped;
    }
    if (ended) stop();
    return s;
}

void Session::wait() {
    if (worker_.joinable()) worker_.join();
}

} // namespace rec::app
