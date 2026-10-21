#include "app/session.hpp"

#include "cli/engine.hpp"

#include "core/overlay.hpp"
#include "core/phone.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
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

namespace {
// "1,x,y,width,height": shown, and where.
std::string item_text(const SceneItem& item) {
    std::ostringstream out;
    out << (item.visible ? 1 : 0) << ',' << item.area.x << ',' << item.area.y << ',' << item.area.width << ',' << item.area.height;
    return out.str();
}

std::optional<SceneItem> parse_item(const std::string& text) {
    int visible = 0;
    SceneItem item;
    char tail = 0;
    if (std::sscanf(text.c_str(), "%d,%lf,%lf,%lf,%lf%c", &visible, &item.area.x, &item.area.y, &item.area.width,
                    &item.area.height, &tail) != 5 ||
        !core::valid(item.area))
        return std::nullopt;
    item.visible = visible != 0;
    return item;
}

std::optional<Area> parse_area(const std::string& text) {
    Area a;
    char tail = 0;
    if (std::sscanf(text.c_str(), "%lf,%lf,%lf,%lf%c", &a.x, &a.y, &a.width, &a.height, &tail) != 4 || !core::valid(a))
        return std::nullopt;
    return a;
}

// "prefix.N.rest": N and rest.
bool indexed(const std::string& key, const char* prefix, std::size_t& index, std::string& rest) {
    const std::size_t length = std::strlen(prefix);
    if (key.compare(0, length, prefix) != 0) return false;
    std::size_t end = length;
    while (end < key.size() && std::isdigit(static_cast<unsigned char>(key[end]))) ++end;
    if (end == length || end - length > 3 || end >= key.size() || key[end] != '.') return false;
    index = static_cast<std::size_t>(std::stoul(key.substr(length, end - length)));
    rest = key.substr(end + 1);
    return true;
}

struct LoadedScene {
    std::optional<std::string> name, vertical;
    std::optional<SceneItem> screen, camera, image;
    std::map<std::size_t, SceneItem> layers;
};
} // namespace
Settings load_settings(const std::string& path) {
    Settings s;
    std::map<std::size_t, Layer> layers;
    std::map<std::size_t, LoadedScene> scenes;
    // Before scenes: one camera placement and a scene by name.
    std::string old_corner, old_size, old_scene;
    std::optional<Area> old_custom;
    std::map<std::size_t, Destination> destinations;
    // Before the destination list: a first and an optional second destination.
    std::map<std::string, std::string> old_stream;
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
        } else if (key == "codec" && value == kCodecs[choice_index(kCodecs, value)].value) s.codec = value;
        else if (key == "cursor") s.cursor = yes;
        else if (key == "clicks") s.clicks = yes;
        else if (key == "system_audio") s.system_audio = yes;
        else if (key == "microphone") s.microphone = yes;
        else if (key == "microphone_device" && !value.empty()) s.microphone_device = value;
        else if (key == "system_volume") s.system_volume = volume(value);
        else if (key == "mic_volume") s.mic_volume = volume(value);
        else if (key == "system_muted") s.system_muted = yes;
        else if (key == "mic_muted") s.mic_muted = yes;
        else if (key == "stream") s.stream = yes;
        else if (key == "destination" || key == "server" || key == "destination2" || key == "server2" || key == "x_server" ||
                 key == "tiktok_server" || key == "destination_vertical" || key == "destination2_vertical")
            old_stream[key] = value;
        else if (key == "vertical_movie") s.vertical_movie = yes;
        else if (key == "stream_kbps") s.stream_kbps = std::clamp(number(value, 6000), 500u, 50000u);
        else if (key == "replay") s.replay = yes;
        else if (key == "replay_seconds") {
            const auto seconds = number(value, 30);
            if (std::find(kReplayLengths.begin(), kReplayLengths.end(), seconds) != kReplayLengths.end()) s.replay_seconds = seconds;
        } else if (key == "camera") s.camera = yes;
        else if (key == "camera_device" && !value.empty()) s.camera_device = value;
        else if (key == "phone_token" && value.size() >= 16 && value.size() <= 64 &&
                 std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isalnum(c); }))
            s.phone_token = value;
        else if (key == "camera_style") {
            Style style;
            std::string error;
            if (core::parse_style(value, style, error) && core::check_style(style).empty()) s.camera_style = style;
        }
        else if (key == "screen_color" || key == "camera_color") {
            rec::ColorAdjust color;
            std::string error;
            if (core::parse_color(value, color, error)) {
                if (key == "screen_color") s.screen_color = color;
                else s.camera_color = color;
            }
        }
        else if (key == "camera_corner" && known(kCorners, value)) old_corner = value;
        else if (key == "camera_size" && known(kCameraSizes, value)) old_size = value;
        else if (key == "camera_custom") old_custom = parse_area(value);
        else if (key == "scene") {
            if (!value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); }))
                s.scene = number(value, 0);
            else old_scene = value;
        }
        else if (key == "image") s.image = yes;
        else if (key == "image_path") s.image_path = value;
        else if (key == "preview") s.preview = yes;
        else if (key == "keep_recording" && (value == "ask" || value == "yes" || value == "no")) s.keep_recording = value;
        else if (key == "last_recording") s.last_recording = value;
        else {
            std::size_t n = 0;
            std::string field;
            if (indexed(key, "destination.", n, field)) {
                Destination& d = destinations[n];
                if (field == "on") d.on = yes;
                else if (field == "service" && is_choice(kServices, value)) d.service = value;
                else if (field == "server") d.server = value;
                else if (field == "vertical") d.vertical = yes;
            } else if (indexed(key, "layer.", n, field)) {
                Layer& layer = layers[n];
                if (field == "kind" && (value == "display" || value == "window")) layer.kind = value;
                else if (field == "display") layer.display = std::max(1u, number(value, 1));
                else if (field == "region" && (value.empty() || parse_region(value))) layer.region = value;
                else if (field == "app") layer.window_app = value;
                else if (field == "title") layer.window_title = value;
            } else if (indexed(key, "scene.", n, field)) {
                LoadedScene& scene = scenes[n];
                if (field == "name") scene.name = value;
            else if (field == "vertical" && is_choice(kVerticalLayouts, value)) scene.vertical = value;
                else if (field == "screen") scene.screen = parse_item(value);
                else if (field == "camera") scene.camera = parse_item(value);
                else if (field == "image") scene.image = parse_item(value);
                else if (field.rfind("layer.", 0) == 0) {
                    const auto item = parse_item(value);
                    const auto digits = field.substr(6);
                    if (item && !digits.empty() && digits.size() <= 3 &&
                        std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); }))
                        scene.layers[std::stoul(digits)] = *item;
                }
            }
        }
    }
    // Indices count from 0; a gap ends the list.
    for (std::size_t i = 0; layers.count(i) && s.layers.size() < kMaxLayers; ++i) s.layers.push_back(layers[i]);
    if (destinations.count(0)) {
        s.destinations.clear();
        for (std::size_t i = 0; destinations.count(i) && s.destinations.size() < kMaxDestinations; ++i)
            s.destinations.push_back(destinations[i]);
    } else if (!old_stream.empty()) {
        // X and TikTok servers were shared by both destinations; custom ones were each their own.
        const auto old = [&](const char* key) { return old_stream.count(key) ? old_stream[key] : std::string(); };
        const auto typed = [&](const std::string& service, const char* custom) {
            return service == "x" ? old("x_server") : service == "tiktok" ? old("tiktok_server") : service == "custom" ? old(custom) : std::string();
        };
        Destination first;
        if (is_choice(kServices, old("destination"))) first.service = old("destination");
        first.server = typed(first.service, "server");
        first.vertical = old("destination_vertical") == "1";
        s.destinations = {first};
        if (const auto second = old("destination2"); is_choice(kServices, second))
            s.destinations.push_back({true, second, typed(second, "server2"), old("destination2_vertical") == "1"});
    }
    const auto hidden = SceneItem{false, Area{}};
    if (!scenes.empty()) {
        std::vector<Scene> loaded;
        for (std::size_t i = 0; scenes.count(i); ++i) {
            const LoadedScene& from = scenes[i];
            Scene scene;
            scene.name = from.name.value_or("Scene " + std::to_string(i + 1));
            scene.screen = from.screen.value_or(SceneItem{});
            scene.camera = from.camera.value_or(SceneItem{true, corner_area("bottom-right", "medium")});
            scene.image = from.image.value_or(SceneItem{});
            scene.vertical = from.vertical.value_or("fit");
            for (std::size_t j = 0; j < s.layers.size(); ++j)
                scene.layers.push_back(from.layers.count(j) ? from.layers.at(j) : hidden);
            loaded.push_back(std::move(scene));
        }
        if (!loaded.empty()) s.scenes = std::move(loaded);
    } else {
        // Settings from before scenes: their camera placement in the scenes that show a corner.
        Area corner = corner_area(old_corner.empty() || old_corner == "custom" ? "bottom-right" : old_corner,
                                  old_size.empty() ? "medium" : old_size);
        if (old_corner == "custom" && old_custom) corner = *old_custom;
        s.scenes[0].camera.area = s.scenes[1].camera.area = corner;
        if (old_scene == "screen") s.scene = 1;
        else if (old_scene == "camera") s.scene = 2;
    }
    for (auto& scene : s.scenes) scene.layers.resize(s.layers.size(), hidden);
    if (s.scene >= s.scenes.size()) s.scene = 0;
    if (s.phone_token.empty()) s.phone_token = core::PhoneCamera::new_token();
    // A phone camera from another token is no longer reachable; the current one replaces it.
    if (is_phone_camera(s.camera_device)) s.camera_device = phone_camera_id(s);
    return s;
}

std::string phone_camera_id(const Settings& settings) { return "phone:" + settings.phone_token; }
bool is_phone_camera(const std::string& device) { return device.rfind("phone:", 0) == 0; }

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
            << "\ncursor=" << s.cursor << "\nclicks=" << s.clicks << "\nsystem_audio=" << s.system_audio
            << "\nmicrophone=" << s.microphone << "\nmicrophone_device=" << s.microphone_device
            << "\nsystem_volume=" << s.system_volume << "\nmic_volume=" << s.mic_volume
            << "\nsystem_muted=" << s.system_muted << "\nmic_muted=" << s.mic_muted
            << "\nstream=" << s.stream << "\nvertical_movie=" << s.vertical_movie
            << "\nstream_kbps=" << s.stream_kbps << "\nreplay=" << s.replay << "\nreplay_seconds=" << s.replay_seconds
            << "\ncamera=" << s.camera << "\ncamera_device=" << one_line(s.camera_device)
            << "\nphone_token=" << s.phone_token
            << "\ncamera_style=" << core::style_text(s.camera_style)
            << "\nscreen_color=" << core::color_text(s.screen_color)
            << "\ncamera_color=" << core::color_text(s.camera_color)
            << "\nimage=" << s.image << "\nimage_path=" << one_line(s.image_path) << "\npreview=" << s.preview
            << "\nkeep_recording=" << s.keep_recording
            << "\nlast_recording=" << s.last_recording << '\n';
        for (std::size_t i = 0; i < s.destinations.size(); ++i) {
            const Destination& d = s.destinations[i];
            const auto key = "destination." + std::to_string(i) + ".";
            out << key << "on=" << d.on << '\n' << key << "service=" << d.service << '\n'
                << key << "server=" << one_line(d.server) << '\n' << key << "vertical=" << d.vertical << '\n';
        }
        for (std::size_t i = 0; i < s.layers.size(); ++i) {
            const Layer& layer = s.layers[i];
            const auto key = "layer." + std::to_string(i) + ".";
            out << key << "kind=" << layer.kind << '\n' << key << "display=" << layer.display << '\n'
                << key << "region=" << layer.region << '\n' << key << "app=" << one_line(layer.window_app) << '\n'
                << key << "title=" << one_line(layer.window_title) << '\n';
        }
        for (std::size_t i = 0; i < s.scenes.size(); ++i) {
            const Scene& scene = s.scenes[i];
            const auto key = "scene." + std::to_string(i) + ".";
            out << key << "name=" << one_line(scene.name) << '\n' << key << "vertical=" << scene.vertical << '\n'
                << key << "screen=" << item_text(scene.screen) << '\n'
                << key << "camera=" << item_text(scene.camera) << '\n' << key << "image=" << item_text(scene.image) << '\n';
            for (std::size_t j = 0; j < scene.layers.size(); ++j)
                out << key << "layer." << j << '=' << item_text(scene.layers[j]) << '\n';
        }
        out << "scene=" << s.scene << '\n';
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

std::optional<Window> find_window(const std::string& app, const std::string& title) {
    if (app.empty() && title.empty()) return std::nullopt;
    std::string error;
    const auto windows = list_windows(error);
    for (const auto& w : windows)
        if (w.app == app && w.title == title) return w;
    if (!app.empty())
        for (const auto& w : windows)
            if (w.app == app) return w;
    return std::nullopt;
}

std::optional<Window> find_window(const Settings& s) { return find_window(s.window_app, s.window_title); }

bool uses_vertical(const Settings& s) {
    if (s.vertical_movie) return true;
    if (!s.stream) return false;
    for (const auto& d : s.destinations)
        if (d.on && d.vertical) return true;
    return false;
}

Layout vertical_layout(const Scene& scene, const SourceRef& source, double aspect, bool camera) {
    const SceneItem& item = scene_item(scene, source);
    if (!(aspect > 0)) aspect = 16.0 / 9;
    // Without a camera below it, the picture fills the frame instead of its top half.
    const std::string mode = scene.vertical == "split" && !(camera && scene.camera.visible) ? "fill" : scene.vertical;
    constexpr double canvas = static_cast<double>(kVerticalWidth) / kVerticalHeight;
    const auto full = [](const Area& a) { return a.x <= 0.001 && a.y <= 0.001 && a.width >= 0.999 && a.height >= 0.999; };
    // The part of the landscape picture shown (region) and where it goes (target).
    Area region, target;
    if (mode == "fill") {
        region.width = std::min(1.0, canvas / aspect);
    } else if (mode == "split") {
        target.height = 0.5;
        region.width = std::min(1.0, canvas * 2 / aspect);
    } else {
        target.height = std::min(1.0, canvas / aspect);
        target.y = (1 - target.height) / 2;
    }
    region.x = (1 - region.width) / 2;
    // A camera in a corner would be cropped away; it gets its own place below the picture.
    if (source.kind == SourceRef::Kind::camera && mode != "fit" && !full(item.area)) {
        if (mode == "split") return {item.visible, Area{0, 0.5, 1, 0.5}, Area{}};
        return {item.visible, Area{0.05, 0.7, 0.9, 0.27}, Area{}};
    }
    // The source's area cut to the region. The crop assumes the picture fills its area, which
    // holds for the screen and the camera; a letterboxed window cut by the region is cropped
    // by its area's fraction instead of its picture's.
    const Area& a = item.area;
    const double left = std::max(a.x, region.x), top = std::max(a.y, region.y);
    const double right = std::min(a.x + a.width, region.x + region.width), bottom = std::min(a.y + a.height, region.y + region.height);
    if (right - left < 0.001 || bottom - top < 0.001 || a.width <= 0 || a.height <= 0) return {false, Area{}, Area{}};
    const auto clamp01 = [](double v) { return std::clamp(v, 0.0, 1.0); };
    Area crop{clamp01((left - a.x) / a.width), clamp01((top - a.y) / a.height), 0, 0};
    crop.width = std::min(1 - crop.x, (right - left) / a.width);
    crop.height = std::min(1 - crop.y, (bottom - top) / a.height);
    Area area{clamp01(target.x + (left - region.x) / region.width * target.width),
              clamp01(target.y + (top - region.y) / region.height * target.height), 0, 0};
    area.width = std::min(1 - area.x, (right - left) / region.width * target.width);
    area.height = std::min(1 - area.y, (bottom - top) / region.height * target.height);
    return {item.visible, area, crop};
}
std::string server_url(const Destination& d) { return server_url(d.service, d.server); }

bool typed_server(const std::string& service) { return service == "custom" || service == "x" || service == "tiktok"; }

std::string key_name(const Settings& s, std::size_t index) {
    if (index >= s.destinations.size()) return {};
    const auto url = server_url(s.destinations[index]);
    if (url.empty()) return {};
    std::size_t same = 1;
    for (std::size_t i = 0; i < index; ++i)
        if (server_url(s.destinations[i]) == url) ++same;
    return same == 1 ? url : url + "#" + std::to_string(same);
}

bool uses_key_name(const Settings& s, const std::string& name) {
    for (std::size_t i = 0; i < s.destinations.size(); ++i)
        if (key_name(s, i) == name) return true;
    return false;
}

std::string destination_name(const Settings& s, std::size_t index) {
    if (index >= s.destinations.size()) return {};
    const auto& service = s.destinations[index].service;
    std::size_t same = 1;
    for (std::size_t i = 0; i < index; ++i)
        if (s.destinations[i].service == service) ++same;
    const std::string label = service == "custom" ? "Custom" : service == "x" ? "X" : kServices[choice_index(kServices, service)].label;
    return same == 1 ? label : label + " " + std::to_string(same);
}

std::string lost_sources_text(const Status& s) {
    std::vector<std::string> audio;
    if (s.system_audio_lost) audio.push_back("System audio");
    if (s.microphone_lost) audio.push_back("The microphone");
    std::string text;
    if (audio.size() == 2) text = "System audio and the microphone stopped; their tracks stay silent until they return.";
    else if (!audio.empty()) text = audio[0] + " stopped; its track stays silent until it returns.";
    if (s.camera_lost) text += std::string(text.empty() ? "" : " ") + "The camera stopped; it shows its last frame.";
    return text;
}

bool add_destination(Settings& s) {
    if (s.destinations.size() >= kMaxDestinations) return false;
    Destination d;
    for (const auto& choice : kServices) {
        const bool used = std::any_of(s.destinations.begin(), s.destinations.end(),
                                      [&](const Destination& o) { return o.service == choice.value; });
        if (!used) { d.service = choice.value; break; }
    }
    // TikTok is watched on phones.
    d.vertical = d.service == "tiktok";
    s.destinations.push_back(d);
    return true;
}

void remove_destination(Settings& s, std::size_t index) {
    if (s.destinations.size() > 1 && index < s.destinations.size())
        s.destinations.erase(s.destinations.begin() + static_cast<std::ptrdiff_t>(index));
}

const char* server_hint(const std::string& destination) {
    if (destination == "x") return "From X Media Studio, Producer: rtmps://…";
    if (destination == "tiktok") return "From TikTok LIVE Center; it changes each LIVE";
    if (destination == "custom") return "rtmp://host/app, rtmps://host:443/app or a WHIP https:// URL";
    return "";
}
const char* key_page(const std::string& destination) {
    if (destination == "twitch") return "https://dashboard.twitch.tv/settings/stream";
    if (destination == "youtube") return "https://studio.youtube.com/";
    if (destination == "x") return "https://studio.x.com/producer";
    if (destination == "tiktok") return "https://livecenter.tiktok.com/producer";
    return "";
}

Pasted read_pasted(const std::string& text) {
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    const auto first = std::find_if_not(text.begin(), text.end(), space);
    const auto last = std::find_if_not(text.rbegin(), text.rend(), space).base();
    if (first >= last) return {};
    const std::string token(first, last);
    if (token.size() > 1024 || std::any_of(token.begin(), token.end(), space)) return {};
    std::string lower = token;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.rfind("rtmp://", 0) == 0 || lower.rfind("rtmps://", 0) == 0) return {token, {}};
    if (token.size() >= 8 && token.find("://") == std::string::npos) return {{}, token};
    return {};
}

std::string server_url(const std::string& destination, const std::string& server) {
    if (destination == "twitch") return "rtmps://live.twitch.tv:443/app";
    if (destination == "youtube") return "rtmps://a.rtmps.youtube.com:443/live2";
    std::string url = server;
    while (!url.empty() && (url.back() == '/' || url.back() == ' ')) url.pop_back();
    return url;
}

std::string redact(const std::string& url) {
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)
        return url.substr(0, url.find_first_of("/#", url.find("://") + 3)) + "/****";
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

namespace {
// The camera's margin from the frame's edges: the same in pixels on both sides of a 16:9 frame.
constexpr double kMarginX = 0.02, kMarginY = 0.035;
// The smallest source a drag makes, as a fraction of the frame.
constexpr double kMinArea = 0.08;
// Within this of the frame's edges, its middle or a corner margin, an edge lands on it.
constexpr double kSnapX = 0.012, kSnapY = 0.02;

double snap_edge(double value, double margin, double within) {
    for (const double target : {0.0, margin, 0.5, 1 - margin, 1.0})
        if (std::abs(value - target) < within) return target;
    return value;
}

bool same(const Area& a, const Area& b) {
    constexpr double e = 1e-6;
    return std::abs(a.x - b.x) < e && std::abs(a.y - b.y) < e && std::abs(a.width - b.width) < e &&
           std::abs(a.height - b.height) < e;
}

bool full_frame(const Area& a) { return a.x <= 0.001 && a.y <= 0.001 && a.width >= 0.999 && a.height >= 0.999; }

// Whether a press at x,y lands on area: inside it or on a corner handle, or only on a corner
// handle for a source that fills the frame (so the ones below can still be grabbed).
bool hits(const Area& a, double x, double y, double handle_x, double handle_y, bool corners_only) {
    if (x < a.x - handle_x / 2 || x > a.x + a.width + handle_x / 2 || y < a.y - handle_y / 2 ||
        y > a.y + a.height + handle_y / 2)
        return false;
    if (!corners_only) return true;
    const bool near_x = x < a.x + handle_x || x > a.x + a.width - handle_x;
    const bool near_y = y < a.y + handle_y || y > a.y + a.height - handle_y;
    return near_x && near_y;
}
} // namespace

Area corner_area(const std::string& corner, const std::string& size_name) {
    const double size = size_name == "small" ? 0.2 : size_name == "large" ? 0.33 : 0.25;
    const bool left = corner.ends_with("left"), top = corner.starts_with("top");
    return {left ? kMarginX : 1 - kMarginX - size, top ? kMarginY : 1 - kMarginY - size, size, size};
}

std::string corner_of(const Area& area) {
    for (const auto& corner : kCorners)
        for (const auto& size : kCameraSizes)
            if (std::string(corner.value) != "custom" && same(area, corner_area(corner.value, size.value)))
                return corner.value;
    return "custom";
}

std::string size_of(const Area& area) {
    for (const auto& corner : kCorners)
        for (const auto& size : kCameraSizes)
            if (std::string(corner.value) != "custom" && same(area, corner_area(corner.value, size.value)))
                return size.value;
    return "";
}

std::vector<Scene> default_scenes() {
    const Area corner = corner_area("bottom-right", "medium");
    Scene both{"Screen and camera", {}, {true, corner}, {}, {}, "split"};
    Scene screen{"Screen only", {}, {false, corner}, {}, {}, "fit"};
    Scene camera{"Camera only", {}, {true, Area{}}, {}, {}, "fill"};
    return {both, screen, camera};
}

Scene& current_scene(Settings& s) {
    if (s.scenes.empty()) s.scenes = default_scenes();
    if (s.scene >= s.scenes.size()) s.scene = 0;
    return s.scenes[s.scene];
}

const Scene& current_scene(const Settings& s) {
    return s.scenes[std::min<std::size_t>(s.scene, s.scenes.size() - 1)];
}

bool add_scene(Settings& s) {
    // Settings keep scene numbers to three digits.
    if (s.scenes.size() >= 999) return false;
    Scene scene = current_scene(s);
    const auto taken = [&](const std::string& name) {
        return std::any_of(s.scenes.begin(), s.scenes.end(), [&](const Scene& other) { return other.name == name; });
    };
    std::size_t n = s.scenes.size() + 1;
    while (taken("Scene " + std::to_string(n))) ++n;
    scene.name = "Scene " + std::to_string(n);
    s.scenes.insert(s.scenes.begin() + s.scene + 1, std::move(scene));
    ++s.scene;
    return true;
}

void remove_scene(Settings& s) {
    current_scene(s);
    if (s.scenes.size() <= 1) return;
    s.scenes.erase(s.scenes.begin() + s.scene);
    s.scene = std::min<std::uint32_t>(s.scene, static_cast<std::uint32_t>(s.scenes.size() - 1));
}

bool add_layer(Settings& s, const Layer& layer) {
    if (s.layers.size() >= kMaxLayers) return false;
    current_scene(s);
    s.layers.push_back(layer);
    for (std::size_t i = 0; i < s.scenes.size(); ++i) {
        auto& items = s.scenes[i].layers;
        items.resize(s.layers.size() - 1, SceneItem{false, Area{}});
        items.push_back(SceneItem{i == s.scene, Area{}});
    }
    return true;
}

void remove_layer(Settings& s, std::size_t index) {
    if (index >= s.layers.size()) return;
    s.layers.erase(s.layers.begin() + static_cast<std::ptrdiff_t>(index));
    for (auto& scene : s.scenes)
        if (index < scene.layers.size()) scene.layers.erase(scene.layers.begin() + static_cast<std::ptrdiff_t>(index));
}

std::string layer_name(const Layer& layer) {
    if (layer.kind == "window") {
        if (layer.window_title.empty()) return layer.window_app.empty() ? "Window" : layer.window_app;
        return layer.window_app.empty() ? layer.window_title : layer.window_title + " (" + layer.window_app + ")";
    }
    std::string name = "Display " + std::to_string(layer.display);
    if (const auto r = parse_region(layer.region))
        name += ", " + std::to_string(r->width) + "x" + std::to_string(r->height) + " at " + std::to_string(r->x) +
                "," + std::to_string(r->y);
    return name;
}

std::vector<SourceRef> scene_sources(const Settings& s) {
    std::vector<SourceRef> sources{{SourceRef::Kind::screen, 0}};
    for (std::size_t i = 0; i < s.layers.size(); ++i) sources.push_back({SourceRef::Kind::layer, i});
    if (s.camera) sources.push_back({SourceRef::Kind::camera, 0});
    if (s.image) sources.push_back({SourceRef::Kind::image, 0});
    return sources;
}

std::string source_name(const Settings& s, const SourceRef& source) {
    switch (source.kind) {
    case SourceRef::Kind::screen:
        return s.source == "window" ? "Recorded window" : s.source == "region" ? "Recorded region" : "Recorded display";
    case SourceRef::Kind::camera: return "Camera";
    case SourceRef::Kind::image: return "Image";
    case SourceRef::Kind::layer: return source.layer < s.layers.size() ? layer_name(s.layers[source.layer]) : "";
    }
    return "";
}

SceneItem& scene_item(Scene& scene, const SourceRef& source) {
    switch (source.kind) {
    case SourceRef::Kind::camera: return scene.camera;
    case SourceRef::Kind::image: return scene.image;
    case SourceRef::Kind::layer:
        if (source.layer >= scene.layers.size()) scene.layers.resize(source.layer + 1, SceneItem{false, Area{}});
        return scene.layers[source.layer];
    case SourceRef::Kind::screen: break;
    }
    return scene.screen;
}

const SceneItem& scene_item(const Scene& scene, const SourceRef& source) {
    static const SceneItem hidden{false, Area{}};
    switch (source.kind) {
    case SourceRef::Kind::camera: return scene.camera;
    case SourceRef::Kind::image: return scene.image;
    case SourceRef::Kind::layer: return source.layer < scene.layers.size() ? scene.layers[source.layer] : hidden;
    case SourceRef::Kind::screen: break;
    }
    return scene.screen;
}

std::optional<SourceRef> source_at(const Settings& s, double x, double y, double handle_x, double handle_y) {
    const Scene& scene = current_scene(s);
    const auto sources = scene_sources(s);
    for (auto it = sources.rbegin(); it != sources.rend(); ++it) {
        const SceneItem& item = scene_item(scene, *it);
        if (item.visible && hits(item.area, x, y, handle_x, handle_y, full_frame(item.area))) return *it;
    }
    return std::nullopt;
}

AreaDrag grab_area(const Area& a, double x, double y, double handle_x, double handle_y, bool keep_shape) {
    AreaDrag drag;
    if (!hits(a, x, y, handle_x, handle_y, false)) return drag;
    drag.from = a;
    drag.x = x;
    drag.y = y;
    drag.keep_shape = keep_shape;
    const bool near_left = x < a.x + handle_x, near_right = x > a.x + a.width - handle_x;
    const bool near_top = y < a.y + handle_y, near_bottom = y > a.y + a.height - handle_y;
    if ((near_left || near_right) && (near_top || near_bottom)) {
        drag.grab = AreaDrag::Grab::resize;
        drag.left = near_left && !near_right ? true : near_right && !near_left ? false : x < a.x + a.width / 2;
        drag.top = near_top && !near_bottom ? true : near_bottom && !near_top ? false : y < a.y + a.height / 2;
    } else {
        drag.grab = AreaDrag::Grab::move;
    }
    return drag;
}

Area drag_area(const AreaDrag& drag, double x, double y) {
    const Area& from = drag.from;
    if (drag.grab == AreaDrag::Grab::move) {
        Area a = from;
        a.x = std::clamp(from.x + x - drag.x, 0.0, 1 - a.width);
        a.y = std::clamp(from.y + y - drag.y, 0.0, 1 - a.height);
        // Either edge may snap; the left and top win when both are near a target.
        if (const double left = snap_edge(a.x, kMarginX, kSnapX); left != a.x) a.x = left;
        else a.x = snap_edge(a.x + a.width, kMarginX, kSnapX) - a.width;
        if (const double top = snap_edge(a.y, kMarginY, kSnapY); top != a.y) a.y = top;
        else a.y = snap_edge(a.y + a.height, kMarginY, kSnapY) - a.height;
        a.x = std::clamp(a.x, 0.0, 1 - a.width);
        a.y = std::clamp(a.y, 0.0, 1 - a.height);
        return a;
    }
    if (drag.grab != AreaDrag::Grab::resize) return from;
    // The corner opposite the one held stays put.
    const double anchor_x = drag.left ? from.x + from.width : from.x;
    const double anchor_y = drag.top ? from.y + from.height : from.y;
    const double room_x = drag.left ? anchor_x : 1 - anchor_x;
    const double room_y = drag.top ? anchor_y : 1 - anchor_y;
    const double reach_x = drag.left ? anchor_x - x : x - anchor_x;
    const double reach_y = drag.top ? anchor_y - y : y - anchor_y;
    // The moving edge, snapped, as a width or height from the anchor.
    const auto snapped = [](double anchor, double reach, bool back, double margin, double within) {
        const double edge = snap_edge(back ? anchor - reach : anchor + reach, margin, within);
        return back ? anchor - edge : edge - anchor;
    };
    double width = 0, height = 0;
    if (drag.keep_shape) {
        const double aspect = from.height / from.width;
        const double room = std::min(room_x, room_y / aspect);
        width = std::clamp(std::max(reach_x, reach_y / aspect), std::min(kMinArea, room), room);
        const double by_x = snapped(anchor_x, width, drag.left, kMarginX, kSnapX);
        const double by_y = snapped(anchor_y, width * aspect, drag.top, kMarginY, kSnapY) / aspect;
        if (by_x != width && by_x > 0 && by_x <= room) width = by_x;
        else if (by_y != width && by_y > 0 && by_y <= room) width = by_y;
        height = width * aspect;
    } else {
        width = std::clamp(reach_x, std::min(kMinArea, room_x), room_x);
        height = std::clamp(reach_y, std::min(kMinArea, room_y), room_y);
        width = std::clamp(snapped(anchor_x, width, drag.left, kMarginX, kSnapX), std::min(kMinArea, room_x), room_x);
        height = std::clamp(snapped(anchor_y, height, drag.top, kMarginY, kSnapY), std::min(kMinArea, room_y), room_y);
    }
    return {drag.left ? anchor_x - width : anchor_x, drag.top ? anchor_y - height : anchor_y, width, height};
}

namespace {
// What a recording and the idle preview share: the picture, every source in the scenes and
// the audio settings. indices receives each source's overlay index.
Result capture_options(const Settings& s, bool test_pattern, Options& o, SourceIndices& indices) {
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
    o.codec = s.codec == "h264" ? Codec::h264 : s.codec == "hevc" ? Codec::hevc :
              s.codec == "prores" ? Codec::prores : Codec::automatic;
    o.screen_color = s.screen_color;
    o.camera_color = s.camera_color;
    o.cursor = s.cursor;
    o.clicks = s.clicks;
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
    o.preview_width = kPreviewWidth;
    const Scene& scene = current_scene(s);
    o.screen_visible = scene.screen.visible;
    o.screen_area = scene.screen.area;
    indices = {};
    for (std::size_t i = 0; i < s.layers.size(); ++i) {
        const Layer& layer = s.layers[i];
        Overlay l;
        if (layer.kind == "window") {
            const auto window = find_window(layer.window_app, layer.window_title);
            if (!window) return {"the window \"" + layer_name(layer) + "\" in the scenes is not open; open it or remove it"};
            l.kind = Overlay::Kind::window;
            l.window = window->id;
        } else {
            l.kind = Overlay::Kind::display;
            l.display = layer.display;
            if (!layer.region.empty()) l.region = parse_region(layer.region);
        }
        const SceneItem item = i < scene.layers.size() ? scene.layers[i] : SceneItem{false, Area{}};
        l.area = item.area;
        l.visible = item.visible;
        indices.layers.push_back(o.overlays.size());
        o.overlays.push_back(l);
    }
    if (s.camera) {
        indices.camera = o.overlays.size();
        Overlay c;
        c.kind = Overlay::Kind::camera;
        c.source = s.camera_device.empty() ? "default" : s.camera_device;
        c.area = scene.camera.area;
        c.visible = scene.camera.visible;
        c.style = s.camera_style;
        o.overlays.push_back(c);
    }
    if (s.image) {
        if (s.image_path.empty()) return {"choose an image to show"};
        indices.image = o.overlays.size();
        Overlay i;
        i.kind = Overlay::Kind::image;
        i.source = s.image_path;
        i.area = scene.image.area;
        i.visible = scene.image.visible;
        o.overlays.push_back(i);
    }
    if (uses_vertical(s)) {
        // The landscape picture has the source's shape; the resolution presets only bound it.
        indices.vertical = true;
        std::string ignored;
        const auto ratio = [](double w, double h) { return w > 0 && h > 0 ? w / h : 0.0; };
        double aspect = 0;
        if (test_pattern) aspect = ratio(o.width ? o.width : 1920, o.height ? o.height : 1080);
        else if (o.region) aspect = ratio(o.region->width, o.region->height);
        else if (o.window) {
            for (const auto& w : list_windows(ignored))
                if (w.id == o.window) aspect = ratio(w.width, w.height);
        } else if (const auto displays = list_displays(ignored); s.display >= 1 && s.display <= displays.size()) {
            aspect = ratio(displays[s.display - 1].width, displays[s.display - 1].height);
        }
        if (aspect > 0) indices.aspect = aspect;
        o.canvas2.width = kVerticalWidth;
        o.canvas2.height = kVerticalHeight;
        const bool camera = indices.camera.has_value();
        o.canvas2.screen = vertical_layout(scene, {SourceRef::Kind::screen}, indices.aspect, camera);
        o.canvas2.overlays.resize(o.overlays.size());
        for (std::size_t i = 0; i < indices.layers.size(); ++i)
            o.canvas2.overlays[indices.layers[i]] = vertical_layout(scene, {SourceRef::Kind::layer, i}, indices.aspect, camera);
        if (indices.camera) o.canvas2.overlays[*indices.camera] = vertical_layout(scene, {SourceRef::Kind::camera}, indices.aspect, camera);
        if (indices.image) o.canvas2.overlays[*indices.image] = vertical_layout(scene, {SourceRef::Kind::image}, indices.aspect, camera);
    }
    return {};
}
// Moves, shows and hides every source the capture has to where s's scene puts it, in both
// pictures when it composes the vertical video, and gives the camera its style.
template <typename Capture>
Result show(Capture& recorder, const SourceIndices& indices, const Settings& s) {
    const Scene& scene = current_scene(s);
    if (auto set = recorder.set_color(ColorTarget::screen, s.screen_color); !set) return set;
    if (auto set = recorder.set_color(ColorTarget::camera, s.camera_color); !set) return set;
    if (indices.camera)
        if (auto set = recorder.set_style(*indices.camera, s.camera_style); !set) return set;
    if (auto set = recorder.set_screen(scene.screen.visible, scene.screen.area); !set) return set;
    for (std::size_t i = 0; i < indices.layers.size() && i < scene.layers.size(); ++i)
        if (auto set = recorder.set_overlay(indices.layers[i], scene.layers[i].visible, scene.layers[i].area); !set)
            return set;
    if (indices.camera)
        if (auto set = recorder.set_overlay(*indices.camera, scene.camera.visible, scene.camera.area); !set) return set;
    if (indices.image)
        if (auto set = recorder.set_overlay(*indices.image, scene.image.visible, scene.image.area); !set) return set;
    if (!indices.vertical) return {};
    const auto place = [&](std::size_t layer, const SourceRef& source) {
        return recorder.set_layout(kSecondCanvas, layer, vertical_layout(scene, source, indices.aspect, indices.camera.has_value()));
    };
    if (auto set = place(kScreenLayer, {SourceRef::Kind::screen}); !set) return set;
    for (std::size_t i = 0; i < indices.layers.size(); ++i)
        if (auto set = place(indices.layers[i], {SourceRef::Kind::layer, i}); !set) return set;
    if (indices.camera)
        if (auto set = place(*indices.camera, {SourceRef::Kind::camera}); !set) return set;
    if (indices.image)
        if (auto set = place(*indices.image, {SourceRef::Kind::image}); !set) return set;
    return {};
}
// What needs a new capture for the idle preview; scenes only move what it has.
std::string preview_key(const Settings& s, bool test_pattern) {
    std::string key = s.source + '\n' + std::to_string(s.display) + '\n' + s.window_app + '\n' + s.window_title +
                      '\n' + s.region + '\n' + s.resolution + '\n' + (s.cursor ? "1" : "0") + (s.clicks ? "1" : "0") +
                      (test_pattern ? "1" : "0") + (uses_vertical(s) ? "v" : "");
    if (s.camera) key += "\ncamera " + s.camera_device;
    if (s.image) key += "\nimage " + s.image_path;
    for (const Layer& layer : s.layers)
        key += "\nlayer " + layer.kind + ' ' + std::to_string(layer.display) + ' ' + layer.region + '\n' +
               layer.window_app + '\n' + layer.window_title;
    return key;
}
} // namespace
Session::Session(std::function<void()> changed) : changed_(std::move(changed)) {}

Session::~Session() {
    stop();
    wait();
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }
    idle_wake_.notify_all();
    if (idle_thread_.joinable()) idle_thread_.join();
}

Result Session::start(const Settings& s, const std::vector<std::string>& keys, bool test_pattern) {
    {
        std::lock_guard lock(mutex_);
        if (state_ != State::idle) return {"a recording is already running"};
    }
    wait();
    Options o;
    SourceIndices indices;
    if (auto built = capture_options(s, test_pattern, o, indices); !built) return built;
    if (s.folder.empty()) return {"choose a folder for recordings"};
    std::error_code error;
    std::filesystem::create_directories(utf8_path(s.folder), error);
    if (error) return {"cannot use the recordings folder: " + error.message()};
    o.output = next_output(s.folder, o.codec == Codec::prores ? "mov" : s.format);
    if (s.stream) {
        std::vector<std::string> urls;
        for (std::size_t i = 0; i < s.destinations.size() && i < kMaxDestinations; ++i) {
            const Destination& d = s.destinations[i];
            if (!d.on) continue;
            const auto name = destination_name(s, i);
            const auto server = server_url(d);
            const auto key = i < keys.size() ? keys[i] : std::string();
            if (server.empty()) return {name + ": enter the server URL"};
            // A WHIP endpoint is the whole URL; its key, if any, is the bearer token.
            const bool whip = server.rfind("http://", 0) == 0 || server.rfind("https://", 0) == 0;
            if (key.empty() && !whip) return {name + ": enter the stream key"};
            const auto url = whip ? (key.empty() ? server : server + "#" + key) : server + "/" + key;
            if (std::find(urls.begin(), urls.end(), url) != urls.end()) return {name + " goes to the same place as another destination"};
            urls.push_back(url);
            // Each destination gets the picture it is set to; a vertical one the second canvas.
            (d.vertical ? o.canvas2.stream_urls : o.stream_urls).push_back(url);
        }
        if (urls.empty()) return {"turn on a destination to go live"};
    }
    if (s.vertical_movie) {
        // NAME-vertical.mp4 beside the recording, with -2, -3 ... like next_output() if taken.
        const auto dot = o.output.rfind('.');
        const auto stem = o.output.substr(0, dot) + "-vertical";
        std::error_code ignored;
        o.canvas2.output = stem + ".mp4";
        for (int n = 2; std::filesystem::exists(utf8_path(o.canvas2.output), ignored); ++n)
            o.canvas2.output = stem + "-" + std::to_string(n) + ".mp4";
    }
    o.stream_kbps = s.stream_kbps;
    if (s.replay) o.replay_seconds = s.replay_seconds;
    {
        std::lock_guard lock(mutex_);
        // The idle preview gives up the screen and camera before the recording opens them.
        idle_settings_.reset();
        idle_key_.clear();
        ++idle_generation_;
        idle_wake_.notify_all();
        options_ = o;
        state_ = State::starting;
        stop_requested_ = detach_requested_ = false;
        streaming_ = s.stream;
        replay_ = s.replay;
        indices_ = indices;
        scene_ = std::min<std::uint32_t>(s.scene, static_cast<std::uint32_t>(s.scenes.size() - 1));
        error_.clear();
        stream_error_.clear();
        saved_.clear();
        saved_vertical_.clear();
        last_ = {};
        rate_bytes_ = 0;
        rate_time_ = std::chrono::steady_clock::now();
        stream_kbps_ = 0;
    }
    auto recorder = std::make_unique<cli::EngineRecorder>(o, cli::engine_socket(), std::vector<std::string>{"--engine"});
    worker_ = std::thread([this, recorder = std::move(recorder)]() mutable { run(std::move(recorder), false); });
    return {};
}

bool Session::attach(const Settings& s, std::string& error) {
    error.clear();
    {
        std::lock_guard lock(mutex_);
        if (state_ != State::idle) return false;
    }
    wait();
    Options o;
    auto recorder = cli::EngineRecorder::attach(cli::engine_socket(), o, error);
    if (!recorder) return false;
    const bool paused = recorder->stats().paused;
    SourceIndices indices;
    for (std::size_t i = 0; i < o.overlays.size(); ++i) {
        const auto kind = o.overlays[i].kind;
        if (kind == Overlay::Kind::camera && !indices.camera) indices.camera = i;
        else if (kind == Overlay::Kind::image && !indices.image) indices.image = i;
        else if (kind == Overlay::Kind::display || kind == Overlay::Kind::window) indices.layers.push_back(i);
    }
    indices.vertical = o.canvas2.width > 0;
    if (o.width && o.height) indices.aspect = static_cast<double>(o.width) / o.height;
    {
        std::lock_guard lock(mutex_);
        idle_settings_.reset();
        idle_key_.clear();
        ++idle_generation_;
        idle_wake_.notify_all();
        options_ = o;
        state_ = State::starting;
        stop_requested_ = detach_requested_ = false;
        streaming_ = !o.stream_urls.empty() || !o.canvas2.stream_urls.empty();
        replay_ = o.replay_seconds > 0;
        indices_ = indices;
        scene_ = std::min<std::uint32_t>(s.scene, static_cast<std::uint32_t>(s.scenes.size() - 1));
        error_.clear();
        stream_error_.clear();
        saved_.clear();
        saved_vertical_.clear();
        last_ = {};
        rate_bytes_ = 0;
        rate_time_ = std::chrono::steady_clock::now();
        stream_kbps_ = 0;
        paused_on_attach_ = paused;
    }
    worker_ = std::thread([this, recorder = std::move(recorder)]() mutable { run(std::move(recorder), true); });
    return true;
}

void Session::detach() {
    {
        std::lock_guard lock(mutex_);
        if (state_ != State::recording && state_ != State::paused) return;
        detach_requested_ = stop_requested_ = true;
    }
    wake_.notify_all();
    wait();
}
void Session::run(std::unique_ptr<cli::EngineRecorder> recorder, bool attached) {
    {
        std::unique_lock lock(mutex_);
        idle_wake_.wait(lock, [this] { return !idle_alive_; });
    }
    const auto started = attached ? Result{} : recorder->start();
    {
        std::lock_guard lock(mutex_);
        if (!started) {
            error_ = started.error;
            state_ = State::idle;
        } else {
            recorder_ = recorder.get();
            recorder_->show_preview(preview_shown_);
            state_ = attached && paused_on_attach_ ? State::paused : State::recording;
        }
    }
    if (changed_) changed_();
    if (!started) return;
    bool let_go = false;
    {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return stop_requested_; });
        // Letting go, the engine keeps recording; only this session ends.
        let_go = detach_requested_;
        if (let_go) recorder_ = nullptr;
        state_ = let_go ? State::idle : State::finishing;
    }
    if (changed_) changed_();
    if (let_go) return;
    // Finalizing can take seconds; it happens here, never on the UI thread.
    const auto stopped = recorder->stop();
    {
        std::lock_guard lock(mutex_);
        last_ = recorder->stats();
        error_ = !stopped ? stopped.error : recorder->failure();
        stream_error_ = recorder->stream_failure();
        std::error_code ignored;
        if (std::filesystem::exists(utf8_path(options_.output), ignored)) saved_ = options_.output;
        if (!options_.canvas2.output.empty() && std::filesystem::exists(utf8_path(options_.canvas2.output), ignored))
            saved_vertical_ = options_.canvas2.output;
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

Result Session::show_scene(const Settings& settings) {
    std::lock_guard lock(mutex_);
    if (state_ == State::recording || state_ == State::paused) {
        if (stop_requested_) return {"not recording"};
        auto shown = show(*recorder_, indices_, settings);
        if (shown) scene_ = std::min<std::uint32_t>(settings.scene, static_cast<std::uint32_t>(settings.scenes.size() - 1));
        return shown;
    }
    if (idle_settings_) {
        idle_settings_->scenes = settings.scenes;
        idle_settings_->scene = settings.scene;
        idle_settings_->camera_style = settings.camera_style;
        idle_settings_->screen_color = settings.screen_color;
        idle_settings_->camera_color = settings.camera_color;
        if (idle_) {
            if (auto set = idle_->set_color(ColorTarget::screen, settings.screen_color); !set) return set;
            if (auto set = idle_->set_color(ColorTarget::camera, settings.camera_color); !set) return set;
        }
        place_idle();
    }
    return {};
}
Result Session::set_volume(AudioInput input, int volume, bool muted) {
    std::lock_guard lock(mutex_);
    if ((state_ != State::recording && state_ != State::paused) || stop_requested_) return {"not recording"};
    return recorder_->set_volume(input, std::clamp(volume, kVolumeMin, kVolumeMax), muted);
}

bool Session::preview(PreviewFrame& frame, std::size_t canvas) {
    std::lock_guard lock(mutex_);
    const void* source = recorder_ ? static_cast<const void*>(recorder_) : idle_;
    // Each capture numbers its frames from 1.
    auto& last = preview_source_[canvas == kSecondCanvas ? 1 : 0];
    if (source != last) {
        last = source;
        frame.number = 0;
    }
    if (recorder_) return recorder_->preview(frame, canvas);
    return idle_ && idle_->preview(frame, canvas);
}

void Session::idle_preview(const Settings* settings, bool test_pattern) {
    {
        std::lock_guard lock(mutex_);
        if (state_ != State::idle) settings = nullptr;
        const std::string key = settings ? preview_key(*settings, test_pattern) : std::string();
        if (key == idle_key_) {
            if (settings && idle_settings_) {
                idle_settings_->scenes = settings->scenes;
                idle_settings_->scene = settings->scene;
                idle_settings_->camera_style = settings->camera_style;
                idle_settings_->screen_color = settings->screen_color;
                idle_settings_->camera_color = settings->camera_color;
                if (idle_) {
                    idle_->set_color(ColorTarget::screen, settings->screen_color);
                    idle_->set_color(ColorTarget::camera, settings->camera_color);
                }
                place_idle();
            }
            return;
        }
        // A capture that failed keeps its key, so it is retried only once a setting changes.
        idle_key_ = key;
        idle_error_.clear();
        if (settings) idle_settings_ = *settings;
        else idle_settings_.reset();
        idle_test_pattern_ = test_pattern;
        ++idle_generation_;
        if (!idle_thread_.joinable()) idle_thread_ = std::thread([this] { run_idle(); });
    }
    idle_wake_.notify_all();
}

void Session::place_idle() {
    if (idle_ && idle_settings_) show(*idle_, idle_indices_, *idle_settings_);
}

void Session::run_idle() {
    std::unique_ptr<Recorder> running;
    std::uint64_t seen = 0;
    std::unique_lock lock(mutex_);
    const auto changed = [&] {
        lock.unlock();
        if (changed_) changed_();
        lock.lock();
    };
    for (;;) {
        idle_wake_.wait(lock, [&] { return closing_ || seen != idle_generation_; });
        if (closing_) break;
        seen = idle_generation_;
        if (running) {
            idle_ = nullptr;
            lock.unlock();
            running->stop();
            running.reset();
            lock.lock();
        }
        if (!idle_settings_) {
            idle_alive_ = false;
            idle_wake_.notify_all();
            changed();
            continue;
        }
        const Settings settings = *idle_settings_;
        const bool test_pattern = idle_test_pattern_;
        idle_alive_ = true;
        lock.unlock();
        // Finding a window and starting capture can take a while; never on the UI thread.
        Options o;
        SourceIndices indices;
        Result started = capture_options(settings, test_pattern, o, indices);
        std::unique_ptr<Recorder> recorder;
        if (started) {
            o.preview_only = true;
            o.fps = std::min<std::uint32_t>(o.fps, 30);
            recorder = std::make_unique<Recorder>(o);
            started = recorder->start();
        }
        lock.lock();
        if (started) {
            running = std::move(recorder);
            idle_ = running.get();
            idle_indices_ = indices;
            idle_->show_preview(true);
            place_idle();
        } else {
            idle_error_ = started.error;
            idle_alive_ = false;
            idle_wake_.notify_all();
        }
        changed();
    }
    if (running) {
        idle_ = nullptr;
        lock.unlock();
        running->stop();
        lock.lock();
    }
    idle_alive_ = false;
    idle_wake_.notify_all();
}

void Session::show_preview(bool on) {
    std::lock_guard lock(mutex_);
    preview_shown_ = on;
    if (recorder_) recorder_->show_preview(on);
}

Status Session::status() {
    bool ended = false;
    Status s;
    {
        std::lock_guard lock(mutex_);
        const bool active = recorder_ && (state_ == State::recording || state_ == State::paused);
        if (active) {
            last_ = recorder_->stats();
            // Grows as more servers end early.
            if (auto failure = recorder_->stream_failure(); !failure.empty()) stream_error_ = std::move(failure);
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
        s.saved_vertical = saved_vertical_;
        s.vertical = options_.canvas2.width > 0;
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
        s.servers = st.stream_count;
        s.servers_live = 0;
        for (std::uint32_t i = 0; active && i < st.stream_count; ++i) s.servers_live += st.streams[i].live ? 1 : 0;
        s.replay_seconds = active ? st.replay_seconds : 0;
        s.scene = scene_;
        s.camera_frames = st.camera_frames;
        s.system_audio_lost = active && st.system_audio_lost;
        s.microphone_lost = active && st.microphone_lost;
        s.camera_lost = active && st.camera_lost;
        s.window_closed = st.window_closed;
        // A preview capture that ended on its own (its window closed, a device lost) stops.
        if (idle_ && idle_->finished() && idle_settings_) {
            const auto failure = idle_->failure();
            idle_error_ = !failure.empty() ? failure : "the window closed; choose another source";
            idle_settings_.reset();
            ++idle_generation_;
            idle_wake_.notify_all();
        }
        s.previewing = idle_ != nullptr && !idle_->finished();
        if (s.previewing && state_ == State::idle) s.camera_frames = idle_->stats().camera_frames;
        s.preview_error = idle_error_;
    }
    if (ended) stop();
    return s;
}

void Session::wait() {
    if (worker_.joinable()) worker_.join();
}

} // namespace rec::app
