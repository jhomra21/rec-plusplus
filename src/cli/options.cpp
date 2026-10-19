#include "cli/options.hpp"
#include "core/shape.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace rec::cli {
namespace {
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

bool region(std::string_view s, Region& r) {
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

bool area(std::string_view s, Area& a) {
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

// WIDTHxHEIGHT
bool canvas_size(std::string_view s, std::uint32_t& width, std::uint32_t& height) {
    const auto x = s.find('x');
    return x != std::string_view::npos && integer(s.substr(0, x), width) && integer(s.substr(x + 1), height);
}

// N|screen:X,Y,W,H or N|screen:X,Y,W,H:CX,CY,CW,CH for --canvas2-place and --crop's N|screen:CROP.
bool layer_areas(std::string_view s, std::size_t& layer, Area& first, std::optional<Area>& second) {
    const auto colon = s.find(':');
    if (colon == std::string_view::npos) return false;
    const auto name = s.substr(0, colon);
    if (name == "screen") layer = kScreen;
    else {
        std::uint32_t n = 0;
        if (!integer(name, n) || n < 1 || n > 15) return false;
        layer = n - 1;
    }
    auto rest = s.substr(colon + 1);
    const auto next = rest.find(':');
    if (!area(rest.substr(0, next), first)) return false;
    if (next == std::string_view::npos) return true;
    Area crop;
    if (!area(rest.substr(next + 1), crop)) return false;
    second = crop;
    return true;
}

// SOURCE or SOURCE@X,Y,W,H. A path may contain @, so only a valid area after the last one counts.
Overlay overlay(Overlay::Kind kind, std::string_view value, Area fallback) {
    Overlay o;
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
    const auto windows = list_windows(error);
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

// "screen", or an overlay number from 1.
std::optional<std::size_t> layer_index(std::string_view text) {
    if (text == "screen") return kScreen;
    std::size_t n = 0;
    const auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
    if (ec != std::errc{} || p != text.data() + text.size() || n < 1 || n > 15) return std::nullopt;
    return n - 1;
}

std::optional<AudioInput> audio_input(std::string_view text) {
    if (text == "system") return AudioInput::system;
    if (text == "mic" || text == "microphone") return AudioInput::microphone;
    return std::nullopt;
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
} // namespace

bool parse_options(const std::vector<std::string>& args, Options& o, std::string& error) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg(args[i]);
        if (arg == "--no-cursor") { o.cursor = false; continue; }
        if (arg == "--clicks") { o.clicks = true; continue; }
        if (arg == "--test-pattern") { o.test_pattern = true; continue; }
        if (arg == "--test-tone") { o.test_tone = true; continue; }
        if (arg == "--test-mic") { o.test_mic = true; continue; }
        if (arg == "--system-audio") { o.system_audio = true; continue; }
        if (arg == "--mute-system") { o.system_muted = true; continue; }
        if (arg == "--mute-mic") { o.mic_muted = true; continue; }
        if (arg == "--hide-screen") { o.screen_visible = false; continue; }
        if (arg == "--canvas2-hide-screen") { o.canvas2.screen.visible = false; continue; }
        if (i + 1 >= args.size()) { error = "missing value for " + std::string(arg); return false; }
        std::string_view value(args[++i]);
        double real = 0;
        if (arg == "-o" || arg == "--output") o.output = value;
        else if (arg == "--display") {
            if (!integer(value, o.display) || o.display == 0) { error = "invalid display index"; return false; }
        } else if (arg == "--window") {
            if (!find_window(value, o.window, error)) return false;
        } else if (arg == "--region") {
            Region r;
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
            if (value == "auto") o.codec = Codec::automatic;
            else if (value == "h264") o.codec = Codec::h264;
            else if (value == "hevc") o.codec = Codec::hevc;
            else if (value == "prores") o.codec = Codec::prores;
            else { error = "codec must be auto, h264, hevc, or prores"; return false; }
        } else if (arg == "--scaler") {
            if (value == "lanczos") o.scaler = Scaler::lanczos;
            else if (value == "fast") o.scaler = Scaler::fast;
            else { error = "scaler must be lanczos or fast"; return false; }
        } else if (arg == "--bitrate") {
            if (!integer(value, o.bitrate_mbps) || o.bitrate_mbps > 300) { error = "bitrate must be between 0 and 300 Mb/s"; return false; }
        } else if (arg == "--stream") {
            if (value.empty()) { error = "--stream needs an rtmp:// or rtmps:// URL"; return false; }
            if (o.stream_urls.size() == kMaxStreams) { error = "at most " + std::to_string(kMaxStreams) + " --stream servers"; return false; }
            o.stream_urls.emplace_back(value);
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
            Area checked;
            if (at != std::string_view::npos && !area(value.substr(at + 1), checked) &&
                value.substr(at + 1).find(',') != std::string_view::npos) {
                error = "the area after @ must be X,Y,WIDTH,HEIGHT as fractions of the frame, inside it"; return false;
            }
            o.overlays.push_back(overlay(camera ? Overlay::Kind::camera : Overlay::Kind::image, value,
                                         camera ? Area{0.73, 0.715, 0.25, 0.25} : Area{}));
        } else if (arg == "--camera-style") {
            if (o.overlays.empty() || o.overlays.back().kind != Overlay::Kind::camera) {
                error = "--camera-style follows the --camera it styles"; return false;
            }
            if (!core::parse_style(value, o.overlays.back().style, error)) return false;
        } else if (arg == "--add-display" || arg == "--add-window") {
            if (o.overlays.size() == 15) { error = "at most 15 overlays"; return false; }
            Overlay layer = overlay(Overlay::Kind::display, value, Area{});
            const auto at = value.rfind('@');
            if (layer.source.size() == value.size() && at != std::string_view::npos &&
                value.substr(at + 1).find(',') != std::string_view::npos) {
                error = "the area after @ must be X,Y,WIDTH,HEIGHT as fractions of the frame, inside it"; return false;
            }
            const std::string spec = std::move(layer.source);
            std::string_view what = spec;
            layer.source.clear();
            if (arg == "--add-window") {
                layer.kind = Overlay::Kind::window;
                if (!find_window(what, layer.window, error)) return false;
            } else {
                if (const auto colon = what.find(':'); colon != std::string_view::npos) {
                    Region r;
                    if (!region(what.substr(colon + 1), r)) {
                        error = "the region after : must be X,Y,WIDTH,HEIGHT in display pixels, at least 2x2"; return false;
                    }
                    layer.region = r;
                    what = what.substr(0, colon);
                }
                if (!integer(what, layer.display) || layer.display == 0) { error = "--add-display needs a display number"; return false; }
            }
            o.overlays.push_back(layer);
        } else if (arg == "--canvas2") {
            if (value == "vertical") { o.canvas2.width = 1080; o.canvas2.height = 1920; }
            else if (!canvas_size(value, o.canvas2.width, o.canvas2.height)) {
                error = "--canvas2 must be WIDTHxHEIGHT, for example 1080x1920, or vertical"; return false;
            }
        } else if (arg == "--canvas2-stream") {
            if (value.empty()) { error = "--canvas2-stream needs an rtmp:// or rtmps:// URL"; return false; }
            o.canvas2.stream_urls.emplace_back(value);
        } else if (arg == "--canvas2-output") {
            o.canvas2.output = value;
        } else if (arg == "--canvas2-place" || arg == "--crop") {
            std::size_t layer = 0;
            Area first;
            std::optional<Area> crop;
            const bool place = arg == "--canvas2-place";
            if (!layer_areas(value, layer, first, crop) || (!place && crop)) {
                error = place ? "--canvas2-place must be N|screen:X,Y,W,H or N|screen:X,Y,W,H:CROP_X,CROP_Y,CROP_W,CROP_H (fractions)"
                              : "--crop must be N|screen:X,Y,W,H, fractions of the source";
                return false;
            }
            if (layer != kScreen && layer >= o.overlays.size()) {
                error = std::string(arg) + " " + std::to_string(layer + 1) + ": add the overlay first"; return false;
            }
            if (!place) {
                (layer == kScreen ? o.screen_crop : o.overlays[layer].crop) = first;
            } else if (layer == kScreen) {
                o.canvas2.screen.area = first;
                if (crop) o.canvas2.screen.crop = *crop;
            } else {
                while (o.canvas2.overlays.size() <= layer) {
                    const auto& source = o.overlays[o.canvas2.overlays.size()];
                    o.canvas2.overlays.push_back({source.visible, source.area, source.crop});
                }
                o.canvas2.overlays[layer].area = first;
                if (crop) o.canvas2.overlays[layer].crop = *crop;
            }
        } else if (arg == "--canvas2-hide") {
            std::uint32_t n = 0;
            if (!integer(value, n) || n < 1 || n > o.overlays.size()) { error = "--canvas2-hide needs an overlay number"; return false; }
            while (o.canvas2.overlays.size() < n) {
                const auto& source = o.overlays[o.canvas2.overlays.size()];
                o.canvas2.overlays.push_back({source.visible, source.area, source.crop});
            }
            o.canvas2.overlays[n - 1].visible = false;
        } else if (arg == "--screen-area") {
            if (!area(value, o.screen_area)) {
                error = "the screen area must be X,Y,WIDTH,HEIGHT as fractions of the frame, inside it"; return false;
            }
        } else if (arg == "--mic") {
            if (value.empty()) { error = "--mic needs a device: default, or an ID or name from `rec devices`"; return false; }
            o.microphone = true;
            o.microphone_device = value;
        } else if (arg == "--audio-tracks") {
            if (value == "both") o.audio_tracks = AudioTracks::both;
            else if (value == "mix") o.audio_tracks = AudioTracks::mix;
            else if (value == "stems") o.audio_tracks = AudioTracks::stems;
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
    if ((!o.canvas2.stream_urls.empty() || !o.canvas2.output.empty()) && !o.canvas2.width) {
        error = "--canvas2-stream and --canvas2-output need --canvas2"; return false;
    }
    if (o.stream_urls.size() + o.canvas2.stream_urls.size() > kMaxStreams) {
        error = "at most " + std::to_string(kMaxStreams) + " stream servers across both canvases"; return false;
    }
    if (o.output.empty() || (o.output.size() < 4 ||
        (o.output.substr(o.output.size() - 4) != ".mov" && o.output.substr(o.output.size() - 4) != ".mp4"))) {
        error = "output must end with .mov or .mp4";
        return false;
    }
    return true;
}

std::optional<Request> parse_request(std::string_view line, std::string& error) {
    const auto rest = [&](std::size_t from) { return line.substr(std::min(from, line.size())); };
    Request r;
    if (line == "pause") r.action = Action::pause;
    else if (line == "resume") r.action = Action::resume;
    else if (line == "toggle") r.action = Action::toggle;
    else if (line == "save") r.action = Action::save_replay;
    else if (line == "stop") r.action = Action::stop;
    else if (line.rfind("show ", 0) == 0 || line.rfind("hide ", 0) == 0) {
        const auto layer = layer_index(rest(5));
        if (!layer) { error = "Use: show N|screen or hide N|screen"; return std::nullopt; }
        r.action = line[0] == 's' ? Action::show : Action::hide;
        r.layer = *layer;
    } else if (line.rfind("move ", 0) == 0) {
        const auto space = line.find(' ', 5);
        const auto layer = space == std::string_view::npos ? std::nullopt : layer_index(line.substr(5, space - 5));
        Area to;
        if (!layer || !area(rest(space + 1), to)) { error = "Use: move N|screen X,Y,W,H (fractions of the frame)"; return std::nullopt; }
        r.action = Action::move;
        r.layer = *layer;
        r.area = to;
    } else if (line.rfind("place ", 0) == 0) {
        std::istringstream words{std::string(rest(6))};
        std::vector<std::string> w;
        for (std::string word; words >> word;) w.push_back(word);
        std::size_t i = 0;
        if (!w.empty() && w[0] == "canvas2") { r.canvas = kSecondCanvas; ++i; }
        const auto layer = w.size() > i ? layer_index(w[i]) : std::nullopt;
        Area to, crop;
        const bool ok = layer && (w.size() == i + 3 || w.size() == i + 4) && (w[i + 1] == "0" || w[i + 1] == "1") &&
                        area(w[i + 2], to) && (w.size() == i + 3 || area(w[i + 3], crop));
        if (!ok) {
            error = "Use: place [canvas2] N|screen 0|1 X,Y,W,H [CROP_X,CROP_Y,CROP_W,CROP_H] (fractions)";
            return std::nullopt;
        }
        r.action = Action::place;
        r.layer = *layer;
        r.visible = w[i + 1] == "1";
        r.area = to;
        if (w.size() == i + 4) r.crop = crop;
    } else if (line.rfind("style ", 0) == 0) {
        const auto space = line.find(' ', 6);
        const auto layer = space == std::string_view::npos ? std::nullopt : layer_index(line.substr(6, space - 6));
        std::string why;
        if (!layer || *layer == kScreen || !core::parse_style(rest(space + 1), r.style, why)) {
            error = "Use: style N SHAPE[:ROUNDNESS][,border=PIXELS[:RRGGBB]][,shadow]" + (why.empty() ? std::string() : " (" + why + ")");
            return std::nullopt;
        }
        r.action = Action::style;
        r.layer = *layer;
    } else if (line.rfind("level ", 0) == 0) {
        const auto args = std::string(rest(6));
        const auto first = args.find(' ');
        const auto second = first == std::string::npos ? std::string::npos : args.find(' ', first + 1);
        const auto input = first == std::string::npos ? std::nullopt : audio_input(std::string_view(args).substr(0, first));
        double db = 0;
        const auto muted = second == std::string::npos ? std::string() : args.substr(second + 1);
        if (!input || !number(std::string_view(args).substr(first + 1, second - first - 1), -60, 24, db) ||
            (muted != "0" && muted != "1")) {
            error = "Use: level system|mic DB 0|1 (-60 to 24 dB, then muted)";
            return std::nullopt;
        }
        r.action = Action::audio;
        r.input = *input;
        r.gain_db = db;
        r.muted = muted == "1";
    } else if ((line.rfind("mute ", 0) == 0 && audio_input(rest(5))) || (line.rfind("unmute ", 0) == 0 && audio_input(rest(7)))) {
        const bool mute = line[0] == 'm';
        r.action = Action::audio;
        r.input = *audio_input(rest(mute ? 5 : 7));
        r.muted = mute;
    } else if (line.rfind("volume ", 0) == 0) {
        const auto space = line.find(' ', 7);
        const auto input = space == std::string_view::npos ? std::nullopt : audio_input(line.substr(7, space - 7));
        double db = 0;
        if (!input || !number(rest(space + 1), -60, 24, db)) { error = "Use: volume system|mic DB (-60 to 24)"; return std::nullopt; }
        r.action = Action::audio;
        r.input = *input;
        r.gain_db = db;
    } else {
        error = "unknown command: " + std::string(line);
        return std::nullopt;
    }
    return r;
}

Controls::Controls(Recorder& recorder, const Options& options)
    : recorder_(recorder), options_(options), screen_visible_(options.screen_visible),
      gains_{options.system_gain_db, options.mic_gain_db}, mutes_{options.system_muted, options.mic_muted},
      has_input_{options.system_audio || options.test_tone, options.microphone || options.test_mic} {
    for (const auto& o : options.overlays) visible_.push_back(o.visible);
}

std::string Controls::layer_name(std::size_t n) const {
    if (n == kScreen) return "the screen";
    static const char* const kinds[] = {"image ", "camera ", "display ", "window "};
    return kinds[static_cast<int>(options_.overlays[n].kind)] + std::to_string(n + 1);
}

std::string Controls::muted() const {
    const bool system = has_input_[0] && mutes_[0], mic = has_input_[1] && mutes_[1];
    return system && mic ? "system audio and mic" : system ? "system audio" : mic ? "mic" : "";
}

Result Controls::apply(const Request& request, double recorded, std::string& message) {
    std::ostringstream out;
    const auto at = " at " + clock_text(recorded);
    switch (request.action) {
    case Action::show:
    case Action::hide:
    case Action::flip:
    case Action::move:
    case Action::place: {
        const std::size_t n = request.layer;
        if (n != kScreen && n >= visible_.size())
            return {"No overlay " + std::to_string(n + 1) + "; add one with --camera, --image, --add-display or --add-window"};
        if (request.action == Action::place && (request.canvas != kMainCanvas || request.crop)) {
            // A layout replaces the source's crop too; without one it shows all of it.
            const Layout layout{request.visible, *request.area, request.crop.value_or(Area{})};
            if (auto changed = recorder_.set_layout(request.canvas, n == kScreen ? kScreenLayer : n, layout); !changed) return changed;
            if (request.canvas == kMainCanvas) {
                if (n == kScreen) screen_visible_ = request.visible;
                else visible_[n] = request.visible;
            }
            return {};
        }
        const bool was = n == kScreen ? screen_visible_ : static_cast<bool>(visible_[n]);
        const bool show = request.action == Action::place  ? request.visible
                          : request.action == Action::move ? was
                                                           : request.action == Action::show || (request.action == Action::flip && !was);
        const auto changed = n == kScreen ? recorder_.set_screen(show, request.area) : recorder_.set_overlay(n, show, request.area);
        if (!changed) return changed;
        if (n == kScreen) screen_visible_ = show;
        else visible_[n] = show;
        if (request.action == Action::place) return {};  // Scenes place many sources at once; not logged.
        if (request.action == Action::move) {
            const auto& a = *request.area;
            out << "Moved " << layer_name(n) << " to " << a.x << ',' << a.y << ',' << a.width << ',' << a.height;
        } else {
            out << (show ? "Showing " : "Hiding ") << layer_name(n);
        }
        out << at;
        break;
    }
    case Action::audio: {
        const auto i = request.input == AudioInput::system ? 0 : 1;
        const char* name = i == 0 ? "system audio" : "microphone";
        const bool mute = request.muted.value_or(request.gain_db ? mutes_[i] : !mutes_[i]);
        const double gain = request.gain_db.value_or(gains_[i]);
        if (auto changed = recorder_.set_volume(request.input, gain, mute); !changed) return changed;
        if (request.gain_db) out << "Set " << name << " to " << std::fixed << std::setprecision(1) << gain << " dB";
        else out << (mute ? "Muted " : "Unmuted ") << name;
        out << at;
        gains_[i] = gain;
        mutes_[i] = mute;
        break;
    }
    case Action::style: {
        const std::size_t n = request.layer;
        if (n >= visible_.size()) return {"No overlay " + std::to_string(n + 1)};
        if (auto changed = recorder_.set_style(n, request.style); !changed) return changed;
        out << "Styled " << layer_name(n) << ' ' << core::style_text(request.style) << at;
        break;
    }
    case Action::save_replay: {
        if (!options_.replay_seconds) return {"No replay buffer; record with --replay SECONDS"};
        const auto path = replay_path(options_.output, replays_);
        double seconds = 0;
        if (auto saved = recorder_.save_replay(path, &seconds); !saved) return {"Replay not saved: " + saved.error};
        out << "Saved replay (" << std::fixed << std::setprecision(1) << seconds << " s) to " << path;
        break;
    }
    case Action::pause:
    case Action::resume:
    case Action::toggle: {
        const bool pause = request.action == Action::pause || (request.action == Action::toggle && !recorder_.stats().paused);
        if (auto changed = pause ? recorder_.pause() : recorder_.resume(); !changed) return changed;
        out << (pause ? "Paused" : "Resumed") << at;
        break;
    }
    case Action::stop:
        return {"stop ends the recording"};
    }
    message = out.str();
    return {};
}

std::string clock_text(double seconds) {
    const auto tenths = static_cast<long long>(seconds * 10);
    char text[32];
    std::snprintf(text, sizeof text, "%02lld:%02lld.%lld", tenths / 600, (tenths / 10) % 60, tenths % 10);
    return text;
}

std::string redact(const std::string& url) {
    const auto last = url.rfind('/');
    return last == std::string::npos ? url : url.substr(0, last + 1) + "****";
}

std::string summary(const Options& options, const Stats& stats, double wall_seconds) {
    std::ostringstream out;
    out << "Captured " << stats.captured << ", written " << stats.written << ", dropped " << stats.dropped << '\n';
    if (options.system_audio || options.test_tone)
        out << "System audio buffers written " << stats.audio_written << ", dropped " << stats.audio_dropped << " (offset "
            << std::fixed << std::setprecision(1) << stats.system_audio_offset_ms << " ms)\n";
    if (options.microphone || options.test_mic)
        out << "Microphone buffers written " << stats.mic_written << ", dropped " << stats.mic_dropped << '\n';
    if (stats.mix_written) out << "Mix buffers written " << stats.mix_written << '\n';
    if (std::any_of(options.overlays.begin(), options.overlays.end(), [](const Overlay& o) { return o.kind == Overlay::Kind::camera; }))
        out << "Camera frames " << stats.camera_frames << '\n';
    if (stats.paused_seconds > 0)
        out << "Paused " << std::fixed << std::setprecision(1) << stats.paused_seconds << " s (not in the movie)\n";
    std::vector<std::string> urls = options.stream_urls;
    urls.insert(urls.end(), options.canvas2.stream_urls.begin(), options.canvas2.stream_urls.end());
    if (options.canvas2.width)
        out << "Second canvas " << options.canvas2.width << 'x' << options.canvas2.height << ": " << stats.canvas2_frames
            << " frames, skipped " << stats.canvas2_dropped
            << (options.canvas2.output.empty() ? std::string() : ", saved " + options.canvas2.output) << '\n';
    if (!urls.empty())
        out << "Streamed " << stats.stream_frames << " frames (" << std::fixed << std::setprecision(1)
            << static_cast<double>(stats.stream_bytes) / 1e6 << " MB, "
            << static_cast<int>(static_cast<double>(stats.stream_bytes) * 8 / 1000 / std::max(1.0, wall_seconds))
            << " kb/s), skipped " << stats.stream_dropped
            << (stats.stream_reconnects ? ", reconnected " + std::to_string(stats.stream_reconnects) : std::string()) << '\n';
    for (std::uint32_t i = 0; stats.stream_count > 1 && i < stats.stream_count && i < urls.size(); ++i) {
        const auto& s = stats.streams[i];
        out << "  " << redact(urls[i]) << ": " << s.frames << " frames, " << std::fixed << std::setprecision(1)
            << static_cast<double>(s.bytes) / 1e6 << " MB, skipped " << s.dropped
            << (s.reconnects ? ", reconnected " + std::to_string(s.reconnects) : std::string()) << '\n';
    }
    return out.str();
}

} // namespace rec::cli
