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
};

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
    std::string destination = "twitch"; // twitch, youtube, x, tiktok, custom.
    std::string server;               // Custom destination: rtmp[s]://host[:port]/app.
    // A second server sent the same stream: none, or one of the destinations.
    std::string destination2 = "none";
    std::string server2;              // Custom second destination.
    std::string x_server, tiktok_server;  // Pasted from X Media Studio and TikTok LIVE Center.
    std::uint32_t stream_kbps = 6000;
    bool replay = false;              // Keep the last replay_seconds for Save Replay.
    std::uint32_t replay_seconds = 30;
    bool camera = false;              // Use a camera in the scenes.
    std::string camera_device = "default";  // default, test, or a camera ID.
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
    {{"twitch", "Twitch"}, {"youtube", "YouTube"}, {"x", "X (Twitter)"}, {"tiktok", "TikTok"}, {"custom", "Custom RTMP server"}}};
inline constexpr std::array<Choice, 6> kSecondServices{{{"none", "None"},
                                                        {"twitch", "Twitch"},
                                                        {"youtube", "YouTube"},
                                                        {"x", "X (Twitter)"},
                                                        {"tiktok", "TikTok"},
                                                        {"custom", "Custom RTMP server"}}};
inline constexpr std::array<Choice, 5> kCorners{{{"bottom-right", "Bottom right"},
                                                {"bottom-left", "Bottom left"},
                                                {"top-right", "Top right"},
                                                {"top-left", "Top left"},
                                                {"custom", "Custom"}}};
inline constexpr std::array<Choice, 3> kCameraSizes{{{"small", "Small"}, {"medium", "Medium"}, {"large", "Large"}}};

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

// rtmp:// or rtmps://host/app for the destination; the stream key is appended as the last
// segment. The Twitch and YouTube presets use RTMPS.
std::string server_url(const Settings& settings);
// The same for the second destination; empty when there is none.
std::string second_server_url(const Settings& settings);
std::string server_url(const std::string& destination, const std::string& server);
// X and TikTok give each account (TikTok each LIVE) its own server, so it is pasted like a
// custom one. These destinations take a typed server URL.
bool typed_server(const std::string& destination);
// The typed server of the first or second destination: each custom destination has its own,
// and both share the X and the TikTok server.
std::string typed_server_of(const Settings& settings, bool second);
void set_typed_server(Settings& settings, bool second, const std::string& server);
// Where to find the server URL and key for a destination, as a placeholder; empty for presets.
const char* server_hint(const std::string& destination);
enum class State { idle, starting, recording, paused, finishing };

struct Status {
    State state = State::idle;
    double seconds = 0;              // Recorded time, excluding pauses.
    std::uint64_t frames = 0, dropped = 0, audio_dropped = 0;
    float system_peak = 0, mic_peak = 0;  // Since the previous status() call.
    bool system_audio = false, microphone = false;  // Sources in this recording.
    bool streaming = false;          // A stream was requested for this recording.
    bool live = false;               // Connected and sending.
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
    bool window_closed = false;      // The recorded window closed, which ended the recording.
    bool previewing = false;         // The scene shows before recording (idle_preview()).
    std::string preview_error;       // Why it cannot.
    std::string error;               // Why the last recording failed or ended early.
    std::string saved;               // Movie the last recording saved; empty while one runs.
};

// Each source's overlay index in one capture.
struct SourceIndices {
    std::optional<std::size_t> camera, image;
    std::vector<std::size_t> layers;
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

    // The keys are used only when settings.stream is set, second_key only with a second
    // destination. test_pattern records the synthetic flash/beep sources instead of the
    // screen, for end-to-end checks.
    Result start(const Settings& settings, const std::string& stream_key, const std::string& second_key,
                 bool test_pattern = false);
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
    // Copies the recording's newest preview frame when it is newer than frame.number.
    bool preview(PreviewFrame& frame);
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
    std::string error_, stream_error_, saved_;
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
    const void* preview_source_ = nullptr;  // Whose frames preview() copied last.
};

// <prefix>-YYYY-MM-DD-HHMMSS.<format> in folder, with -2, -3 ... if that file exists.
std::string next_output(const std::string& folder, const std::string& format, const char* prefix = "rec");
std::string redact(const std::string& url);  // The URL without its stream key.

} // namespace rec::app
