#pragma once

#include "cli/engine_client.hpp"
#include "rec/rec.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// The app layer shared by the macOS and Windows interfaces: persisted settings, stream
// destinations, and one recording session at a time. Platform shells own windows, menus,
// hotkeys, folders and secret storage; they never talk to rec::Recorder directly.
namespace rec::app {

// A display or window shown in scenes besides the screen, camera and image.
struct Layer {
    std::string kind = "display";     // display or window.
    std::uint32_t display = 1;
    std::string region;               // display: x,y,width,height in its pixels; empty for all of it.
    std::string window_app, window_title;  // window: found again by app and title.
};

// How a scene shows one source: whether, and where as fractions of the frame.
struct SceneItem {
    bool visible = true;
    Area area;
};

// A named arrangement of every source. Switching scenes moves, shows and hides them at once;
// a recording keeps capturing all of them.
struct Scene {
    std::string name;
    SceneItem screen, camera, image;
    std::vector<SceneItem> layers;    // One per Settings::layers, in order.
    std::string vertical = "fit";     // How the vertical video shows it: one of kVerticalLayouts.
};

// The vertical video's size: 9:16 for TikTok, YouTube Shorts and Reels.
inline constexpr std::uint32_t kVerticalWidth = 1080, kVerticalHeight = 1920;

// A place the live stream goes. Presets (Twitch, YouTube) have a fixed server; X, TikTok and
// custom servers are pasted in. Its key is kept by the platform's secret store under
// key_name(), never in the settings file.
struct Destination {
    bool on = true;                   // Off: kept, with its key, but not sent to.
    std::string service = "twitch";   // One of kServices.
    std::string server;               // Typed services: rtmp[s]://host[:port]/app.
    bool vertical = false;            // Send the vertical video instead of the landscape one.
};

// The engine sends one encode to at most four servers, across both canvases.
inline constexpr std::size_t kMaxDestinations = 4;

// With the camera and the image, the engine's 15 overlays.
inline constexpr std::size_t kMaxLayers = 13;

// Screen and camera, Screen only, Camera only.
std::vector<Scene> default_scenes();

struct Settings {
    std::string folder;               // Empty: the platform's movies folder.
#ifdef _WIN32
    std::string format = "mp4";       // mov or mp4.
#else
    std::string format = "mov";
#endif
    std::uint32_t display = 1;
    std::string source = "display";   // display, window, region.
    // The window to record, found again by app and title because window IDs change.
    std::string window_app, window_title;
    std::string region;               // x,y,width,height in pixels of the display.
    std::string resolution = "1080p"; // 720p, 1080p, 1440p, 4k, native.
    std::uint32_t fps = 60;
    std::string codec = "auto";       // auto, h264, hevc.
    bool cursor = true;
    bool clicks = false;
    bool system_audio = true;
    bool microphone = false;
    std::string microphone_device = "default";
    // Volume in dB (kVolumeMin to kVolumeMax) and mute of each source in the program audio
    // (mix, stream, replay); both change while recording.
    int system_volume = 0, mic_volume = 0;
    bool system_muted = false, mic_muted = false;
    bool stream = false;              // Go live while recording.
    // Up to kMaxDestinations; each one that is on gets the stream. Never empty.
    std::vector<Destination> destinations{Destination{}};
    bool vertical_movie = false;      // Also save the vertical video, as NAME-vertical.mp4.
    std::uint32_t stream_kbps = 6000;
    bool replay = false;              // Keep the last replay_seconds for Save Replay.
    std::uint32_t replay_seconds = 30;
    bool camera = false;              // Use a camera in the scenes.
    std::string camera_device = "default";  // default, test, phone_camera_id(), or a camera ID.
    std::string phone_token;          // The secret in the phone camera's address; made once, kept.
    rec::Style camera_style;          // The camera's shape, border and shadow in every scene; changeable while recording.
    rec::ColorAdjust screen_color, camera_color;
    bool image = false;               // Use an image (PNG, JPEG) in the scenes, drawn over everything, fitted.
    std::string image_path;
    std::vector<Layer> layers;        // Drawn above the screen and below the camera, in order.
    std::vector<Scene> scenes = default_scenes();  // Never empty.
    std::uint32_t scene = 0;          // The scene shown; switchable while recording.
    bool preview = true;              // Show the scene before recording while the window shows.
    // Quitting during a recording: "ask" (the first time), "yes" (the background engine keeps
    // recording) or "no" (stop and save first).
    std::string keep_recording = "ask";
    std::string last_recording;
};

// The phone camera in camera lists, "phone:" and settings.phone_token: the same each run, so
// a phone's open page reconnects by itself.
std::string phone_camera_id(const Settings& settings);
bool is_phone_camera(const std::string& device);

// The choices both interfaces offer, as a settings value and its label.
struct Choice {
    const char* value;
    const char* label;
};
inline constexpr std::array<Choice, 5> kResolutions{
    {{"720p", "720p"}, {"1080p", "1080p"}, {"1440p", "1440p"}, {"4k", "4K"}, {"native", "Native"}}};
inline constexpr std::array<std::uint32_t, 6> kFrameRates{24, 25, 30, 50, 60, 120};
#ifdef __APPLE__
// ProRes always records to .mov, whatever the format setting says.
inline constexpr std::array<Choice, 4> kCodecs{
    {{"auto", "Automatic"}, {"h264", "H.264"}, {"hevc", "HEVC"}, {"prores", "ProRes 422 (MOV)"}}};
#else
inline constexpr std::array<Choice, 3> kCodecs{{{"auto", "Automatic"}, {"h264", "H.264"}, {"hevc", "HEVC"}}};
#endif
inline constexpr std::array<Choice, 2> kFormats{{{"mp4", "MP4"}, {"mov", "MOV"}}};
inline constexpr std::array<std::uint32_t, 5> kReplayLengths{15, 30, 60, 120, 300};
inline constexpr int kVolumeMin = -40, kVolumeMax = 20;
// Preview frames are made this wide at most: sharp in a half-window view on a 2x display.
inline constexpr std::uint32_t kPreviewWidth = 960;
// Custom stays last; the self-test picks it by position.
inline constexpr std::array<Choice, 5> kServices{
    {{"twitch", "Twitch"}, {"youtube", "YouTube"}, {"x", "X (Twitter)"}, {"tiktok", "TikTok"}, {"custom", "Custom server (RTMP or WHIP)"}}};
inline constexpr std::array<Choice, 5> kCorners{{{"bottom-right", "Bottom right"},
                                                {"bottom-left", "Bottom left"},
                                                {"top-right", "Top right"},
                                                {"top-left", "Top left"},
                                                {"custom", "Custom"}}};
inline constexpr std::array<Choice, 3> kCameraSizes{{{"small", "Small"}, {"medium", "Medium"}, {"large", "Large"}}};
// In rec::Style::Shape's order.
inline constexpr std::array<Choice, 6> kCameraShapes{{{"rectangle", "Rectangle"},
                                                     {"rounded", "Rounded"},
                                                     {"circle", "Circle"},
                                                     {"square", "Square"},
                                                     {"pill", "Pill"},
                                                     {"squircle", "Squircle"}}};
// The camera borders offered: width in pixels at 1080p and color.
struct BorderChoice {
    double width;
    std::uint32_t color;
    const char* label;
};
inline constexpr std::array<BorderChoice, 5> kCameraBorders{{{0, 0xffffff, "None"},
                                                             {4, 0xffffff, "Thin white"},
                                                             {10, 0xffffff, "Thick white"},
                                                             {4, 0x000000, "Thin black"},
                                                             {10, 0x000000, "Thick black"}}};
// The kCameraBorders entry style has, or -1.
inline int border_index(const rec::Style& style) {
    for (std::size_t i = 0; i < kCameraBorders.size(); ++i)
        if (kCameraBorders[i].width == style.border && (style.border == 0 || kCameraBorders[i].color == style.border_color))
            return static_cast<int>(i);
    return -1;
}
// Fit: the whole landscape picture in the middle. Fill: its middle fills the frame and the
// camera sits below. Split: the middle on top, the camera filling the bottom half.
inline constexpr std::array<Choice, 3> kVerticalLayouts{
    {{"fit", "Whole picture"}, {"fill", "Fill, sides cropped"}, {"split", "Screen above camera"}}};

template <std::size_t N>
bool is_choice(const std::array<Choice, N>& choices, const std::string& value) {
    for (const auto& choice : choices)
        if (value == choice.value) return true;
    return false;
}

// The position of value in choices, or 0.
template <std::size_t N>
int choice_index(const std::array<Choice, N>& choices, const std::string& value) {
    for (std::size_t i = 0; i < N; ++i)
        if (value == choices[i].value) return static_cast<int>(i);
    return 0;
}

// key=value lines; unknown keys are ignored and missing ones keep their defaults.
Settings load_settings(const std::string& path);
bool save_settings(const std::string& path, const Settings& settings);

// "x,y,width,height" in display pixels; empty when malformed or smaller than 2x2.
std::optional<Region> parse_region(const std::string& text);
std::string region_text(const Region& region);
// The open window that settings.window_app and window_title name: the same title, else the
// first window of that app.
std::optional<Window> find_window(const Settings& settings);
std::optional<Window> find_window(const std::string& app, const std::string& title);

// The scene shown (settings.scene, kept in range).
Scene& current_scene(Settings& settings);
const Scene& current_scene(const Settings& settings);
// A copy of the shown scene named "Scene N", shown next. False at 999 scenes.
bool add_scene(Settings& settings);
// Removes the shown scene, unless it is the only one.
void remove_scene(Settings& settings);
// Adds a source to every scene: shown over the whole frame in the shown scene, hidden in
// the others. False when there are kMaxLayers already.
bool add_layer(Settings& settings, const Layer& layer);
void remove_layer(Settings& settings, std::size_t index);
// "Display 2", "Display 1, 640x360 at 0,0", or the window's title and app.
std::string layer_name(const Layer& layer);

// One source in the scenes.
struct SourceRef {
    enum class Kind { screen, camera, image, layer };
    Kind kind = Kind::screen;
    std::size_t layer = 0;  // Into Settings::layers.
    bool operator==(const SourceRef&) const = default;
};
// One mixer column, in order: system audio, microphone, screen, camera, then layers.
struct Strip {
    enum class Kind { audio, visual };
    Kind kind;
    std::string name;
    std::optional<rec::AudioInput> input;
    std::optional<SourceRef> source;
    bool color = false;  // Derivable from source, kept for future layers.
};
std::vector<Strip> mixer_strips(const Settings& settings);
// Peak meter over the last 60 dB, falling about 20 dB per second assuming a 50 ms poll.
struct Meter {
    float shown = 0;
    float feed(float peak);
    void reset() { shown = 0; }
};
// The sources settings use, bottom to top as drawn: the recorded screen, the layers, the
// camera, the image.
std::vector<SourceRef> scene_sources(const Settings& settings);
std::string source_name(const Settings& settings, const SourceRef& source);
SceneItem& scene_item(Scene& scene, const SourceRef& source);
const SceneItem& scene_item(const Scene& scene, const SourceRef& source);
// The topmost shown source at x,y in the shown scene (fractions of the frame). A source that
// fills the frame is taken only by a corner, within handle_x, handle_y.
std::optional<SourceRef> source_at(const Settings& settings, double x, double y, double handle_x, double handle_y);

// The camera in a corner (one of kCorners but custom) at a size (one of kCameraSizes).
Area corner_area(const std::string& corner, const std::string& size);
// The corner and size that place the camera at area, or "custom" and "".
std::string corner_of(const Area& area);
std::string size_of(const Area& area);

// Moving and resizing a source with the pointer in the preview. Points are fractions of
// the frame, x from the left and y from the top.
struct AreaDrag {
    enum class Grab { none, move, resize };
    Grab grab = Grab::none;
    Area from;               // The source's area when the drag began.
    double x = 0, y = 0;     // Where it began.
    bool left = false, top = false;  // The corner held while resizing.
    bool keep_shape = true;  // A resize keeps the area's aspect ratio.
};
// What a press at x,y takes hold of: a corner of area (within handle_x, handle_y of it)
// resizes it, anywhere else inside moves it. The camera keeps its shape, since it is cropped
// to fill its area; the other sources fit inside theirs.
AreaDrag grab_area(const Area& area, double x, double y, double handle_x, double handle_y, bool keep_shape);
// The area with the pointer now at x,y: it stays inside the frame and a resize keeps the
// opposite corner in place. Edges snap to the frame's edges and middle, and to the corner
// margins.
Area drag_area(const AreaDrag& drag, double x, double y);

// Whether a recording composes the vertical video: saved, or sent to a destination.
bool uses_vertical(const Settings& settings);
// Where the vertical video shows a source of scene (kVerticalLayouts), for a landscape
// picture aspect (width / height): the part of the landscape picture the layout keeps, mapped
// into the vertical frame. camera says whether the capture has a camera; without one shown,
// "split" fills the frame like "fill".
Layout vertical_layout(const Scene& scene, const SourceRef& source, double aspect, bool camera);

// rtmp:// or rtmps://host/app for the destination; the stream key is appended as the last
// segment. The Twitch and YouTube presets use RTMPS.
std::string server_url(const Destination& destination);
std::string server_url(const std::string& service, const std::string& server);
// X and TikTok give each account (TikTok each LIVE) its own server, so it is pasted like a
// custom one. These services take a typed server URL.
bool typed_server(const std::string& service);
// Where to find the server URL and key for a service, as a placeholder; empty for presets.
const char* server_hint(const std::string& service);
// The service's page that shows the stream key (and, for TikTok and X, the server); empty for
// custom servers.
const char* key_page(const std::string& service);
// What copied text is to a destination: an RTMP server URL, a stream key (one token, no
// scheme), or neither. Lets the app fill both from the clipboard after Get Key.
struct Pasted {
    std::string server, key;
};
Pasted read_pasted(const std::string& text);
// The name a destination's key is kept under: its server URL, then "#2", "#3" ... for later
// destinations with the same server (two YouTube channels). Empty without a server.
std::string key_name(const Settings& settings, std::size_t destination);
// Whether any destination's key is kept under this name.
bool uses_key_name(const Settings& settings, const std::string& name);
// "YouTube", or "YouTube 2" when an earlier destination uses the same service.
std::string destination_name(const Settings& settings, std::size_t destination);
// Adds a destination after the others: the first service none of them uses. False at
// kMaxDestinations.
bool add_destination(Settings& settings);
// Removes one, unless it is the only one.
void remove_destination(Settings& settings, std::size_t destination);
enum class State { idle, starting, recording, paused, finishing };
struct Status;
// What a lost source means for the recording, for the status line; empty when none is lost.
std::string lost_sources_text(const Status& status);

struct Status {
    State state = State::idle;
    double seconds = 0;              // Recorded time, excluding pauses.
    std::uint64_t frames = 0, dropped = 0, audio_dropped = 0;
    float system_peak = 0, mic_peak = 0;  // Since the previous status() call.
    bool system_audio = false, microphone = false;  // Sources in this recording.
    bool streaming = false;          // A stream was requested for this recording.
    bool live = false;               // Connected and sending.
    bool vertical = false;           // This recording also makes the vertical video.
    bool reconnecting = false;       // Lost the server and trying again.
    std::uint32_t reconnects = 0;
    std::uint64_t stream_dropped = 0;
    double stream_kbps = 0;          // Send rate over about the last second.
    std::uint32_t stream_queued_ms = 0;  // Media waiting to be sent; grows when the network falls behind.
    std::uint32_t servers = 0, servers_live = 0;  // Stream servers, and those connected and sending.
    std::string stream_error;        // Why the stream ended; the recording continues.
    bool replay = false;             // This recording keeps a replay buffer.
    double replay_seconds = 0;       // Media the replay buffer holds now.
    std::uint32_t scene = 0;         // The scene shown in this recording.
    std::uint64_t camera_frames = 0;
    // A source stopped delivering (unplugged); the recording continues without it.
    bool system_audio_lost = false, microphone_lost = false, camera_lost = false;
    bool window_closed = false;      // The recorded window closed, which ended the recording.
    bool previewing = false;         // The scene shows before recording (idle_preview()).
    std::string preview_error;       // Why it cannot.
    std::string error;               // Why the last recording failed or ended early.
    std::string saved;               // Movie the last recording saved; empty while one runs.
    std::string saved_vertical;      // Its vertical video, when it saved one.
};

// Each source's overlay index in one capture.
struct SourceIndices {
    std::optional<std::size_t> camera, image;
    std::vector<std::size_t> layers;
    bool vertical = false;        // The capture composes the vertical video.
    double aspect = 16.0 / 9;     // Of its landscape picture, for vertical_layout().
};

// One recording at a time, hosted by the background engine (cli/engine_client.hpp) so it
// can outlive the app, and followed by a worker thread: start() checks the settings and
// returns at once; the worker starts the engine's capture (which can wait on permissions and
// the first frame), waits for stop(), then has the engine finalize the movie. The change callback runs on the
// worker after each step; it must only post to the UI thread, never call back in. A
// recording that ends on its own (capture lost) is stopped when status() sees it.
// start(), wait() and the destructor belong to one thread, the UI thread.
class Session {
public:
    explicit Session(std::function<void()> changed);
    ~Session();  // Finalizes a running recording.
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // keys holds each destination's stream key, in order; they are used only when
    // settings.stream is set, and only for destinations that are on. test_pattern records the
    // synthetic flash/beep sources instead of the screen, for end-to-end checks.
    Result start(const Settings& settings, const std::vector<std::string>& keys, bool test_pattern = false);
    void stop();
    // Takes over the recording the background engine runs, if any: one this app left
    // running when it quit, or `rec start`'s. True when there was one; it then runs as if
    // start() had started it. error says why the engine could not be asked.
    bool attach(const Settings& settings, std::string& error);
    // Lets go of a running recording without stopping it: the engine keeps recording and
    // saves it when stopped from the CLI or a later attach(). Returns once let go.
    void detach();
    Result pause();
    Result resume();
    // Writes the replay buffer to a new replay-YYYY-MM-DD-HHMMSS.mp4 in folder; path and
    // seconds receive where and how much. Takes as long as writing the file.
    Result save_replay(const std::string& folder, std::string& path, double& seconds);
    // Shows settings' current scene at once: in the recording, or in the preview before
    // Start. A recording keeps the sources it started with; their placement can change.
    Result show_scene(const Settings& settings);
    // Changes a source's volume and mute at once; the source must be in this recording.
    Result set_volume(AudioInput input, int volume, bool muted);
    // Copies the recording's newest preview frame when it is newer than frame.number: the
    // landscape picture, or the vertical video's with kSecondCanvas.
    bool preview(PreviewFrame& frame, std::size_t canvas = kMainCanvas);
    // Whether the interface shows the preview; frames are made only while it does.
    void show_preview(bool on);
    // Shows the scene before recording: while idle, a capture of settings for preview() only
    // (no file or audio) runs on its own thread; nullptr stops it. Call it whenever the
    // settings may have changed: a new source restarts capture, a scene change only moves
    // the layers. A recording stops it first and takes over preview().
    void idle_preview(const Settings* settings, bool test_pattern = false);
    Status status();
    void wait();  // Blocks until a stop in progress has saved the movie.

private:
    void run(std::unique_ptr<cli::EngineRecorder> recorder, bool attached);
    void run_idle();
    void place_idle();  // With mutex_ held.

    std::function<void()> changed_;
    std::mutex mutex_;
    std::condition_variable wake_;
    cli::EngineRecorder* recorder_ = nullptr;  // Owned by the worker; set while capture runs.
    Options options_;
    State state_ = State::idle;
    bool stop_requested_ = false;
    bool detach_requested_ = false;
    bool paused_on_attach_ = false;
    std::thread worker_;
    std::string error_, stream_error_, saved_, saved_vertical_;
    bool streaming_ = false, replay_ = false;
    bool preview_shown_ = true;
    SourceIndices indices_;              // This recording's.
    std::uint32_t scene_ = 0;
    Stats last_{};
    // Stream rate measurement: bytes sent at rate_time_.
    std::uint64_t rate_bytes_ = 0;
    std::chrono::steady_clock::time_point rate_time_{};
    double stream_kbps_ = 0;
    // The idle preview: what the interface wants (idle_settings_, changed when idle_key_
    // changes, which bumps idle_generation_) and the capture run_idle() keeps for it.
    std::condition_variable idle_wake_;
    std::thread idle_thread_;
    std::optional<Settings> idle_settings_;
    bool idle_test_pattern_ = false;
    std::string idle_key_, idle_error_;
    std::uint64_t idle_generation_ = 0;
    Recorder* idle_ = nullptr;          // Set while its capture runs.
    bool idle_alive_ = false;           // A capture exists, starting, running or stopping.
    bool closing_ = false;
    SourceIndices idle_indices_;
    const void* preview_source_[2] = {};  // Whose frames preview() copied last, per canvas.
};

// <prefix>-YYYY-MM-DD-HHMMSS.<format> in folder, with -2, -3 ... if that file exists.
std::string next_output(const std::string& folder, const std::string& format, const char* prefix = "rec");
std::string redact(const std::string& url);  // The URL without its stream key.

} // namespace rec::app
