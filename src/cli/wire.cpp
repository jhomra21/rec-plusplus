#include "cli/wire.hpp"
#include "core/shape.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>

namespace rec::cli {
namespace {

std::string number(double value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.17g", value);
    return text;
}

std::string area_text(const Area& a) {
    return number(a.x) + ',' + number(a.y) + ',' + number(a.width) + ',' + number(a.height);
}

std::string region_text(const Region& r) {
    return std::to_string(r.x) + ',' + std::to_string(r.y) + ',' + std::to_string(r.width) + ',' + std::to_string(r.height);
}

template <typename T>
bool integer(std::string_view text, T& out) {
    const auto end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc() && ptr == end;
}

bool real(std::string_view text, double& out) {
    if (text.empty()) return false;
    const std::string copy(text);
    char* end = nullptr;
    out = std::strtod(copy.c_str(), &end);
    return end == copy.c_str() + copy.size();
}

bool flag(std::string_view text, bool& out) {
    if (text != "0" && text != "1") return false;
    out = text == "1";
    return true;
}

// Four comma-separated parts.
std::vector<std::string_view> parts(std::string_view text) {
    std::vector<std::string_view> out;
    while (true) {
        const auto comma = text.find(',');
        out.push_back(text.substr(0, comma));
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    return out;
}

bool area(std::string_view text, Area& out) {
    const auto p = parts(text);
    return p.size() == 4 && real(p[0], out.x) && real(p[1], out.y) && real(p[2], out.width) && real(p[3], out.height);
}

// visible/area/crop
std::string layout_text(const Layout& l) { return std::string(l.visible ? "1" : "0") + '/' + area_text(l.area) + '/' + area_text(l.crop); }

bool layout(std::string_view text, Layout& out) {
    const auto first = text.find('/');
    const auto second = first == std::string_view::npos ? first : text.find('/', first + 1);
    return second != std::string_view::npos && flag(text.substr(0, first), out.visible) &&
           area(text.substr(first + 1, second - first - 1), out.area) && area(text.substr(second + 1), out.crop);
}

bool region(std::string_view text, Region& out) {
    const auto p = parts(text);
    return p.size() == 4 && integer(p[0], out.x) && integer(p[1], out.y) && integer(p[2], out.width) && integer(p[3], out.height);
}

} // namespace

std::string one_line(std::string text) {
    for (char& c : text)
        if (c == '\n' || c == '\r') c = ' ';
    return text;
}

std::vector<std::string> encode_options(const Options& o) {
    std::vector<std::string> w;
    const auto add = [&](const char* key, const std::string& value) { w.push_back(std::string(key) + '=' + value); };
    const auto yes = [](bool b) { return std::string(b ? "1" : "0"); };
    add("output", o.output);
    add("display", std::to_string(o.display));
    add("window", std::to_string(o.window));
    if (o.region) add("region", region_text(*o.region));
    add("width", std::to_string(o.width));
    add("height", std::to_string(o.height));
    add("fps", std::to_string(o.fps));
    add("bitrate", std::to_string(o.bitrate_mbps));
    add("codec", std::to_string(static_cast<int>(o.codec)));
    add("scaler", std::to_string(static_cast<int>(o.scaler)));
    add("cursor", yes(o.cursor));
    add("clicks", yes(o.clicks));
    add("test_pattern", yes(o.test_pattern));
    add("test_tone", yes(o.test_tone));
    add("test_mic", yes(o.test_mic));
    add("system_audio", yes(o.system_audio));
    add("microphone", yes(o.microphone));
    add("microphone_device", o.microphone_device);
    add("audio_tracks", std::to_string(static_cast<int>(o.audio_tracks)));
    add("system_gain", number(o.system_gain_db));
    add("mic_gain", number(o.mic_gain_db));
    add("system_muted", yes(o.system_muted));
    add("mic_muted", yes(o.mic_muted));
    if (o.system_audio_offset_ms) add("system_offset", number(*o.system_audio_offset_ms));
    add("mic_offset", number(o.mic_offset_ms));
    add("duration", number(o.duration_seconds));
    for (const auto& url : o.stream_urls) add("stream", url);
    add("stream_kbps", std::to_string(o.stream_kbps));
    add("replay", std::to_string(o.replay_seconds));
    add("screen_area", area_text(o.screen_area));
    add("screen_visible", yes(o.screen_visible));
    add("screen_crop", area_text(o.screen_crop));
    add("preview_width", std::to_string(o.preview_width));
    add("preview_only", yes(o.preview_only));
    for (const auto& l : o.overlays) {
        add("overlay", std::to_string(static_cast<int>(l.kind)));
        add("overlay.source", l.source);
        add("overlay.display", std::to_string(l.display));
        add("overlay.window", std::to_string(l.window));
        if (l.region) add("overlay.region", region_text(*l.region));
        add("overlay.area", area_text(l.area));
        add("overlay.visible", yes(l.visible));
        add("overlay.crop", area_text(l.crop));
        if (!core::plain(l.style)) add("overlay.style", core::style_text(l.style));
    }
    if (const auto& c = o.canvas2; c.width) {
        add("canvas2", std::to_string(c.width) + 'x' + std::to_string(c.height));
        add("canvas2.screen", layout_text(c.screen));
        for (const auto& l : c.overlays) add("canvas2.overlay", layout_text(l));
        for (const auto& url : c.stream_urls) add("canvas2.stream", url);
        if (!c.output.empty()) add("canvas2.output", c.output);
    }
    return w;
}

bool decode_options(const std::vector<std::string>& words, Options& o, std::string& error) {
    o = Options{};
    for (const auto& word : words) {
        const auto eq = word.find('=');
        const std::string_view key = std::string_view(word).substr(0, eq);
        const std::string_view v = eq == std::string::npos ? std::string_view() : std::string_view(word).substr(eq + 1);
        int e = 0;
        double d = 0;
        Region r;
        bool ok = eq != std::string::npos;
        Overlay* layer = o.overlays.empty() ? nullptr : &o.overlays.back();
        if (!ok) {
        } else if (key == "output") o.output = v;
        else if (key == "display") ok = integer(v, o.display);
        else if (key == "window") ok = integer(v, o.window);
        else if (key == "region") { ok = region(v, r); o.region = r; }
        else if (key == "width") ok = integer(v, o.width);
        else if (key == "height") ok = integer(v, o.height);
        else if (key == "fps") ok = integer(v, o.fps) && o.fps >= 1 && o.fps <= 240;
        else if (key == "bitrate") ok = integer(v, o.bitrate_mbps);
        else if (key == "codec") { ok = integer(v, e) && e >= 0 && e <= 3; o.codec = static_cast<Codec>(e); }
        else if (key == "scaler") { ok = integer(v, e) && e >= 0 && e <= 1; o.scaler = static_cast<Scaler>(e); }
        else if (key == "cursor") ok = flag(v, o.cursor);
        else if (key == "clicks") ok = flag(v, o.clicks);
        else if (key == "test_pattern") ok = flag(v, o.test_pattern);
        else if (key == "test_tone") ok = flag(v, o.test_tone);
        else if (key == "test_mic") ok = flag(v, o.test_mic);
        else if (key == "system_audio") ok = flag(v, o.system_audio);
        else if (key == "microphone") ok = flag(v, o.microphone);
        else if (key == "microphone_device") o.microphone_device = v;
        else if (key == "audio_tracks") { ok = integer(v, e) && e >= 0 && e <= 2; o.audio_tracks = static_cast<AudioTracks>(e); }
        else if (key == "system_gain") ok = real(v, o.system_gain_db);
        else if (key == "mic_gain") ok = real(v, o.mic_gain_db);
        else if (key == "system_muted") ok = flag(v, o.system_muted);
        else if (key == "mic_muted") ok = flag(v, o.mic_muted);
        else if (key == "system_offset") { ok = real(v, d); o.system_audio_offset_ms = d; }
        else if (key == "mic_offset") ok = real(v, o.mic_offset_ms);
        else if (key == "duration") ok = real(v, o.duration_seconds);
        else if (key == "stream") { ok = o.stream_urls.size() < kMaxStreams; o.stream_urls.emplace_back(v); }
        else if (key == "stream_kbps") ok = integer(v, o.stream_kbps);
        else if (key == "replay") ok = integer(v, o.replay_seconds);
        else if (key == "screen_area") ok = area(v, o.screen_area);
        else if (key == "screen_visible") ok = flag(v, o.screen_visible);
        else if (key == "screen_crop") ok = area(v, o.screen_crop);
        else if (key == "canvas2") {
            const auto x = v.find('x');
            ok = x != std::string_view::npos && integer(v.substr(0, x), o.canvas2.width) && integer(v.substr(x + 1), o.canvas2.height);
        } else if (key == "canvas2.screen") ok = layout(v, o.canvas2.screen);
        else if (key == "canvas2.overlay") {
            Layout l;
            ok = layout(v, l) && o.canvas2.overlays.size() < 15;
            o.canvas2.overlays.push_back(l);
        } else if (key == "canvas2.stream") { ok = o.canvas2.stream_urls.size() < kMaxStreams; o.canvas2.stream_urls.emplace_back(v); }
        else if (key == "canvas2.output") o.canvas2.output = v;
        else if (key == "preview_width") ok = integer(v, o.preview_width) && o.preview_width <= 3840;
        else if (key == "preview_only") ok = flag(v, o.preview_only);
        else if (key == "overlay") {
            ok = integer(v, e) && e >= 0 && e <= 3 && o.overlays.size() < 15;
            if (ok) o.overlays.push_back(Overlay{static_cast<Overlay::Kind>(e)});
        } else if (key.rfind("overlay.", 0) == 0 && layer) {
            const auto field = key.substr(8);
            if (field == "source") layer->source = v;
            else if (field == "display") ok = integer(v, layer->display);
            else if (field == "window") ok = integer(v, layer->window);
            else if (field == "region") { ok = region(v, r); layer->region = r; }
            else if (field == "area") ok = area(v, layer->area);
            else if (field == "visible") ok = flag(v, layer->visible);
            else if (field == "crop") ok = area(v, layer->crop);
            else if (field == "style") { std::string why; ok = core::parse_style(v, layer->style, why); }
            else ok = false;
        } else ok = false;
        if (!ok) {
            error = "bad recording option: " + std::string(key);
            return false;
        }
    }
    return true;
}

std::string encode_stats(const Stats& s) {
    std::string out;
    const auto add = [&](const char* key, const std::string& value) { out += std::string(key) + '=' + value + '\n'; };
    add("captured", std::to_string(s.captured));
    add("written", std::to_string(s.written));
    add("dropped", std::to_string(s.dropped));
    add("audio_written", std::to_string(s.audio_written));
    add("audio_dropped", std::to_string(s.audio_dropped));
    add("mic_written", std::to_string(s.mic_written));
    add("mic_dropped", std::to_string(s.mic_dropped));
    add("mix_written", std::to_string(s.mix_written));
    add("system_peak", number(s.system_peak));
    add("mic_peak", number(s.mic_peak));
    add("system_offset", number(s.system_audio_offset_ms));
    add("paused", s.paused ? "1" : "0");
    add("paused_seconds", number(s.paused_seconds));
    add("window_closed", s.window_closed ? "1" : "0");
    add("streaming", s.streaming ? "1" : "0");
    add("stream_reconnecting", s.stream_reconnecting ? "1" : "0");
    add("stream_reconnects", std::to_string(s.stream_reconnects));
    add("stream_hardware", s.stream_hardware ? "1" : "0");
    add("stream_frames", std::to_string(s.stream_frames));
    add("stream_dropped", std::to_string(s.stream_dropped));
    add("stream_bytes", std::to_string(s.stream_bytes));
    add("stream_queued_ms", std::to_string(s.stream_queued_ms));
    add("stream_count", std::to_string(s.stream_count));
    for (std::uint32_t i = 0; i < s.stream_count && i < kMaxStreams; ++i) {
        const auto& t = s.streams[i];
        add(("server" + std::to_string(i)).c_str(),
            std::string(t.live ? "1" : "0") + ',' + (t.reconnecting ? "1" : "0") + ',' + std::to_string(t.reconnects) + ',' +
                std::to_string(t.frames) + ',' + std::to_string(t.dropped) + ',' + std::to_string(t.bytes) + ',' +
                std::to_string(t.queued_ms));
    }
    add("replay_seconds", number(s.replay_seconds));
    add("camera_frames", std::to_string(s.camera_frames));
    add("canvas2_frames", std::to_string(s.canvas2_frames));
    add("canvas2_dropped", std::to_string(s.canvas2_dropped));
    add("system_audio_lost", s.system_audio_lost ? "1" : "0");
    add("microphone_lost", s.microphone_lost ? "1" : "0");
    add("camera_lost", s.camera_lost ? "1" : "0");
    add("audio_reconnects", std::to_string(s.audio_reconnects));
    return out;
}

bool decode_stat(std::string_view key, std::string_view v, Stats& s) {
    double d = 0;
    if (key == "captured") return integer(v, s.captured);
    if (key == "written") return integer(v, s.written);
    if (key == "dropped") return integer(v, s.dropped);
    if (key == "audio_written") return integer(v, s.audio_written);
    if (key == "audio_dropped") return integer(v, s.audio_dropped);
    if (key == "mic_written") return integer(v, s.mic_written);
    if (key == "mic_dropped") return integer(v, s.mic_dropped);
    if (key == "mix_written") return integer(v, s.mix_written);
    if (key == "system_peak") { const bool ok = real(v, d); s.system_peak = static_cast<float>(d); return ok; }
    if (key == "mic_peak") { const bool ok = real(v, d); s.mic_peak = static_cast<float>(d); return ok; }
    if (key == "system_offset") return real(v, s.system_audio_offset_ms);
    if (key == "paused") return flag(v, s.paused);
    if (key == "paused_seconds") return real(v, s.paused_seconds);
    if (key == "window_closed") return flag(v, s.window_closed);
    if (key == "streaming") return flag(v, s.streaming);
    if (key == "stream_reconnecting") return flag(v, s.stream_reconnecting);
    if (key == "stream_reconnects") return integer(v, s.stream_reconnects);
    if (key == "stream_hardware") return flag(v, s.stream_hardware);
    if (key == "stream_frames") return integer(v, s.stream_frames);
    if (key == "stream_dropped") return integer(v, s.stream_dropped);
    if (key == "stream_bytes") return integer(v, s.stream_bytes);
    if (key == "stream_queued_ms") return integer(v, s.stream_queued_ms);
    if (key == "stream_count") return integer(v, s.stream_count) && s.stream_count <= kMaxStreams;
    if (key.rfind("server", 0) == 0) {
        std::uint32_t i = 0;
        const auto p = parts(v);
        if (!integer(key.substr(6), i) || i >= kMaxStreams || p.size() != 7) return false;
        auto& t = s.streams[i];
        return flag(p[0], t.live) && flag(p[1], t.reconnecting) && integer(p[2], t.reconnects) && integer(p[3], t.frames) &&
               integer(p[4], t.dropped) && integer(p[5], t.bytes) && integer(p[6], t.queued_ms);
    }
    if (key == "replay_seconds") return real(v, s.replay_seconds);
    if (key == "camera_frames") return integer(v, s.camera_frames);
    if (key == "canvas2_frames") return integer(v, s.canvas2_frames);
    if (key == "canvas2_dropped") return integer(v, s.canvas2_dropped);
    if (key == "system_audio_lost") return flag(v, s.system_audio_lost);
    if (key == "microphone_lost") return flag(v, s.microphone_lost);
    if (key == "camera_lost") return flag(v, s.camera_lost);
    if (key == "audio_reconnects") return integer(v, s.audio_reconnects);
    return false;
}

} // namespace rec::cli
