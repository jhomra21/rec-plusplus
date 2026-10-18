#include "app/session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <utility>

namespace rec::app {
namespace {
int volume(const std::string& text) {
    try {
        return std::clamp(std::stoi(text), kVolumeMin, kVolumeMax);
    } catch (...) {
        return 0;
    }
}

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

// Settings are one key per line, so a value never carries a line break.
std::string one_line(std::string text) {
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    return text;
}

template <std::size_t N>
bool known(const std::array<Choice, N>& choices, const std::string& value) {
    return std::any_of(choices.begin(), choices.end(), [&](const Choice& c) { return value == c.value; });
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
        else if (key == "source" && (value == "display" || value == "window" || value == "region")) s.source = value;
        else if (key == "window_app") s.window_app = value;
        else if (key == "window_title") s.window_title = value;
        else if (key == "region" && parse_region(value)) s.region = value;
        else if (key == "resolution") { Options o; if (resolution(value, o)) s.resolution = value; }
        else if (key == "fps") {
            const auto fps = number(value, 60);
            if (fps == 24 || fps == 25 || fps == 30 || fps == 50 || fps == 60 || fps == 120) s.fps = fps;
        } else if (key == "codec" && (value == "auto" || value == "h264" || value == "hevc")) s.codec = value;
        else if (key == "cursor") s.cursor = yes;
        else if (key == "system_audio") s.system_audio = yes;
        else if (key == "microphone") s.microphone = yes;
        else if (key == "microphone_device" && !value.empty()) s.microphone_device = value;
        else if (key == "system_volume") s.system_volume = volume(value);
        else if (key == "mic_volume") s.mic_volume = volume(value);
        else if (key == "system_muted") s.system_muted = yes;
        else if (key == "mic_muted") s.mic_muted = yes;
        else if (key == "stream") s.stream = yes;
        else if (key == "destination" && (value == "twitch" || value == "youtube" || value == "custom")) s.destination = value;
        else if (key == "server") s.server = value;
        else if (key == "stream_kbps") s.stream_kbps = std::clamp(number(value, 6000), 500u, 50000u);
        else if (key == "replay") s.replay = yes;
        else if (key == "replay_seconds") {
            const auto seconds = number(value, 30);
            if (std::find(kReplayLengths.begin(), kReplayLengths.end(), seconds) != kReplayLengths.end()) s.replay_seconds = seconds;
        } else if (key == "camera") s.camera = yes;
        else if (key == "camera_device" && !value.empty()) s.camera_device = value;
        else if (key == "camera_corner" && known(kCorners, value)) s.camera_corner = value;
        else if (key == "camera_size" && known(kCameraSizes, value)) s.camera_size = value;
        else if (key == "scene" && known(kScenes, value)) s.scene = value;
        else if (key == "image") s.image = yes;
        else if (key == "image_path") s.image_path = value;
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
            << "\nsource=" << s.source << "\nwindow_app=" << one_line(s.window_app)
            << "\nwindow_title=" << one_line(s.window_title) << "\nregion=" << s.region
            << "\nresolution=" << s.resolution << "\nfps=" << s.fps << "\ncodec=" << s.codec
            << "\ncursor=" << s.cursor << "\nsystem_audio=" << s.system_audio
            << "\nmicrophone=" << s.microphone << "\nmicrophone_device=" << s.microphone_device
            << "\nsystem_volume=" << s.system_volume << "\nmic_volume=" << s.mic_volume
            << "\nsystem_muted=" << s.system_muted << "\nmic_muted=" << s.mic_muted
            << "\nstream=" << s.stream << "\ndestination=" << s.destination << "\nserver=" << s.server
            << "\nstream_kbps=" << s.stream_kbps << "\nreplay=" << s.replay << "\nreplay_seconds=" << s.replay_seconds
            << "\ncamera=" << s.camera << "\ncamera_device=" << one_line(s.camera_device)
            << "\ncamera_corner=" << s.camera_corner << "\ncamera_size=" << s.camera_size << "\nscene=" << s.scene
            << "\nimage=" << s.image << "\nimage_path=" << one_line(s.image_path)
            << "\nlast_recording=" << s.last_recording << '\n';
        if (!out) return false;
    }
    // Replace in one step so a crash never leaves half a settings file.
    std::filesystem::rename(temporary, file, error);
    return !error;
}

std::optional<Region> parse_region(const std::string& text) {
    Region r;
    long long values[4]{};
    char tail = 0;
    if (std::sscanf(text.c_str(), "%lld,%lld,%lld,%lld%c", &values[0], &values[1], &values[2], &values[3], &tail) != 4)
        return std::nullopt;
    if (values[0] < 0 || values[1] < 0 || values[2] < 2 || values[3] < 2 || values[0] > INT32_MAX ||
        values[1] > INT32_MAX || values[2] > INT32_MAX || values[3] > INT32_MAX)
        return std::nullopt;
    r.x = static_cast<std::int32_t>(values[0]);
    r.y = static_cast<std::int32_t>(values[1]);
    r.width = static_cast<std::uint32_t>(values[2]);
    r.height = static_cast<std::uint32_t>(values[3]);
    return r;
}

std::string region_text(const Region& r) {
    return std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.width) + "," + std::to_string(r.height);
}

std::optional<Window> find_window(const Settings& s) {
    if (s.window_app.empty() && s.window_title.empty()) return std::nullopt;
    std::string error;
    const auto windows = list_windows(error);
    for (const auto& w : windows)
        if (w.app == s.window_app && w.title == s.window_title) return w;
    if (!s.window_app.empty())
        for (const auto& w : windows)
            if (w.app == s.window_app) return w;
    return std::nullopt;
}

std::string server_url(const Settings& s) {
    if (s.destination == "twitch") return "rtmps://live.twitch.tv:443/app";
    if (s.destination == "youtube") return "rtmps://a.rtmps.youtube.com:443/live2";
    std::string url = s.server;
    while (!url.empty() && (url.back() == '/' || url.back() == ' ')) url.pop_back();
    return url;
}

std::string redact(const std::string& url) {
    const auto last = url.rfind('/');
    return last == std::string::npos ? url : url.substr(0, last + 1) + "****";
}

std::string next_output(const std::string& folder, const std::string& format, const char* prefix) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "-%Y-%m-%d-%H%M%S", &local);
    std::string base = folder;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base += '/';
    base += prefix;
    base += stamp;
    std::string path = base + "." + format;
    std::error_code error;
    for (int n = 2; std::filesystem::exists(utf8_path(path), error); ++n)
        path = base + "-" + std::to_string(n) + "." + format;
    return path;
}

Area camera_area(const Settings& s, const std::string& scene) {
    if (scene == "camera") return {};
    const double size = s.camera_size == "small" ? 0.2 : s.camera_size == "large" ? 0.33 : 0.25;
    // The same margin in pixels on both sides of a 16:9 frame.
    const double mx = 0.02, my = 0.035;
    const bool left = s.camera_corner.ends_with("left"), top = s.camera_corner.starts_with("top");
    return {left ? mx : 1 - mx - size, top ? my : 1 - my - size, size, size};
}

bool camera_visible(const std::string& scene) { return scene != "screen"; }

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
    if (!test_pattern && s.source == "window") {
        const auto window = find_window(s);
        if (!window) return {"the window \"" + (s.window_title.empty() ? s.window_app : s.window_title) +
                             "\" is not open; choose another source"};
        o.window = window->id;
    } else if (!test_pattern && s.source == "region") {
        o.region = parse_region(s.region);
        if (!o.region) return {"select a region to record"};
    }
    if (!resolution(s.resolution, o)) return {"unknown resolution: " + s.resolution};
    o.fps = s.fps;
    o.codec = s.codec == "h264" ? Codec::h264 : s.codec == "hevc" ? Codec::hevc : Codec::automatic;
    o.cursor = s.cursor;
    o.system_gain_db = std::clamp(s.system_volume, kVolumeMin, kVolumeMax);
    o.mic_gain_db = std::clamp(s.mic_volume, kVolumeMin, kVolumeMax);
    o.system_muted = s.system_muted;
    o.mic_muted = s.mic_muted;
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
    }
    o.stream_kbps = s.stream_kbps;
    if (s.replay) o.replay_seconds = s.replay_seconds;
    std::optional<std::size_t> camera;
    if (s.camera) {
        camera = o.overlays.size();
        Overlay c;
        c.kind = Overlay::Kind::camera;
        c.source = s.camera_device.empty() ? "default" : s.camera_device;
        c.area = camera_area(s, s.scene);
        c.visible = camera_visible(s.scene);
        o.overlays.push_back(c);
    }
    if (s.image) {
        if (s.image_path.empty()) return {"choose an image to show"};
        Overlay i;
        i.kind = Overlay::Kind::image;
        i.source = s.image_path;
        o.overlays.push_back(i);
    }
    {
        std::lock_guard lock(mutex_);
        options_ = o;
        state_ = State::starting;
        stop_requested_ = false;
        streaming_ = s.stream;
        replay_ = s.replay;
        camera_ = camera;
        camera_settings_ = s;
        scene_ = s.scene;
        error_.clear();
        stream_error_.clear();
        saved_.clear();
        last_ = {};
        rate_bytes_ = 0;
        rate_time_ = std::chrono::steady_clock::now();
        stream_kbps_ = 0;
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

Result Session::save_replay(const std::string& folder, std::string& path, double& seconds) {
    path.clear();
    seconds = 0;
    std::lock_guard lock(mutex_);
    if ((state_ != State::recording && state_ != State::paused) || stop_requested_) return {"not recording"};
    if (!replay_) return {"turn on the replay buffer before recording"};
    const auto target = next_output(folder, "mp4", "replay");
    auto saved = recorder_->save_replay(target, &seconds);
    if (saved) path = target;
    return saved;
}

Result Session::set_scene(const std::string& scene) {
    if (!known(kScenes, scene)) return {"unknown scene: " + scene};
    std::lock_guard lock(mutex_);
    if ((state_ != State::recording && state_ != State::paused) || stop_requested_) return {"not recording"};
    if (!camera_) return {"turn on the camera before recording"};
    auto set = recorder_->set_overlay(*camera_, camera_visible(scene), camera_area(camera_settings_, scene));
    if (set) scene_ = scene;
    return set;
}

Result Session::set_volume(AudioInput input, int volume, bool muted) {
    std::lock_guard lock(mutex_);
    if ((state_ != State::recording && state_ != State::paused) || stop_requested_) return {"not recording"};
    return recorder_->set_volume(input, std::clamp(volume, kVolumeMin, kVolumeMax), muted);
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
            const auto now = std::chrono::steady_clock::now();
            if (const std::chrono::duration<double> span = now - rate_time_; span.count() >= 1) {
                stream_kbps_ = static_cast<double>(last_.stream_bytes - rate_bytes_) * 8 / 1000 / span.count();
                rate_bytes_ = last_.stream_bytes;
                rate_time_ = now;
            }
            ended = recorder_->finished() && !stop_requested_;
        }
        s.state = state_;
        s.error = error_;
        s.stream_error = stream_error_;
        s.saved = saved_;
        s.streaming = streaming_;
        s.replay = replay_;
        const Stats& st = last_;
        s.frames = st.written;
        s.seconds = options_.fps ? static_cast<double>(st.written) / options_.fps : 0;
        s.dropped = st.dropped;
        s.audio_dropped = st.audio_dropped + st.mic_dropped;
        s.system_audio = options_.system_audio || options_.test_tone;
        s.microphone = options_.microphone || options_.test_mic;
        s.system_peak = active ? st.system_peak : 0;
        s.mic_peak = active ? st.mic_peak : 0;
        s.live = active && st.streaming;
        s.reconnecting = active && st.stream_reconnecting;
        s.reconnects = st.stream_reconnects;
        s.stream_dropped = st.stream_dropped;
        s.stream_kbps = active && st.streaming ? stream_kbps_ : 0;
        s.stream_queued_ms = active ? st.stream_queued_ms : 0;
        s.replay_seconds = active ? st.replay_seconds : 0;
        s.camera = camera_.has_value();
        s.scene = scene_;
        s.camera_frames = st.camera_frames;
    }
    if (ended) stop();
    return s;
}

void Session::wait() {
    if (worker_.joinable()) worker_.join();
}

} // namespace rec::app
