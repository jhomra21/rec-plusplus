#include "cli/engine_client.hpp"

#include "cli/engine.hpp"
#include "cli/local_socket.hpp"
#include "cli/wire.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace rec::cli {
namespace {
constexpr int kStatsTimeoutMs = 500;
const char* const kGone = "the background engine stopped; the recording may be incomplete";

std::string area_text(const Area& a) {
    char text[128];
    std::snprintf(text, sizeof text, "%.17g,%.17g,%.17g,%.17g", a.x, a.y, a.width, a.height);
    return text;
}

std::string_view value_of(const std::string& line, std::string_view key) {
    if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '=')
        return std::string_view(line).substr(key.size() + 1);
    return {};
}
} // namespace

EngineRecorder::EngineRecorder(Options options, std::string socket, std::vector<std::string> launch_args)
    : options_(std::move(options)), socket_(std::move(socket)), launch_args_(std::move(launch_args)) {}

EngineRecorder::EngineRecorder(std::string socket, std::uint64_t id, const std::string& preview, const std::string& preview2)
    : socket_(std::move(socket)), id_(id) {
    open_preview(preview, preview2);
}

std::unique_ptr<EngineRecorder> EngineRecorder::attach(const std::string& socket, Options& options, std::string& error) {
    std::string ignored;
    if (!LocalSocket::connect(socket, ignored).valid()) return nullptr;  // No engine, so no recording.
    EngineRecorder probe(socket, 0, {}, {});
    const auto reply = probe.exchange("options", 2000);
    if (!reply.ok) {
        if (reply.text != "not recording") error = reply.text;
        return nullptr;
    }
    std::uint64_t id = 0;
    std::string preview, preview2, words;
    for (const auto& line : reply.lines) {
        if (const auto v = value_of(line, "id"); !v.empty()) id = std::strtoull(std::string(v).c_str(), nullptr, 10);
        else if (const auto p = value_of(line, "preview"); !p.empty()) preview = p;
        else if (const auto p2 = value_of(line, "preview2"); !p2.empty()) preview2 = p2;
        else if (const auto o = value_of(line, "options"); !o.empty()) words = o;
    }
    if (!id || !decode_options(split(words), options, error)) {
        if (error.empty()) error = "the engine's reply was not understood";
        return nullptr;
    }
    auto recorder = std::unique_ptr<EngineRecorder>(new EngineRecorder(socket, id, preview, preview2));
    recorder->options_ = options;
    return recorder;
}

EngineRecorder::Reply EngineRecorder::exchange(const std::string& line, int timeout_ms) const {
    Reply reply;
    std::string error;
    auto connection = LocalSocket::connect(socket_, error);
    if (!connection.valid() || !connection.write(line + "\n")) {
        reply.text = kGone;
        return reply;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::string text;
    bool timed_out = false;
    for (;;) {
        if (!connection.read_line(text, 200, timed_out)) {
            if (timed_out && (timeout_ms == 0 || std::chrono::steady_clock::now() < deadline)) continue;
            reply.text = timed_out ? "the background engine is busy" : kGone;
            return reply;
        }
        if (text == "ok" || text.rfind("ok ", 0) == 0) {
            reply.ok = true;
            reply.text = text.size() > 3 ? text.substr(3) : std::string();
            return reply;
        }
        if (text.rfind("error ", 0) == 0) {
            reply.text = text.substr(6);
            return reply;
        }
        reply.lines.push_back(std::move(text));
    }
}

Result EngineRecorder::simple(const std::string& line) const {
    const auto reply = exchange(line);
    return reply.ok ? Result{} : Result{reply.text};
}

void EngineRecorder::open_preview(const std::string& name, const std::string& second) {
    std::string ignored;
    if (!name.empty()) preview_ = SharedFrame::open(name, ignored);
    if (!second.empty()) preview2_ = SharedFrame::open(second, ignored);
}

Result EngineRecorder::start() {
    std::string error;
    if (!LocalSocket::connect(socket_, error).valid()) {
        if (!launch_engine(socket_, launch_args_, error)) return {error};
        bool up = false;
        for (int i = 0; i < 100 && !up; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            up = LocalSocket::connect(socket_, error).valid();
        }
        if (!up) return {"the background engine did not start; see engine.log beside " + socket_};
    }
    // Starting can wait on a permission prompt; there is no deadline.
    const auto reply = exchange("begin " + join(encode_options(options_)));
    if (!reply.ok) return {reply.text == kGone ? std::string("the background engine stopped while starting") : reply.text};
    std::string preview, preview2;
    for (const auto& line : reply.lines) {
        if (const auto v = value_of(line, "id"); !v.empty()) id_ = std::strtoull(std::string(v).c_str(), nullptr, 10);
        else if (const auto p = value_of(line, "preview"); !p.empty()) preview = p;
        else if (const auto p2 = value_of(line, "preview2"); !p2.empty()) preview2 = p2;
    }
    std::lock_guard lock(mutex_);
    open_preview(preview, preview2);
    return {};
}

void EngineRecorder::read_ending(const Reply& reply) {
    Stats s;
    std::string failure, stream_failure;
    for (const auto& line : reply.lines) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string_view key = std::string_view(line).substr(0, eq), value = std::string_view(line).substr(eq + 1);
        if (key == "failure") failure = value;
        else if (key == "stream_failure") stream_failure = value;
        else decode_stat(key, value, s);
    }
    std::lock_guard lock(mutex_);
    stats_ = s;
    failure_ = std::move(failure);
    stream_failure_ = std::move(stream_failure);
}

Result EngineRecorder::stop() {
    const auto reply = exchange("finish " + std::to_string(id_));
    if (reply.ok) read_ending(reply);
    std::lock_guard lock(mutex_);
    stopped_ = finished_ = true;
    if (!reply.ok && failure_.empty()) failure_ = reply.text;
    preview_ = {};
    preview2_ = {};
    return {};
}

Result EngineRecorder::pause() { return simple("pause"); }
Result EngineRecorder::resume() { return simple("resume"); }

bool EngineRecorder::finished() const {
    std::lock_guard lock(mutex_);
    return finished_;
}

std::string EngineRecorder::failure() const {
    std::lock_guard lock(mutex_);
    return failure_;
}

std::string EngineRecorder::stream_failure() const {
    std::lock_guard lock(mutex_);
    return stream_failure_;
}

Stats EngineRecorder::stats() {
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || finished_) return stats_;
    }
    const auto reply = exchange("stats", kStatsTimeoutMs);
    std::lock_guard lock(mutex_);
    if (!reply.ok) {
        // A busy engine (another client stopping or saving) keeps the last numbers.
        if (reply.text == kGone) {
            finished_ = true;
            if (failure_.empty()) failure_ = kGone;
        }
        return stats_;
    }
    Stats s;
    bool mine = false, ended = false;
    std::string failure, stream_failure;
    for (const auto& line : reply.lines) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string_view key = std::string_view(line).substr(0, eq), value = std::string_view(line).substr(eq + 1);
        if (key == "state") ended = value == "idle";
        else if (key == "id") mine = std::strtoull(std::string(value).c_str(), nullptr, 10) == id_;
        else if (key == "finished") ended = ended || value == "1";
        else if (key == "failure") failure = value;
        else if (key == "stream_failure") stream_failure = value;
        else if (key != "last") decode_stat(key, value, s);
    }
    // Idle, or another recording: this one ended without us; stop() asks how.
    if (ended || !mine) {
        finished_ = true;
        return stats_;
    }
    stats_ = s;
    failure_ = std::move(failure);
    stream_failure_ = std::move(stream_failure);
    return stats_;
}

Result EngineRecorder::save_replay(const std::string& path, double* seconds) {
    const auto reply = exchange("save-to " + path);
    if (!reply.ok) return {reply.text};
    for (const auto& line : reply.lines)
        if (const auto v = value_of(line, "seconds"); !v.empty() && seconds) *seconds = std::strtod(std::string(v).c_str(), nullptr);
    return {};
}

Result EngineRecorder::set_overlay(std::size_t index, bool visible, std::optional<Area> area) {
    const auto n = std::to_string(index + 1);
    if (area) return simple("place " + n + (visible ? " 1 " : " 0 ") + area_text(*area));
    return simple((visible ? "show " : "hide ") + n);
}

Result EngineRecorder::set_screen(bool visible, std::optional<Area> area) {
    if (area) return simple(std::string("place screen ") + (visible ? "1 " : "0 ") + area_text(*area));
    return simple(visible ? "show screen" : "hide screen");
}

Result EngineRecorder::set_volume(AudioInput input, double gain_db, bool muted) {
    char text[64];
    std::snprintf(text, sizeof text, "%.3f", gain_db);
    return simple(std::string("level ") + (input == AudioInput::system ? "system " : "mic ") + text + (muted ? " 1" : " 0"));
}

Result EngineRecorder::set_layout(std::size_t canvas, std::size_t layer, const Layout& layout) {
    const std::string source = layer == kScreenLayer ? std::string("screen") : std::to_string(layer + 1);
    return simple(std::string("place ") + (canvas == kSecondCanvas ? "canvas2 " : "") + source + (layout.visible ? " 1 " : " 0 ") +
                  area_text(layout.area) + ' ' + area_text(layout.crop));
}

bool EngineRecorder::preview(PreviewFrame& frame) const { return preview(frame, kMainCanvas); }

bool EngineRecorder::preview(PreviewFrame& frame, std::size_t canvas) const {
    std::lock_guard lock(mutex_);
    return canvas == kSecondCanvas ? preview2_.read(frame) : preview_.read(frame);
}

void EngineRecorder::show_preview(bool on) { exchange(on ? "preview on" : "preview off", kStatsTimeoutMs); }

} // namespace rec::cli
