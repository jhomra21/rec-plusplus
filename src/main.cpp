#include "rec/rec.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
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
std::atomic_bool interrupted{false};

extern "C" void on_signal(int) { interrupted.store(true); }

enum class Command { toggle, pause, resume, stop, save_replay, show, hide, flip, move, audio };
constexpr std::size_t kScreen = SIZE_MAX;  // show, hide and move the main capture.
struct Posted {
    Command command;
    std::size_t overlay = 0;  // show, hide, flip and move: 0-based, or kScreen.
    std::optional<rec::Area> area;  // move.
    // audio: the source, and its new mute (none: toggle) or volume.
    rec::AudioInput input = rec::AudioInput::system;
    std::optional<bool> muted;
    std::optional<double> gain_db;
};
std::mutex commands_mutex;
std::deque<Posted> commands;

void post(Command command, std::size_t overlay = 0) {
    std::lock_guard lock(commands_mutex);
    commands.push_back({command, overlay});
}

void post_audio(rec::AudioInput input, std::optional<bool> muted, std::optional<double> gain_db = std::nullopt) {
    std::lock_guard lock(commands_mutex);
    commands.push_back({Command::audio, 0, std::nullopt, input, muted, gain_db});
}

void post_move(std::size_t layer, rec::Area area) {
    std::lock_guard lock(commands_mutex);
    commands.push_back({Command::move, layer, area});
}

bool area(std::string_view s, rec::Area& a);

// "screen", or an overlay number from 1.
std::optional<std::size_t> layer_index(std::string_view text) {
    if (text == "screen") return kScreen;
    std::size_t n = 0;
    const auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
    if (ec != std::errc{} || p != text.data() + text.size() || n < 1 || n > 15) return std::nullopt;
    return n - 1;
}

// "system" or "mic" at the start of text.
std::optional<rec::AudioInput> audio_input(std::string_view text) {
    if (text == "system") return rec::AudioInput::system;
    if (text == "mic" || text == "microphone") return rec::AudioInput::microphone;
    return std::nullopt;
}

bool next_command(Posted& posted) {
    std::lock_guard lock(commands_mutex);
    if (commands.empty()) return false;
    posted = commands.front();
    commands.pop_front();
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
// takes one command per line (pause, resume, toggle, save, show N|screen, hide N|screen,
// move N|screen X,Y,W,H, mute SOURCE, unmute SOURCE, volume SOURCE DB, stop), so scripts can
// drive a recording. The reader thread is detached: a pending read must not keep the
// process alive after the movie is saved.
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
                if (key == 'p' || key == 'P' || key == ' ') post(Command::toggle);
                else if (key == 's' || key == 'S') post(Command::save_replay);
                else if (key >= '1' && key <= '9') post(Command::flip, static_cast<std::size_t>(key - '1'));
                else if (key == 'm' || key == 'M') post_audio(rec::AudioInput::microphone, std::nullopt);
                else if (key == 'a' || key == 'A') post_audio(rec::AudioInput::system, std::nullopt);
                else if (key == 'q' || key == 'Q') post(Command::stop);
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
            line = first == std::string::npos ? std::string() : line.substr(first, last - first + 1);
            if (line == "pause") post(Command::pause);
            else if (line == "resume") post(Command::resume);
            else if (line == "toggle") post(Command::toggle);
            else if (line == "save") post(Command::save_replay);
            else if ((line.rfind("show ", 0) == 0 || line.rfind("hide ", 0) == 0) && layer_index(std::string_view(line).substr(5)))
                post(line[0] == 's' ? Command::show : Command::hide, *layer_index(std::string_view(line).substr(5)));
            else if (line.rfind("move ", 0) == 0) {
                const auto space = line.find(' ', 5);
                const auto layer = space == std::string::npos ? std::nullopt : layer_index(std::string_view(line).substr(5, space - 5));
                rec::Area to;
                if (layer && area(std::string_view(line).substr(space + 1), to)) post_move(*layer, to);
                else std::cerr << "Use: move N|screen X,Y,W,H (fractions of the frame)\n";
            }
            else if (line.rfind("mute ", 0) == 0 && audio_input(line.substr(5)))
                post_audio(*audio_input(line.substr(5)), true);
            else if (line.rfind("unmute ", 0) == 0 && audio_input(line.substr(7)))
                post_audio(*audio_input(line.substr(7)), false);
            else if (line.rfind("volume ", 0) == 0) {
                const auto space = line.find(' ', 7);
                const auto input = space == std::string::npos ? std::nullopt : audio_input(line.substr(7, space - 7));
                char* end = nullptr;
                const double db = input ? std::strtod(line.c_str() + space + 1, &end) : 0;
                if (input && end && *end == '\0' && end != line.c_str() + space + 1) post_audio(*input, std::nullopt, db);
                else std::cerr << "Use: volume system|mic DB\n";
            } else if (line == "stop") post(Command::stop);
        }
    }).detach();
}

// rec.mov -> rec-replay-1.mp4, then -2 and so on, skipping names that exist.
std::string replay_path(const std::string& output, unsigned& count) {
    const auto slash = output.find_last_of("/\\");
    const auto dot = output.find_last_of('.');
    const std::string stem = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? output.substr(0, dot) : output;
    for (;;) {
        const std::string path = stem + "-replay-" + std::to_string(++count) + ".mp4";
        std::error_code error;
        if (!std::filesystem::exists(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), error)) return path;
    }
}

std::string clock_text(double seconds) {
    const auto tenths = static_cast<long long>(seconds * 10);
    char text[32];
    std::snprintf(text, sizeof text, "%02lld:%02lld.%lld", tenths / 600, (tenths / 10) % 60, tenths % 10);
    return text;
}

void usage() {
    std::cout << "rec: native screen recorder for macOS and Windows\n\n"
                 "Usage:\n"
                 "  rec devices\n"
                 "  rec windows\n"
                 "  rec record [options]\n"
                 "  rec probe FILE [--sample X,Y,W,H]\n\n"
                 "Options:\n"
                 "  -o, --output FILE              Output .mp4 or .mov (default: rec.mov)\n"
                 "  --display N                    Display number from devices (default: 1)\n"
                 "  --window ID|TEXT               Record one window: an ID from `rec windows`, or part of its title or app\n"
                 "  --region X,Y,W,H               Record part of the display, in its pixels from the top-left corner\n"
                 "  -r, --resolution SIZE          720p, 1080p, 1440p, 4k, or native\n"
                 "  -f, --fps N                    24, 25, 30, 50, 60, or 120\n"
                 "  -t, --duration SECONDS         Recorded length, excluding pauses (0 = until stopped)\n"
                 "  --codec NAME                   auto, h264, hevc\n"
                 "  --bitrate MBPS                 Target megabits/second (0 = automatic)\n"
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
                 "  --stream URL                   Also stream live: rtmp[s]://host[:port]/app/stream-key\n"
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
                 "With stdin piped, send lines: pause, resume, toggle, save, show N|screen, hide N|screen,\n"
                 "move N|screen X,Y,W,H, mute system|mic, unmute system|mic, volume system|mic DB, stop.\n"
                 "rec probe --sample reports the mean color of a rectangle (video pixels) once a second.\n";
}

bool integer(std::string_view s, std::uint32_t& out) {
    const char* end = s.data() + s.size();
    const auto [p, ec] = std::from_chars(s.data(), end, out);
    return ec == std::errc{} && p == end;
}

bool number(std::string_view s, double low, double high, double& out) {
    std::string owned(s);
    char* end = nullptr;
    out = std::strtod(owned.c_str(), &end);
    return end != owned.c_str() && *end == '\0' && std::isfinite(out) && out >= low && out <= high;
}

bool region(std::string_view s, rec::Region& r) {
    std::int32_t values[4]{};
    const char* p = s.data();
    const char* end = s.data() + s.size();
    for (int i = 0; i < 4; ++i) {
        const auto [next, ec] = std::from_chars(p, end, values[i]);
        if (ec != std::errc{}) return false;
        p = next;
        if (i < 3) { if (p == end || *p != ',') return false; ++p; }
    }
    if (p != end || values[0] < 0 || values[1] < 0 || values[2] < 2 || values[3] < 2) return false;
    r = {values[0], values[1], static_cast<std::uint32_t>(values[2]), static_cast<std::uint32_t>(values[3])};
    return true;
}

bool area(std::string_view s, rec::Area& a) {
    double values[4]{};
    std::size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        const auto comma = s.find(',', start);
        if ((i < 3) != (comma != std::string_view::npos)) return false;
        if (!number(s.substr(start, i < 3 ? comma - start : std::string_view::npos), 0, 1, values[i])) return false;
        start = comma + 1;
    }
    a = {values[0], values[1], values[2], values[3]};
    return a.width >= 0.01 && a.height >= 0.01 && a.x + a.width <= 1 + 1e-6 && a.y + a.height <= 1 + 1e-6;
}

// SOURCE or SOURCE@X,Y,W,H. A path may contain @, so only a valid area after the last one counts.
rec::Overlay overlay(rec::Overlay::Kind kind, std::string_view value, rec::Area fallback) {
    rec::Overlay o;
    o.kind = kind;
    o.source = value;
    o.area = fallback;
    if (const auto at = value.rfind('@'); at != std::string_view::npos && area(value.substr(at + 1), o.area))
        o.source = value.substr(0, at);
    return o;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// An ID from `rec windows`, or text in a window's title or app name (the frontmost match).
bool find_window(std::string_view query, std::uint64_t& id, std::string& error) {
    const auto windows = rec::list_windows(error);
    if (!error.empty()) return false;
    std::uint64_t number = 0;
    const auto [p, ec] = std::from_chars(query.data(), query.data() + query.size(), number);
    if (ec == std::errc{} && p == query.data() + query.size()) {
        for (const auto& w : windows)
            if (w.id == number) { id = number; return true; }
    }
    const auto needle = lower(query);
    for (const auto& w : windows)
        if (lower(w.title).find(needle) != std::string::npos || lower(w.app).find(needle) != std::string::npos) {
            id = w.id;
            return true;
        }
    error = "window not found: " + std::string(query) + " (run `rec windows`)";
    return false;
}

bool parse(int argc, char** argv, rec::Options& o, std::string& error) {
    for (int i = 2; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--no-cursor") { o.cursor = false; continue; }
        if (arg == "--clicks") { o.clicks = true; continue; }
        if (arg == "--test-pattern") { o.test_pattern = true; continue; }
        if (arg == "--test-tone") { o.test_tone = true; continue; }
        if (arg == "--test-mic") { o.test_mic = true; continue; }
        if (arg == "--system-audio") { o.system_audio = true; continue; }
        if (arg == "--mute-system") { o.system_muted = true; continue; }
        if (arg == "--mute-mic") { o.mic_muted = true; continue; }
        if (arg == "--hide-screen") { o.screen_visible = false; continue; }
        if (i + 1 >= argc) { error = "missing value for " + std::string(arg); return false; }
        std::string_view value(argv[++i]);
        double real = 0;
        if (arg == "-o" || arg == "--output") o.output = value;
        else if (arg == "--display") {
            if (!integer(value, o.display) || o.display == 0) { error = "invalid display index"; return false; }
        } else if (arg == "--window") {
            if (!find_window(value, o.window, error)) return false;
        } else if (arg == "--region") {
            rec::Region r;
            if (!region(value, r)) { error = "region must be X,Y,WIDTH,HEIGHT in display pixels, at least 2x2"; return false; }
            o.region = r;
        } else if (arg == "-f" || arg == "--fps") {
            if (!integer(value, o.fps) || (o.fps != 24 && o.fps != 25 && o.fps != 30 && o.fps != 50 && o.fps != 60 && o.fps != 120)) {
                error = "fps must be 24, 25, 30, 50, 60, or 120"; return false;
            }
        } else if (arg == "-t" || arg == "--duration") {
            if (!number(value, 0, 86400, o.duration_seconds)) { error = "invalid duration"; return false; }
        } else if (arg == "-r" || arg == "--resolution") {
            if (value == "native") { o.width = o.height = 0; }
            else if (value == "720p") { o.width = 1280; o.height = 720; }
            else if (value == "1080p") { o.width = 1920; o.height = 1080; }
            else if (value == "1440p") { o.width = 2560; o.height = 1440; }
            else if (value == "4k") { o.width = 3840; o.height = 2160; }
            else { error = "resolution must be 720p, 1080p, 1440p, 4k, or native"; return false; }
        } else if (arg == "--codec") {
            if (value == "auto") o.codec = rec::Codec::automatic;
            else if (value == "h264") o.codec = rec::Codec::h264;
            else if (value == "hevc") o.codec = rec::Codec::hevc;
            else { error = "codec must be auto, h264, or hevc"; return false; }
        } else if (arg == "--bitrate") {
            if (!integer(value, o.bitrate_mbps) || o.bitrate_mbps > 300) { error = "bitrate must be between 0 and 300 Mb/s"; return false; }
        } else if (arg == "--stream") {
            if (value.empty()) { error = "--stream needs an rtmp:// or rtmps:// URL"; return false; }
            o.stream_url = value;
        } else if (arg == "--replay") {
            if (!integer(value, o.replay_seconds) || o.replay_seconds < 5 || o.replay_seconds > 300) {
                error = "replay must be between 5 and 300 seconds"; return false;
            }
        } else if (arg == "--stream-bitrate") {
            if (!integer(value, o.stream_kbps) || o.stream_kbps < 500 || o.stream_kbps > 50000) {
                error = "stream bitrate must be between 500 and 50000 kb/s"; return false;
            }
        } else if (arg == "--camera" || arg == "--image") {
            const bool camera = arg == "--camera";
            if (value.empty()) { error = camera ? "--camera needs a device: default, test, or an ID or name from `rec devices`" : "--image needs a file"; return false; }
            if (o.overlays.size() == 15) { error = "at most 15 overlays"; return false; }
            const auto at = value.rfind('@');
            rec::Area checked;
            if (at != std::string_view::npos && !area(value.substr(at + 1), checked) &&
                value.substr(at + 1).find(',') != std::string_view::npos) {
                error = "the area after @ must be X,Y,WIDTH,HEIGHT as fractions of the frame, inside it"; return false;
            }
            o.overlays.push_back(overlay(camera ? rec::Overlay::Kind::camera : rec::Overlay::Kind::image, value,
                                         camera ? rec::Area{0.73, 0.715, 0.25, 0.25} : rec::Area{}));
        } else if (arg == "--add-display" || arg == "--add-window") {
            if (o.overlays.size() == 15) { error = "at most 15 overlays"; return false; }
            rec::Overlay layer = overlay(rec::Overlay::Kind::display, value, rec::Area{});
            const auto at = value.rfind('@');
            if (layer.source.size() == value.size() && at != std::string_view::npos &&
                value.substr(at + 1).find(',') != std::string_view::npos) {
                error = "the area after @ must be X,Y,WIDTH,HEIGHT as fractions of the frame, inside it"; return false;
            }
            const std::string spec = std::move(layer.source);
            std::string_view what = spec;
            layer.source.clear();
            if (arg == "--add-window") {
                layer.kind = rec::Overlay::Kind::window;
                if (!find_window(what, layer.window, error)) return false;
            } else {
                if (const auto colon = what.find(':'); colon != std::string_view::npos) {
                    rec::Region r;
                    if (!region(what.substr(colon + 1), r)) {
                        error = "the region after : must be X,Y,WIDTH,HEIGHT in display pixels, at least 2x2"; return false;
                    }
                    layer.region = r;
                    what = what.substr(0, colon);
                }
                if (!integer(what, layer.display) || layer.display == 0) { error = "--add-display needs a display number"; return false; }
            }
            o.overlays.push_back(layer);
        } else if (arg == "--screen-area") {
            if (!area(value, o.screen_area)) {
                error = "the screen area must be X,Y,WIDTH,HEIGHT as fractions of the frame, inside it"; return false;
            }
        } else if (arg == "--mic") {
            if (value.empty()) { error = "--mic needs a device: default, or an ID or name from `rec devices`"; return false; }
            o.microphone = true;
            o.microphone_device = value;
        } else if (arg == "--audio-tracks") {
            if (value == "both") o.audio_tracks = rec::AudioTracks::both;
            else if (value == "mix") o.audio_tracks = rec::AudioTracks::mix;
            else if (value == "stems") o.audio_tracks = rec::AudioTracks::stems;
            else { error = "audio tracks must be both, mix, or stems"; return false; }
        } else if (arg == "--system-gain" || arg == "--mic-gain") {
            if (!number(value, -60, 24, real)) { error = "gain must be between -60 and 24 dB"; return false; }
            (arg == "--system-gain" ? o.system_gain_db : o.mic_gain_db) = real;
        } else if (arg == "--system-audio-offset-ms" || arg == "--mic-offset-ms") {
            if (!number(value, -1000, 1000, real)) { error = "offset must be between -1000 and 1000 ms"; return false; }
            if (arg == "--mic-offset-ms") o.mic_offset_ms = real;
            else o.system_audio_offset_ms = real;
        } else { error = "unknown option: " + std::string(arg); return false; }
    }
    if ((o.test_tone || o.test_mic) && !o.test_pattern) {
        error = std::string(o.test_tone ? "--test-tone" : "--test-mic") + " requires --test-pattern"; return false;
    }
    if (o.window && o.region) { error = "use --window or --region, not both"; return false; }
    if ((o.window || o.region) && o.test_pattern) {
        error = std::string(o.window ? "--window" : "--region") + " needs a real screen capture"; return false;
    }
    if ((o.system_audio || o.microphone) && o.test_pattern) {
        error = std::string(o.system_audio ? "--system-audio" : "--mic") + " needs a real screen capture"; return false;
    }
    if (o.output.empty() || (o.output.size() < 4 ||
        (o.output.substr(o.output.size() - 4) != ".mov" && o.output.substr(o.output.size() - 4) != ".mp4"))) {
        error = "output must end with .mov or .mp4";
        return false;
    }
    return true;
}

bool terminal() {
#ifdef _WIN32
    return _isatty(1) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

// The stream key is the last path segment; never print it.
std::string redact(const std::string& url) {
    const auto last = url.rfind('/');
    return last == std::string::npos ? url : url.substr(0, last + 1) + "****";
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
            std::cout << "   \033[35m\xE2\x97\x8F LIVE\033[0m " << static_cast<int>(stream_kbps) << " kb/s";
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
    if (command == "probe") {
        std::optional<rec::Region> sample;
        if (argc == 5 && std::string_view(argv[3]) == "--sample") {
            rec::Region r;
            if (!region(argv[4], r)) { std::cerr << "--sample needs X,Y,WIDTH,HEIGHT in video pixels\n"; return 2; }
            sample = r;
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
            std::cout << "sample_" << i << '=' << +probe.samples[i].r << ',' << +probe.samples[i].g << ',' << +probe.samples[i].b << '\n';
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
    if (command != "record") { std::cerr << "unknown command: " << command << '\n'; return 2; }

    rec::Options options;
    std::string error;
    if (!parse(argc, argv, options, error)) { std::cerr << error << '\n'; return 2; }
    rec::Recorder recorder(options);
    if (auto result = recorder.start(); !result) { std::cerr << result.error << '\n'; return 1; }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const bool audio = options.system_audio || options.microphone || options.test_tone || options.test_mic;
    const bool live = terminal();
    const bool streaming = !options.stream_url.empty();
    std::cout << "Recording to " << options.output
              << (!keyboard() ? " (Ctrl-C to stop)\n"
                  : options.replay_seconds ? " (p: pause/resume, s: save replay, q or Ctrl-C: stop)\n"
                                           : " (p: pause/resume, q or Ctrl-C: stop)\n");
    if (options.replay_seconds)
        std::cout << "Keeping the last " << options.replay_seconds << " s for replays at " << options.stream_kbps << " kb/s\n";
    if (streaming) {
        const auto stats = recorder.stats();
        std::cout << "Streaming to " << redact(options.stream_url) << " at " << options.stream_kbps << " kb/s ("
                  << (stats.stream_hardware ? "hardware" : "software") << " H.264)\n";
    }
    std::cout << std::flush;
    watch_input();
    const auto began = std::chrono::steady_clock::now();
    auto shown = began - std::chrono::seconds(1);
    bool reconnecting = false;
    std::uint32_t reconnects = 0;
    auto recorded = [&] {
        const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - began;
        return std::max(0.0, wall.count() - recorder.stats().paused_seconds);
    };
    unsigned replays = 0;
    std::vector<bool> visible;
    for (const auto& o : options.overlays) visible.push_back(o.visible);
    bool screen_visible = options.screen_visible;
    const auto layer_name = [&](std::size_t n) {
        if (n == kScreen) return std::string("the screen");
        static const char* const kinds[] = {"image ", "camera ", "display ", "window "};
        return kinds[static_cast<int>(options.overlays[n].kind)] + std::to_string(n + 1);
    };
    // Volume and mute per source: system audio, then the microphone.
    double gains[2]{options.system_gain_db, options.mic_gain_db};
    bool mutes[2]{options.system_muted, options.mic_muted};
    const bool has_input[2]{options.system_audio || options.test_tone, options.microphone || options.test_mic};
    const auto muted_text = [&] {
        const bool system = has_input[0] && mutes[0], mic = has_input[1] && mutes[1];
        return std::string(system && mic ? "system audio and mic" : system ? "system audio" : mic ? "mic" : "");
    };
    // The stream's send rate, measured over about a second.
    double stream_kbps = 0;
    std::uint64_t rate_bytes = 0;
    auto rate_since = began;
    while (!interrupted.load() && !recorder.finished()) {
        Posted posted;
        while (next_command(posted)) {
            const Command command = posted.command;
            if (command == Command::stop) { interrupted.store(true); break; }
            if (command == Command::show || command == Command::hide || command == Command::flip || command == Command::move) {
                if (live) std::cout << "\r\033[K";
                const std::size_t n = posted.overlay;
                if (n != kScreen && n >= visible.size()) {
                    std::cerr << "No overlay " << n + 1 << "; add one with --camera, --image, --add-display or --add-window\n";
                } else {
                    const bool was = n == kScreen ? screen_visible : static_cast<bool>(visible[n]);
                    const bool show = command == Command::move ? was : command == Command::show || (command == Command::flip && !was);
                    const auto changed = n == kScreen ? recorder.set_screen(show, posted.area) : recorder.set_overlay(n, show, posted.area);
                    if (changed) {
                        if (n == kScreen) screen_visible = show;
                        else visible[n] = show;
                        if (command == Command::move) {
                            const auto& a = *posted.area;
                            std::cout << "Moved " << layer_name(n) << " to " << a.x << ',' << a.y << ',' << a.width << ',' << a.height;
                        } else {
                            std::cout << (show ? "Showing " : "Hiding ") << layer_name(n);
                        }
                        std::cout << " at " << clock_text(recorded()) << '\n';
                    } else {
                        std::cerr << changed.error << '\n';
                    }
                }
                std::cout << std::flush;
                shown -= std::chrono::seconds(1);
                continue;
            }
            if (command == Command::audio) {
                if (live) std::cout << "\r\033[K";
                const auto i = posted.input == rec::AudioInput::system ? 0 : 1;
                const char* name = i == 0 ? "system audio" : "microphone";
                const bool mute = posted.muted.value_or(posted.gain_db ? mutes[i] : !mutes[i]);
                const double gain = posted.gain_db.value_or(gains[i]);
                if (auto changed = recorder.set_volume(posted.input, gain, mute); changed) {
                    if (posted.gain_db)
                        std::cout << "Set " << name << " to " << std::fixed << std::setprecision(1) << gain << " dB";
                    else
                        std::cout << (mute ? "Muted " : "Unmuted ") << name;
                    std::cout << " at " << clock_text(recorded()) << '\n';
                    gains[i] = gain;
                    mutes[i] = mute;
                } else {
                    std::cerr << changed.error << '\n';
                }
                std::cout << std::flush;
                shown -= std::chrono::seconds(1);
                continue;
            }
            if (command == Command::save_replay) {
                if (live) std::cout << "\r\033[K";
                if (!options.replay_seconds) {
                    std::cerr << "No replay buffer; record with --replay SECONDS\n";
                } else {
                    const auto path = replay_path(options.output, replays);
                    double seconds = 0;
                    if (auto saved = recorder.save_replay(path, &seconds); saved)
                        std::cout << "Saved replay (" << std::fixed << std::setprecision(1) << seconds << " s) to " << path << '\n';
                    else
                        std::cerr << "Replay not saved: " << saved.error << '\n';
                }
                std::cout << std::flush;
                shown -= std::chrono::seconds(1);
                continue;
            }
            const bool pause = command == Command::pause ||
                (command == Command::toggle && !recorder.stats().paused);
            const auto changed = pause ? recorder.pause() : recorder.resume();
            if (live) std::cout << "\r\033[K";
            if (changed) std::cout << (pause ? "Paused at " : "Resumed at ") << clock_text(recorded()) << '\n';
            else std::cerr << changed.error << '\n';
            std::cout << std::flush;
            shown -= std::chrono::seconds(1);
        }
        // Outages get their own lines, so a log shows when the stream dropped and came back.
        if (streaming) {
            const auto stats = recorder.stats();
            const bool back = stats.stream_reconnects != reconnects;
            if (back || stats.stream_reconnecting != reconnecting) {
                if (live) std::cout << "\r\033[K";
                if (back) std::cout << "Stream reconnected at " << clock_text(recorded()) << '\n';
                else if (!stats.stream_reconnecting) std::cout << "Stream gave up reconnecting at " << clock_text(recorded()) << '\n';
                if (stats.stream_reconnecting && (back || !reconnecting))
                    std::cout << "Stream lost at " << clock_text(recorded()) << ", reconnecting\n";
                std::cout << std::flush;
                reconnecting = stats.stream_reconnecting;
                reconnects = stats.stream_reconnects;
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
            show_status(recorder.stats(), recorded(), audio, streaming, muted_text(), stream_kbps);
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
    std::cout << "Captured " << stats.captured << ", written " << stats.written
              << ", dropped " << stats.dropped << '\n';
    if (options.system_audio || options.test_tone)
        std::cout << "System audio buffers written " << stats.audio_written << ", dropped " << stats.audio_dropped
                  << " (offset " << std::fixed << std::setprecision(1) << stats.system_audio_offset_ms << " ms)\n";
    if (options.microphone || options.test_mic)
        std::cout << "Microphone buffers written " << stats.mic_written << ", dropped " << stats.mic_dropped << '\n';
    if (stats.mix_written) std::cout << "Mix buffers written " << stats.mix_written << '\n';
    if (std::any_of(options.overlays.begin(), options.overlays.end(), [](const rec::Overlay& o) { return o.kind == rec::Overlay::Kind::camera; }))
        std::cout << "Camera frames " << stats.camera_frames << '\n';
    if (stats.paused_seconds > 0)
        std::cout << "Paused " << std::fixed << std::setprecision(1) << stats.paused_seconds << " s (not in the movie)\n";
    if (streaming)
        std::cout << "Streamed " << stats.stream_frames << " frames (" << std::fixed << std::setprecision(1)
                  << static_cast<double>(stats.stream_bytes) / 1e6 << " MB, "
                  << static_cast<int>(static_cast<double>(stats.stream_bytes) * 8 / 1000 / std::max(1.0, wall.count()))
                  << " kb/s), skipped " << stats.stream_dropped
                  << (stats.stream_reconnects ? ", reconnected " + std::to_string(stats.stream_reconnects) : std::string()) << '\n';
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
