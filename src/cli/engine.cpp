#include "cli/engine.hpp"

#include "cli/local_socket.hpp"
#include "cli/options.hpp"
#include "cli/shared_frame.hpp"
#include "cli/wire.hpp"
#include "rec/rec.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace rec::cli {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

fs::path u8path(const std::string& text) { return fs::path(reinterpret_cast<const char8_t*>(text.c_str())); }
std::string text(const fs::path& path) {
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

} // namespace

std::vector<std::string> split(std::string_view line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        if (i == line.size()) break;
        std::string arg;
        if (line[i] == '"') {
            for (++i; i < line.size() && line[i] != '"'; ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) ++i;
                arg += line[i];
            }
            ++i;
        } else {
            while (i < line.size() && line[i] != ' ') arg += line[i++];
        }
        out.push_back(std::move(arg));
    }
    return out;
}

namespace {
// One recording at a time. Requests come from any connection's thread.
class Engine {
public:
    ~Engine() { close(); }

    std::string handle(const std::string& line, bool& shutdown) {
        std::lock_guard lock(mutex_);
        const auto word = line.substr(0, line.find(' '));
        const auto rest = line.size() > word.size() ? line.substr(word.size() + 1) : std::string();
        if (word == "start") return start(rest);
        if (word == "begin") return begin(rest);
        if (word == "status") return status();
        if (word == "stats") return stats();
        if (word == "options") return options();
        if (word == "finish") return finish_request(rest);
        if (word == "save-to") return save_to(rest);
        if (word == "preview") {
            if (!recorder_) return "error not recording\n";
            if (rest != "on" && rest != "off") return "error Use: preview on|off\n";
            recorder_->show_preview(rest == "on");
            return "ok\n";
        }
        if (line == "shutdown") {
            shutdown = true;
            return (recorder_ ? end() : std::string()) + "ok the engine stopped\n";
        }
        std::string error;
        const auto parsed = parse_request(line, error);
        if (!parsed) return "error " + error + "\n";
        if (!recorder_) return "error not recording\n";
        if (parsed->action == Action::stop) return end();
        std::string message;
        if (auto applied = controls_->apply(*parsed, recorded(), message); !applied) return "error " + applied.error + "\n";
        if (message.empty()) return "ok\n";
        std::cout << message << std::endl;
        return "ok " + message + "\n";
    }

    // Saves a recording that ended by itself: its duration, a closed window, a failure.
    // Returns whether a recording runs.
    bool tick() {
        std::lock_guard lock(mutex_);
        if (recorder_ && recorder_->finished()) end();
        return recorder_ != nullptr;
    }

    void close() {
        std::lock_guard lock(mutex_);
        if (recorder_) end();
    }

private:
    // What the last recording left: its statistics and how it ended, for `finish`.
    struct Ended {
        std::uint64_t id = 0;
        Stats stats;
        std::string failure, stream_failure, saved, saved2;  // saved2: the second canvas's movie.
    };

    std::string start(const std::string& rest) {
        if (recorder_) return "error already recording to " + options_.output + "\n";
        auto args = split(rest);
        fs::path base = fs::current_path();
        if (args.size() >= 2 && args[0] == "--cwd") {
            base = u8path(args[1]);
            args.erase(args.begin(), args.begin() + 2);
        }
        Options options;
        std::string error;
        if (!parse_options(args, options, error)) return "error " + error + "\n";
        // Paths are relative to the client's directory, not the engine's.
        const auto resolve = [&](std::string& path) {
            if (!path.empty() && u8path(path).is_relative()) path = text((base / u8path(path)).lexically_normal());
        };
        resolve(options.output);
        resolve(options.canvas2.output);
        for (auto& overlay : options.overlays)
            if (overlay.kind == Overlay::Kind::image) resolve(overlay.source);
        return launch(std::move(options));
    }

    // A recording described by encode_options(), with absolute paths.
    std::string begin(const std::string& rest) {
        if (recorder_) return "error already recording to " + options_.output + "\n";
        Options options;
        std::string error;
        if (!decode_options(split(rest), options, error)) return "error " + error + "\n";
        if (options.preview_only) return "error the engine records; preview_only is for in-process previews\n";
        if (options.output.empty() || u8path(options.output).is_relative()) return "error the output path must be absolute\n";
        if (!options.canvas2.output.empty() && u8path(options.canvas2.output).is_relative())
            return "error the second canvas's output path must be absolute\n";
        std::cout << "Starting a recording" << std::endl;
        return launch(std::move(options));
    }

    std::string launch(Options options) {
        options_ = std::move(options);
        auto recorder = std::make_unique<Recorder>(options_);
        if (auto started = recorder->start(); !started) return "error " + one_line(started.error) + "\n";
        recorder_ = std::move(recorder);
        controls_ = std::make_unique<Controls>(*recorder_, options_);
        began_ = Clock::now();
        ++id_;
        last_error_.clear();
        std::string reply = "id=" + std::to_string(id_) + "\n";
        if (options_.preview_width) {
            std::string error;
            const std::size_t width = options_.preview_width;
            // Portrait captures are up to twice as tall as wide in the preview.
            preview_ = SharedFrame::create(width * width * 2, error);
            if (options_.canvas2.width && preview_.valid()) preview2_ = SharedFrame::create(width * width * 2, error);
            if (preview_.valid()) {
                reply += "preview=" + preview_.name() + "\n";
                if (preview2_.valid()) reply += "preview2=" + preview2_.name() + "\n";
                pumping_ = true;
                pump_ = std::thread([this, recorder = recorder_.get()] {
                    PreviewFrame frame, second;
                    while (pumping_.load()) {
                        if (recorder->preview(frame)) preview_.write(frame);
                        if (preview2_.valid() && recorder->preview(second, kSecondCanvas)) preview2_.write(second);
                        std::this_thread::sleep_for(std::chrono::milliseconds(8));
                    }
                });
            } else {
                std::cout << error << std::endl;
            }
        }
        std::cout << "Recording to " << options_.output << std::endl;
        return reply + "ok Recording to " + options_.output + "\n";
    }

    double recorded() const {
        const std::chrono::duration<double> wall = Clock::now() - began_;
        return std::max(0.0, wall.count() - recorder_->stats().paused_seconds);
    }

    // Stops and saves the recording; the reply, also logged, lists what was written.
    std::string end() {
        const auto reply = finish();
        const auto last = reply.rfind('\n', reply.size() - 2);
        const auto final_line = last == std::string::npos ? 0 : last + 1;
        const auto space = reply.find(' ', final_line);
        std::cout << reply.substr(0, final_line) << reply.substr(space + 1) << std::flush;
        return reply;
    }

    std::string finish() {
        const std::chrono::duration<double> wall = Clock::now() - began_;
        pumping_ = false;
        if (pump_.joinable()) pump_.join();
        preview_ = {};
        preview2_ = {};
        const auto stopped = recorder_->stop();
        const auto stats = recorder_->stats();
        std::string reply = summary(options_, stats, wall.count());
        ended_ = {id_, stats, !stopped ? stopped.error : recorder_->failure(), recorder_->stream_failure(), {}};
        std::string failure = ended_.failure;
        if (failure.empty() && !ended_.stream_failure.empty())
            failure = "saved " + options_.output + ", but the stream stopped early: " + ended_.stream_failure;
        if (stats.window_closed) reply += "The window closed, so the recording stopped.\n";
        controls_.reset();
        recorder_.reset();
        std::error_code ignored;
        if (fs::exists(u8path(options_.output), ignored)) ended_.saved = options_.output;
        if (!options_.canvas2.output.empty() && fs::exists(u8path(options_.canvas2.output), ignored))
            ended_.saved2 = options_.canvas2.output;
        if (failure.empty()) {
            last_saved_ = options_.output;
            return reply + "ok Saved " + options_.output + "\n";
        }
        last_error_ = one_line(failure);
        return reply + "error " + last_error_ + "\n";
    }

    std::string ended_text() const {
        return encode_stats(ended_.stats) + "failure=" + one_line(ended_.failure) + "\nstream_failure=" +
               one_line(ended_.stream_failure) + "\nsaved=" + ended_.saved + "\nsaved2=" + ended_.saved2 + "\nok\n";
    }

    // finish ID: stops recording ID and reports how it ended, or reports it when it already ended.
    std::string finish_request(const std::string& rest) {
        const auto id = std::strtoull(rest.c_str(), nullptr, 10);
        if (recorder_ && id == id_) {
            end();
            return ended_text();
        }
        if (id && id == ended_.id) return ended_text();
        return "error no recording " + rest + "\n";
    }

    std::string save_to(const std::string& path) {
        if (!recorder_) return "error not recording\n";
        if (path.empty() || u8path(path).is_relative()) return "error Use: save-to ABSOLUTE-PATH\n";
        double seconds = 0;
        if (auto saved = recorder_->save_replay(path, &seconds); !saved) return "error " + one_line(saved.error) + "\n";
        std::ostringstream out;
        out << "seconds=" << seconds << "\n";
        std::cout << "Saved replay (" << std::fixed << std::setprecision(1) << seconds << " s) to " << path << std::endl;
        return out.str() + "ok\n";
    }

    std::string stats() {
        if (!recorder_) return "state=idle\nlast=" + std::to_string(ended_.id) + "\nok\n";
        const auto s = recorder_->stats();
        return std::string("state=") + (s.paused ? "paused" : "recording") + "\nid=" + std::to_string(id_) + "\n" +
               encode_stats(s) + "finished=" + (recorder_->finished() ? "1" : "0") + "\nfailure=" +
               one_line(recorder_->failure()) + "\nstream_failure=" + one_line(recorder_->stream_failure()) + "\nok\n";
    }

    // The running recording, its stream keys hidden, so a client can take it over.
    std::string options() {
        if (!recorder_) return "error not recording\n";
        Options shown = options_;
        for (auto& url : shown.stream_urls) url = redact(url);
        for (auto& url : shown.canvas2.stream_urls) url = redact(url);
        std::string reply = "id=" + std::to_string(id_) + "\n";
        if (preview_.valid()) reply += "preview=" + preview_.name() + "\n";
        if (preview2_.valid()) reply += "preview2=" + preview2_.name() + "\n";
        return reply + "options=" + join(encode_options(shown)) + "\nok\n";
    }

    std::string status() {
        std::ostringstream out;
        if (!recorder_) {
            out << "state=idle\n";
        } else {
            const auto stats = recorder_->stats();
            out << "state=" << (stats.paused ? "paused" : "recording") << "\noutput=" << options_.output << "\nrecorded="
                << std::fixed << std::setprecision(1) << recorded() << "\nwritten=" << stats.written << "\ndropped="
                << stats.dropped << '\n';
            if (stats.stream_count) {
                std::uint32_t live = 0;
                for (std::uint32_t i = 0; i < stats.stream_count; ++i) live += stats.streams[i].live ? 1 : 0;
                out << "streams_live=" << live << '/' << stats.stream_count << '\n';
            }
            if (options_.canvas2.width) out << "canvas2_frames=" << stats.canvas2_frames << '\n';
            if (options_.replay_seconds) out << "replay_seconds=" << static_cast<int>(stats.replay_seconds) << '\n';
            if (const auto muted = controls_->muted(); !muted.empty()) out << "muted=" << muted << '\n';
        }
        if (!last_saved_.empty()) out << "last_saved=" << last_saved_ << '\n';
        if (!last_error_.empty()) out << "last_error=" << last_error_ << '\n';
        out << "ok\n";
        return out.str();
    }

    std::mutex mutex_;
    Options options_;
    std::unique_ptr<Recorder> recorder_;
    std::unique_ptr<Controls> controls_;
    Clock::time_point began_;
    std::uint64_t id_ = 0;
    Ended ended_;
    std::string last_saved_, last_error_;
    SharedFrame preview_, preview2_;  // The second canvas's in preview2_.
    std::thread pump_;  // Copies preview frames into preview_ while recording.
    std::atomic_bool pumping_{false};
};
#ifdef _WIN32
// Windows ends a session by asking each top-level window first, so a hidden one lets the
// engine save its recording before logging off or shutting down ends the process.
class SessionEnd {
public:
    explicit SessionEnd(std::function<void()> ending) : ending_(std::move(ending)) {
        thread_ = std::thread([this] { run(); });
    }
    ~SessionEnd() {
        if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
        if (thread_.joinable()) thread_.join();
    }

private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<SessionEnd*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_QUERYENDSESSION) return TRUE;
        if (message == WM_ENDSESSION && w && self) {
            self->ending_();
            return 0;
        }
        if (message == WM_CLOSE) {
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, w, l);
    }

    void run() {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = &SessionEnd::procedure;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"RecEngineSession";
        RegisterClassExW(&wc);
        HWND window = CreateWindowExW(0, wc.lpszClassName, L"rec engine", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                      wc.hInstance, nullptr);
        if (!window) return;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        window_ = window;
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    }

    std::function<void()> ending_;
    std::atomic<HWND> window_{nullptr};
    std::thread thread_;
};
#endif
} // namespace

bool launch_engine(const std::string& socket, const std::vector<std::string>& args, std::string& error) {
    const auto log = text(u8path(socket).parent_path() / "engine.log");
    std::error_code ignored;
    fs::create_directories(u8path(socket).parent_path(), ignored);
#ifdef _WIN32
    wchar_t self[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    if (!length || length == std::size(self)) { error = "could not find rec's executable"; return false; }
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE out = CreateFileW(u8path(log).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) { error = "could not open " + log; return false; }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    bool ok = InitializeProcThreadAttributeList(attributes, 1, 0, &size) &&
              UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out, sizeof out, nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullptr;
    startup.StartupInfo.hStdOutput = out;
    startup.StartupInfo.hStdError = out;
    startup.lpAttributeList = attributes;
    std::wstring command = L"\"" + std::wstring(self, length) + L"\"";
    for (const auto& arg : args) {
        const auto line = " " + join({arg});
        const int n = MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), wide.data(), n);
        command += wide;
    }
    PROCESS_INFORMATION process{};
    ok = ok && CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                              DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                              &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(out);
    if (!ok) { error = "could not start the engine"; return false; }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    std::string self(4096, '\0');
#ifdef __APPLE__
    std::uint32_t size = static_cast<std::uint32_t>(self.size());
    if (_NSGetExecutablePath(self.data(), &size) != 0) { error = "could not find rec's executable"; return false; }
#else
    const auto n = readlink("/proc/self/exe", self.data(), self.size() - 1);
    if (n <= 0) { error = "could not find rec's executable"; return false; }
    self[static_cast<std::size_t>(n)] = '\0';
#endif
    self.resize(std::char_traits<char>::length(self.c_str()));
    posix_spawn_file_actions_t files;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&files);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&files, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&files, 1, 2);
    short flags = 0;
#ifdef POSIX_SPAWN_SETSID
    flags |= POSIX_SPAWN_SETSID;
#endif
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attributes, flags);
    std::vector<char*> argv{self.data()};
    std::vector<std::string> owned(args);
    for (auto& arg : owned) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int failed = posix_spawn(&pid, self.c_str(), &files, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attributes);
    if (failed) { error = "could not start the engine"; return false; }
    return true;
#endif
}

std::string engine_socket() {
    if (const char* chosen = std::getenv("REC_ENGINE_SOCKET"); chosen && *chosen) return chosen;
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        return text(u8path(local) / "rec" / "engine.sock");
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return text(u8path(home) / "Library" / "Application Support" / "rec" / "engine.sock");
#else
    if (const char* home = std::getenv("HOME"); home && *home) return text(u8path(home) / ".rec" / "engine.sock");
#endif
    return "rec-engine.sock";
}

std::string join(const std::vector<std::string>& args) {
    std::string line;
    for (const auto& arg : args) {
        if (!line.empty()) line += ' ';
        if (!arg.empty() && arg.find_first_of(" \"\\") == std::string::npos) {
            line += arg;
            continue;
        }
        line += '"';
        for (char c : arg) {
            if (c == '"' || c == '\\') line += '\\';
            line += c;
        }
        line += '"';
    }
    return line;
}

int serve(const std::string& socket, const std::atomic_bool& interrupted, int idle_exit_seconds) {
    std::string error;
    if (LocalSocket::connect(socket, error).valid()) {
        std::cerr << "an engine is already running at " << socket << '\n';
        return 1;
    }
    std::error_code ignored;
    fs::create_directories(u8path(socket).parent_path(), ignored);
    auto listener = LocalSocket::listen(socket, error);
    if (!listener.valid()) { std::cerr << error << '\n'; return 1; }
    std::cout << "Engine listening on " << socket << std::endl;

    Engine engine;
#ifdef _WIN32
    SessionEnd session_end([&] {
        std::cout << "Windows is ending the session" << std::endl;
        engine.close();
    });
#endif
    std::atomic_bool shutdown{false};
    std::atomic_int connections{0};
    constexpr int kMaxConnections = 8;
    auto active = Clock::now();
    while (!interrupted.load() && !shutdown.load()) {
        auto client = listener.accept(200);
        if (engine.tick() || connections.load() > 0 || client.valid()) active = Clock::now();
        if (idle_exit_seconds > 0 && Clock::now() - active > std::chrono::seconds(idle_exit_seconds)) {
            std::cout << "Engine idle for " << idle_exit_seconds << " s" << std::endl;
            break;
        }
        if (!client.valid()) continue;
        if (connections.load() >= kMaxConnections) {
            client.write("error the engine is busy\n");
            continue;
        }
        connections.fetch_add(1);
        std::thread([&, client = std::move(client)]() mutable {
            std::string line;
            bool timed_out = false;
            while (!interrupted.load() && !shutdown.load()) {
                if (!client.read_line(line, 200, timed_out)) {
                    if (timed_out) continue;
                    break;
                }
                bool stop = false;
                const auto reply = engine.handle(line, stop);
                client.write(reply);
                if (stop) shutdown.store(true);
            }
            connections.fetch_sub(1);
        }).detach();
    }
    listener.close();
    std::filesystem::remove(u8path(socket), ignored);
    // Connection threads notice within 200 ms and use the engine until they return.
    while (connections.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    engine.close();
    std::cout << "Engine stopped" << std::endl;
    return 0;
}

int request(const std::string& socket, const std::string& line, bool launch) {
    std::string error;
    auto connection = LocalSocket::connect(socket, error);
    if (!connection.valid() && launch) {
        if (!launch_engine(socket, {"serve"}, error)) { std::cerr << error << '\n'; return 1; }
        for (int i = 0; i < 100 && !connection.valid(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            connection = LocalSocket::connect(socket, error);
        }
    }
    if (!connection.valid()) {
        std::cerr << error << (launch ? "" : "; `rec start` launches it") << '\n';
        return 1;
    }
    if (!connection.write(line + "\n")) { std::cerr << "could not reach the engine\n"; return 1; }
    std::string reply;
    bool timed_out = false;
    for (;;) {
        // Stopping waits for the encoders to finish; there is no deadline for a reply.
        if (!connection.read_line(reply, 1000, timed_out)) {
            if (timed_out) continue;
            std::cerr << "the engine closed the connection\n";
            return 1;
        }
        if (reply == "ok") return 0;
        if (reply.rfind("ok ", 0) == 0) { std::cout << reply.substr(3) << '\n'; return 0; }
        if (reply.rfind("error ", 0) == 0) { std::cerr << reply.substr(6) << '\n'; return 1; }
        std::cout << reply << '\n';
    }
}

} // namespace rec::cli
