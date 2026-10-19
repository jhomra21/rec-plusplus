#include "cli/engine.hpp"
#include "cli/options.hpp"
#include "rec/rec.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <conio.h>
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace {
using rec::cli::Action;
using rec::cli::Request;
using rec::cli::clock_text;
using rec::cli::redact;

std::atomic_bool interrupted{false};

extern "C" void on_signal(int) { interrupted.store(true); }

std::mutex requests_mutex;
std::deque<Request> requests;

void post(Request request) {
    std::lock_guard lock(requests_mutex);
    requests.push_back(request);
}

void post(Action action, std::size_t layer = 0) {
    Request request;
    request.action = action;
    request.layer = layer;
    post(request);
}

void post_audio_toggle(rec::AudioInput input) {
    Request request;
    request.action = Action::audio;
    request.input = input;
    post(request);
}

bool next_request(Request& request) {
    std::lock_guard lock(requests_mutex);
    if (requests.empty()) return false;
    request = requests.front();
    requests.pop_front();
    return true;
}

bool keyboard() {
#ifdef _WIN32
    // _isatty is also true for NUL; only a real console delivers keys.
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

#ifndef _WIN32
termios saved_terminal{};
bool terminal_changed = false;
void restore_terminal() {
    if (terminal_changed) tcsetattr(STDIN_FILENO, TCSANOW, &saved_terminal);
    terminal_changed = false;
}
#endif

// Keys in a terminal: p or space toggles pause, s saves the replay, 1 to 9 show or hide an
// overlay, m and a mute or unmute the microphone and system audio, q stops. Piped stdin
// takes one control line at a time (see rec::cli::parse_request), so scripts can drive a
// recording. The reader thread is detached: a pending read must not keep the process alive
// after the movie is saved.
void watch_input() {
    if (keyboard()) {
#ifndef _WIN32
        termios raw{};
        if (tcgetattr(STDIN_FILENO, &saved_terminal) == 0) {
            raw = saved_terminal;
            raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            terminal_changed = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
            std::atexit(restore_terminal);
        }
#endif
        std::thread([] {
            for (;;) {
#ifdef _WIN32
                const int key = _getch();
#else
                const int key = std::getchar();
#endif
                if (key == EOF) return;
                if (key == 'p' || key == 'P' || key == ' ') post(Action::toggle);
                else if (key == 's' || key == 'S') post(Action::save_replay);
                else if (key >= '1' && key <= '9') post(Action::flip, static_cast<std::size_t>(key - '1'));
                else if (key == 'm' || key == 'M') post_audio_toggle(rec::AudioInput::microphone);
                else if (key == 'a' || key == 'A') post_audio_toggle(rec::AudioInput::system);
                else if (key == 'q' || key == 'Q') post(Action::stop);
            }
        }).detach();
        return;
    }
    std::thread([] {
        std::string line;
        while (std::getline(std::cin, line)) {
            // Some writers (.NET, PowerShell) start the stream with a UTF-8 byte-order mark.
            if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
            const auto first = line.find_first_not_of(" \t\r");
            const auto last = line.find_last_not_of(" \t\r");
            if (first == std::string::npos) continue;
            line = line.substr(first, last - first + 1);
            std::string error;
            if (const auto request = rec::cli::parse_request(line, error)) post(*request);
            else std::cerr << error << '\n';
        }
    }).detach();
}

void usage() {
    std::cout << "rec: native screen recorder for macOS and Windows\n\n"
                 "Usage:\n"
                 "  rec devices\n"
                 "  rec windows\n"
                 "  rec record [options]\n"
                 "  rec probe FILE [--sample X,Y,W,H]\n"
                 "  rec start [options]            Record in the background engine (launched if needed)\n"
                 "  rec stop | status | pause | resume\n"
                 "  rec send LINE                  Any control line below, e.g. rec send \"hide 1\"\n"
                 "  rec serve                      Run the engine in this terminal\n"
                 "  rec shutdown                   Stop the engine (saving its recording)\n\n"
                 "Options:\n"
                 "  -o, --output FILE              Output .mp4 or .mov (default: rec.mov)\n"
                 "  --display N                    Display number from devices (default: 1)\n"
                 "  --window ID|TEXT               Record one window: an ID from `rec windows`, or part of its title or app\n"
                 "  --region X,Y,W,H               Record part of the display, in its pixels from the top-left corner\n"
                 "  -r, --resolution SIZE          720p, 1080p, 1440p, 4k, or native\n"
                 "  -f, --fps N                    24, 25, 30, 50, 60, or 120\n"
                 "  -t, --duration SECONDS         Recorded length, excluding pauses (0 = until stopped; record: 10,\n"
                 "                                 start: 0)\n"
                 "  --codec NAME                   auto, h264, hevc, or prores (ProRes 422: macOS, .mov)\n"
                 "  --bitrate MBPS                 Target megabits/second (0 = automatic)\n"
                 "  --scaler NAME                  lanczos (GPU Lanczos-3 from native size, default) or fast (capture service)\n"
                 "  --no-cursor                    Do not record the cursor\n"
                 "  --clicks                       Mark mouse presses: yellow left, blue right\n"
                 "  --system-audio                 Record system audio\n"
                 "  --mic DEVICE                   Record a microphone: default, or an ID or name from devices\n"
                 "  --audio-tracks LAYOUT          both (mix + stems), mix, or stems (default: both)\n"
                 "  --system-gain DB               System audio volume (-60 to 24); stems stay as captured\n"
                 "  --mic-gain DB                  Microphone volume (-60 to 24); stems stay as captured\n"
                 "  --mute-system, --mute-mic      Start with the source muted (unmute with a or m)\n"
                 "  --system-audio-offset-ms MS    Delay system audio (default: output device latency)\n"
                 "  --mic-offset-ms MS             Delay the microphone (default: 0)\n"
                 "  --stream URL                   Also stream live: rtmp[s]://host[:port]/app/stream-key; repeat for up to 4 servers\n"
                 "  --stream-bitrate KBPS          Stream video kilobits/second (default: 6000)\n"
                 "  --replay SECONDS               Keep the last 5 to 300 seconds in memory; s saves them as an MP4\n"
                 "  --camera DEVICE[@X,Y,W,H]      Show a camera (default, or an ID or name from devices; test for a\n"
                 "                                 synthetic one) over the capture, by default in the bottom-right corner\n"
                 "  --image FILE[@X,Y,W,H]         Show a PNG or JPEG over the capture (default: the whole frame)\n"
                 "  --add-display N[:X,Y,W,H][@X,Y,W,H]\n"
                 "                                 Show another display live, or the part of it after : in its pixels\n"
                 "  --add-window ID|TEXT[@X,Y,W,H] Show a window live (as --window finds it)\n"
                 "                                 X,Y,W,H after @ are fractions of the frame, e.g. 0.73,0.715,0.25,0.25\n"
                 "  --screen-area X,Y,W,H          Where the capture itself sits in the frame (default: all of it)\n"
                 "  --hide-screen                  Start with the capture hidden, so only overlays show\n"
                 "  --test-pattern                 Synthetic video that flashes once a second\n"
                 "  --test-tone                    Add a beep with each flash as system audio\n"
                 "  --test-mic                     Add a quiet synthetic microphone\n\n"
                 "Up to 15 cameras, images, displays and windows draw over the capture, later ones on top.\n"
                 "While recording, press p or space to pause and resume, s to save the replay, 1 to 9 to show or\n"
                 "hide an overlay (in the order given), m or a to mute or unmute the microphone or system\n"
                 "audio, q or Ctrl-C to stop.\n"
                 "With stdin piped, and with rec send, the control lines are: pause, resume, toggle, save,\n"
                 "show N|screen, hide N|screen, move N|screen X,Y,W,H, mute system|mic, unmute system|mic,\n"
                 "volume system|mic DB, stop.\n"
                 "The engine keeps recording after the terminal or app that started it exits. Its socket is\n"
                 "per user (REC_ENGINE_SOCKET overrides it) and it logs to engine.log beside the socket.\n"
                 "rec probe --sample reports the mean color of a rectangle (video pixels) once a second, and the spread\n"
                 "(standard deviation) of its luma, which shows detail and aliasing.\n";
}

bool terminal() {
#ifdef _WIN32
    return _isatty(1) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

// One rewritten line: elapsed time and anything that needs attention.
void show_status(const rec::Stats& stats, double recorded, bool audio, bool streaming, const std::string& muted,
                 double stream_kbps) {
    if (stats.paused) std::cout << "\r\033[33m\xE2\x9D\x9A\xE2\x9D\x9A PAUSED\033[0m ";
    else std::cout << "\r\033[31m\xE2\x97\x8F\033[0m REC    ";
    std::cout << clock_text(recorded) << "   " << stats.written << " frames";
    if (stats.dropped) std::cout << ", \033[33m" << stats.dropped << " dropped\033[0m";
    if (audio && stats.audio_dropped + stats.mic_dropped)
        std::cout << ", \033[33maudio drops " << stats.audio_dropped + stats.mic_dropped << "\033[0m";
    if (!muted.empty()) std::cout << "   \033[33m" << muted << " muted\033[0m";
    if (streaming) {
        if (stats.streaming) {
            std::cout << "   \033[35m\xE2\x97\x8F LIVE\033[0m ";
            if (stats.stream_count > 1) {
                const auto on = std::count_if(stats.streams.begin(), stats.streams.begin() + stats.stream_count,
                                              [](const rec::StreamStats& s) { return s.live; });
                std::cout << on << "/" << stats.stream_count << " ";
            }
            std::cout << static_cast<int>(stream_kbps) << " kb/s";
            // Over a second of media waiting means the network is not keeping up.
            if (stats.stream_queued_ms > 1000)
                std::cout << ", \033[33m" << std::fixed << std::setprecision(1) << stats.stream_queued_ms / 1000.0 << " s behind\033[0m";
        }
        else if (stats.stream_reconnecting) std::cout << "   \033[33mreconnecting\033[0m";
        else std::cout << "   \033[33mstream lost\033[0m";
        if (stats.stream_dropped) std::cout << ", \033[33m" << stats.stream_dropped << " skipped\033[0m";
    }
    if (stats.replay_seconds > 0) std::cout << "   replay " << static_cast<int>(stats.replay_seconds) << " s";
    std::cout << "\033[K" << std::flush;
}

int probe(int argc, char** argv) {
    std::optional<rec::Region> sample;
    if (argc == 5 && std::string_view(argv[3]) == "--sample") {
        rec::Options parsed;
        std::string error;
        if (!rec::cli::parse_options({"--region", argv[4]}, parsed, error)) {
            std::cerr << "--sample needs X,Y,WIDTH,HEIGHT in video pixels\n"; return 2;
        }
        sample = parsed.region;
    } else if (argc != 3) {
        std::cerr << "usage: rec probe FILE [--sample X,Y,W,H]\n"; return 2;
    }
    rec::Probe probe;
    if (auto result = rec::probe_file(argv[2], probe, sample); !result) {
        std::cerr << result.error << '\n'; return 1;
    }
    std::cout << "codec=" << probe.codec << "\nwidth=" << probe.width
              << "\nheight=" << probe.height << "\nframes=" << probe.frames
              << "\nduration=" << probe.duration_seconds
              << "\naudio_tracks=" << probe.audio_tracks
              << "\naudio_codec=" << probe.audio_codec << "\naudio_buffers=" << probe.audio_buffers
              << "\naudio_duration=" << probe.audio_duration_seconds
              << "\nav_sync_events=" << probe.sync_events
              << "\nav_offset_ms=" << probe.av_offset_ms
              << "\nav_offset_max_ms=" << probe.av_offset_max_ms << '\n';
    for (std::size_t i = 0; i < probe.samples.size(); ++i)
        std::cout << "sample_" << i << '=' << +probe.samples[i].r << ',' << +probe.samples[i].g << ',' << +probe.samples[i].b
                  << "\nsample_" << i << "_spread=" << probe.samples[i].spread << '\n';
    const auto times = [](const char* key, const std::vector<double>& values) {
        std::cout << key << '=';
        for (std::size_t i = 0; i < values.size(); ++i) {
            char text[32];
            std::snprintf(text, sizeof text, "%s%.4f", i ? "," : "", values[i]);
            std::cout << text;
        }
        std::cout << '\n';
    };
    times("flashes", probe.flashes);
    times("beeps", probe.beeps);
    return 0;
}

// rec start, stop, status, pause, resume, send, serve and shutdown: the engine and its clients.
std::optional<int> engine_command(std::string_view command, int argc, char** argv) {
    const auto socket = rec::cli::engine_socket();
    std::vector<std::string> args(argv + 2, argv + argc);
    if (command == "serve") {
        if (!args.empty()) { std::cerr << "serve takes no options; set REC_ENGINE_SOCKET to choose the socket\n"; return 2; }
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        return rec::cli::serve(socket, interrupted);
    }
    if (command == "start") {
        // Until stopped unless -t says otherwise; check the options here so mistakes show at once.
        args.insert(args.begin(), {"-t", "0"});
        rec::Options options;
        std::string error;
        if (!rec::cli::parse_options(args, options, error)) { std::cerr << error << '\n'; return 2; }
        std::error_code ignored;
        const auto here = std::filesystem::current_path(ignored).u8string();
        args.insert(args.begin(), {"--cwd", std::string(reinterpret_cast<const char*>(here.data()), here.size())});
        return rec::cli::request(socket, "start " + rec::cli::join(args), true);
    }
    if (command == "stop" || command == "status" || command == "pause" || command == "resume" || command == "shutdown") {
        if (!args.empty()) { std::cerr << command << " takes no options\n"; return 2; }
        return rec::cli::request(socket, std::string(command), false);
    }
    if (command == "send") {
        if (args.empty()) { std::cerr << "usage: rec send LINE, e.g. rec send \"hide 1\"\n"; return 2; }
        std::string line;
        for (const auto& arg : args) line += (line.empty() ? "" : " ") + arg;
        return rec::cli::request(socket, line, false);
    }
    return std::nullopt;
}
} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // UTF-8 output and ANSI colors for the status line.
    SetConsoleOutputCP(CP_UTF8);
    if (HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE); out != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(out, &mode)) SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
    if (argc == 1 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
        usage(); return 0;
    }
    const std::string_view command(argv[1]);
    if (command == "devices") {
        if (argc != 2) { std::cerr << "devices takes no options\n"; return 2; }
        int status = 0;
        std::string error;
        auto screens = rec::list_displays(error);
        if (!error.empty()) { std::cerr << error << '\n'; status = 1; }
        std::cout << "Displays:\n";
        for (std::size_t i = 0; i < screens.size(); ++i) {
            const auto& screen = screens[i];
            std::cout << "  " << i + 1 << ": " << screen.name << " (" << screen.width << "x" << screen.height
                      << ", id " << screen.id << ")\n";
        }
        auto microphones = rec::list_microphones(error);
        if (!error.empty()) { std::cerr << error << '\n'; status = 1; }
        std::cout << "Microphones:\n";
        for (const auto& mic : microphones) std::cout << "  " << mic.name << " (id " << mic.id << ")\n";
        auto cameras = rec::list_cameras(error);
        if (!error.empty()) { std::cerr << error << '\n'; status = 1; }
        std::cout << "Cameras:\n";
        for (const auto& camera : cameras) std::cout << "  " << camera.name << " (id " << camera.id << ")\n";
        std::cout << "Default output latency: " << std::fixed << std::setprecision(1)
                  << rec::default_output_latency_ms() << " ms\n";
        return status;
    }
    if (command == "windows") {
        if (argc != 2) { std::cerr << "windows takes no options\n"; return 2; }
        std::string error;
        const auto windows = rec::list_windows(error);
        if (!error.empty()) { std::cerr << error << '\n'; return 1; }
        std::cout << "Windows:\n";
        for (const auto& w : windows)
            std::cout << "  " << w.id << ": " << (w.app.empty() ? "" : w.app + " - ") << w.title << " (" << w.width
                      << "x" << w.height << ")\n";
        return 0;
    }
    if (command == "probe") return probe(argc, argv);
    if (const auto status = engine_command(command, argc, argv)) return *status;
    if (command != "record") { std::cerr << "unknown command: " << command << '\n'; return 2; }

    rec::Options options;
    std::string error;
    if (!rec::cli::parse_options(std::vector<std::string>(argv + 2, argv + argc), options, error)) {
        std::cerr << error << '\n'; return 2;
    }
    rec::Recorder recorder(options);
    if (auto result = recorder.start(); !result) { std::cerr << result.error << '\n'; return 1; }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const bool audio = options.system_audio || options.microphone || options.test_tone || options.test_mic;
    const bool live = terminal();
    const bool streaming = !options.stream_urls.empty();
    std::cout << "Recording to " << options.output
              << (!keyboard() ? " (Ctrl-C to stop)\n"
                  : options.replay_seconds ? " (p: pause/resume, s: save replay, q or Ctrl-C: stop)\n"
                                           : " (p: pause/resume, q or Ctrl-C: stop)\n");
    if (options.replay_seconds)
        std::cout << "Keeping the last " << options.replay_seconds << " s for replays at " << options.stream_kbps << " kb/s\n";
    if (streaming) {
        const auto stats = recorder.stats();
        std::cout << "Streaming to ";
        for (std::size_t i = 0; i < options.stream_urls.size(); ++i) std::cout << (i ? ", " : "") << redact(options.stream_urls[i]);
        std::cout << " at " << options.stream_kbps << " kb/s ("
                  << (stats.stream_hardware ? "hardware" : "software") << " H.264)\n";
    }
    std::cout << std::flush;
    watch_input();
    const auto began = std::chrono::steady_clock::now();
    auto shown = began - std::chrono::seconds(1);
    std::array<bool, rec::kMaxStreams> reconnecting{};
    std::array<std::uint32_t, rec::kMaxStreams> reconnects{};
    auto recorded = [&] {
        const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - began;
        return std::max(0.0, wall.count() - recorder.stats().paused_seconds);
    };
    rec::cli::Controls controls(recorder, options);
    // The stream's send rate, measured over about a second.
    double stream_kbps = 0;
    std::uint64_t rate_bytes = 0;
    auto rate_since = began;
    while (!interrupted.load() && !recorder.finished()) {
        Request request;
        while (next_request(request)) {
            if (request.action == Action::stop) { interrupted.store(true); break; }
            if (live) std::cout << "\r\033[K";
            std::string message;
            if (auto applied = controls.apply(request, recorded(), message); applied) std::cout << message << '\n';
            else std::cerr << applied.error << '\n';
            std::cout << std::flush;
            shown -= std::chrono::seconds(1);
        }
        // Outages get their own lines, so a log shows when a stream dropped and came back.
        if (streaming) {
            const auto stats = recorder.stats();
            for (std::uint32_t i = 0; i < stats.stream_count; ++i) {
                const auto& s = stats.streams[i];
                const bool back = s.reconnects != reconnects[i];
                if (!back && s.reconnecting == reconnecting[i]) continue;
                const auto name = stats.stream_count > 1 ? "Stream " + redact(options.stream_urls[i]) : std::string("Stream");
                if (live) std::cout << "\r\033[K";
                if (back) std::cout << name << " reconnected at " << clock_text(recorded()) << '\n';
                else if (!s.reconnecting) std::cout << name << " gave up reconnecting at " << clock_text(recorded()) << '\n';
                if (s.reconnecting && (back || !reconnecting[i]))
                    std::cout << name << " lost at " << clock_text(recorded()) << ", reconnecting\n";
                std::cout << std::flush;
                reconnecting[i] = s.reconnecting;
                reconnects[i] = s.reconnects;
                shown -= std::chrono::seconds(1);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (streaming && now - rate_since >= std::chrono::seconds(1)) {
            const auto bytes = recorder.stats().stream_bytes;
            stream_kbps = static_cast<double>(bytes - rate_bytes) * 8 / 1000 /
                          std::chrono::duration<double>(now - rate_since).count();
            rate_bytes = bytes;
            rate_since = now;
        }
        if (live && now - shown >= std::chrono::milliseconds(250)) {
            show_status(recorder.stats(), recorded(), audio, streaming, controls.muted(), stream_kbps);
            shown = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (live) std::cout << "\r\033[KFinishing...\n" << std::flush;
#ifndef _WIN32
    restore_terminal();
#endif
    const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - began;
    const auto stopped = recorder.stop();
    const auto stats = recorder.stats();
    std::cout << rec::cli::summary(options, stats, wall.count());
    if (!stopped) { std::cerr << stopped.error << '\n'; return 1; }
    if (!recorder.failure().empty()) { std::cerr << recorder.failure() << '\n'; return 1; }
    if (stats.window_closed) std::cout << "The window closed, so the recording stopped.\n";
    std::cout << "Saved " << options.output << '\n';
    if (const auto lost = recorder.stream_failure(); !lost.empty()) {
        std::cerr << "Stream stopped early: " << lost << '\n';
        return 1;
    }
    return 0;
}
