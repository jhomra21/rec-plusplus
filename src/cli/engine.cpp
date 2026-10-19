#include "cli/engine.hpp"

#include "cli/local_socket.hpp"
#include "cli/options.hpp"
#include "rec/rec.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
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

// Splits a request line into arguments, undoing join().
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

// One recording at a time. Requests come from any connection's thread.
class Engine {
public:
    std::string handle(const std::string& line, bool& shutdown) {
        std::lock_guard lock(mutex_);
        if (line == "start" || line.rfind("start ", 0) == 0) return start(line.size() > 6 ? line.substr(6) : std::string());
        if (line == "status") return status();
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
        std::cout << message << std::endl;
        return "ok " + message + "\n";
    }

    // Saves a recording that ended by itself: its duration, a closed window, a failure.
    void tick() {
        std::lock_guard lock(mutex_);
        if (recorder_ && recorder_->finished()) end();
    }

    void close() {
        std::lock_guard lock(mutex_);
        if (recorder_) end();
    }

private:
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
        for (auto& overlay : options.overlays)
            if (overlay.kind == Overlay::Kind::image) resolve(overlay.source);
        options_ = std::move(options);
        auto recorder = std::make_unique<Recorder>(options_);
        if (auto started = recorder->start(); !started) return "error " + started.error + "\n";
        recorder_ = std::move(recorder);
        controls_ = std::make_unique<Controls>(*recorder_, options_);
        began_ = Clock::now();
        last_error_.clear();
        std::cout << "Recording to " << options_.output << std::endl;
        return "ok Recording to " + options_.output + "\n";
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
        const auto stopped = recorder_->stop();
        const auto stats = recorder_->stats();
        std::string reply = summary(options_, stats, wall.count());
        std::string failure = !stopped ? stopped.error : recorder_->failure();
        if (failure.empty() && !recorder_->stream_failure().empty())
            failure = "saved " + options_.output + ", but the stream stopped early: " + recorder_->stream_failure();
        if (stats.window_closed) reply += "The window closed, so the recording stopped.\n";
        controls_.reset();
        recorder_.reset();
        if (failure.empty()) {
            last_saved_ = options_.output;
            return reply + "ok Saved " + options_.output + "\n";
        }
        last_error_ = failure;
        return reply + "error " + failure + "\n";
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
    std::string last_saved_, last_error_;
};

// Starts `rec serve` detached from this process, its output going to engine.log beside the
// socket. Only the log is inherited, so the engine never holds the caller's pipes open.
bool launch_engine(const std::string& socket, std::string& error) {
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
    std::wstring command = L"\"" + std::wstring(self, length) + L"\" serve";
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
    char* argv[] = {self.data(), const_cast<char*>("serve"), nullptr};
    pid_t pid = 0;
    const int failed = posix_spawn(&pid, self.c_str(), &files, &attributes, argv, environ);
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attributes);
    if (failed) { error = "could not start the engine"; return false; }
    return true;
#endif
}
} // namespace

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

int serve(const std::string& socket, const std::atomic_bool& interrupted) {
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
    std::atomic_bool shutdown{false};
    std::atomic_int connections{0};
    constexpr int kMaxConnections = 8;
    while (!interrupted.load() && !shutdown.load()) {
        auto client = listener.accept(200);
        engine.tick();
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
        if (!launch_engine(socket, error)) { std::cerr << error << '\n'; return 1; }
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
