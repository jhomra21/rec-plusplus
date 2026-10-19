#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rec {

// prores: Apple ProRes 422, macOS only, in a .mov file; it ignores bitrate_mbps.
enum class Codec { automatic, h264, hevc, prores };

// Track layout when both system audio and a microphone are recorded.
// With a single audio source the movie has a single audio track.
enum class AudioTracks {
    both,  // Mix (default track), system stem, microphone stem.
    mix,   // Mix only.
    stems  // System and microphone stems, no mix.
};

enum class AudioInput { system, microphone };

// How the capture is scaled to the output size. lanczos: rec captures at the source's native
// size and scales on the GPU with a Lanczos-3 filter, sharp and without aliasing. fast: the
// capture service scales (ScreenCaptureKit, or the Direct3D 11 video processor), with less GPU
// work and softer or aliased fine detail.
enum class Scaler { lanczos, fast };

// A rectangle of a display in its pixels, from the display's top-left corner.
struct Region {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// A rectangle in fractions of the output frame from its top-left corner; the default is
// the whole frame.
struct Area {
    double x = 0;
    double y = 0;
    double width = 1;
    double height = 1;
};

// How a canvas shows one source: whether, where (fractions of the canvas) and which part of
// the source (fractions of it; the default is all of it). A camera is cropped further to fill
// its area; the others keep their aspect ratio inside it.
struct Layout {
    bool visible = true;
    Area area;
    Area crop;
};

// How a camera is cut out and framed. Circle, square and squircle show a square of the
// camera's area, against the side of it nearer the frame's edge.
struct Style {
    enum class Shape { rectangle, rounded, circle, square, pill, squircle };
    Shape shape = Shape::rectangle;
    double roundness = 0.2;               // 0 (sharp) to 1 (as round as the shape allows): rounded, square, squircle.
    double border = 0;                    // Width in pixels at 1080p, scaled with the canvas; 0 to 40.
    std::uint32_t border_color = 0xffffff;  // 0xRRGGBB.
    bool shadow = false;                  // A soft drop shadow below the camera.
};

// A picture drawn over the capture in every frame: an image file, a camera, or another
// display or window captured live. Overlays draw in order, later ones on top.
struct Overlay {
    enum class Kind { image, camera, display, window };
    Kind kind = Kind::image;
    // image: a PNG, JPEG, BMP or GIF file. camera: "default", an ID or part of a name from
    // list_cameras(), or "test" for a synthetic camera (green with a moving white bar).
    std::string source;
    std::uint32_t display = 1; // display: 1-based index from list_displays().
    std::uint64_t window = 0; // window: id from list_windows().
    std::optional<Region> region; // display: only this part of it.
    // A camera is cropped to fill the area; the others keep their aspect ratio inside it.
    Area area;
    bool visible = true;
    Area crop;  // The part of the picture shown, in fractions of it.
    Style style;  // A camera's shape, border and shadow, the same in both canvases.
};

// A second picture of the same recording with its own size and its own layout of every
// source, for vertical video (TikTok, YouTube Shorts). The sources are captured once and
// composed twice. The second canvas is encoded once with H.264 at Options::stream_kbps, sent
// to its own servers and, with output set, written to an MP4 that survives a crash.
struct Canvas {
    std::uint32_t width = 0, height = 0;  // Even, 64 to 4096; 0 = no second canvas.
    Layout screen;                        // The main capture; crop is in fractions of it as the main canvas shows it.
    std::vector<Layout> overlays;         // One per Options::overlays.
    std::vector<std::string> stream_urls; // Counted with Options::stream_urls against kMaxStreams.
    std::string output;                   // An .mp4 path; empty = no file.
};

// Canvases for Recorder::set_layout() and preview().
inline constexpr std::size_t kMainCanvas = 0, kSecondCanvas = 1;
// The main capture's index in Recorder::set_layout().
inline constexpr std::size_t kScreenLayer = SIZE_MAX;

struct Options {
    std::string output = "rec.mov";
    std::uint32_t display = 1; // 1-based index from list_displays()
    std::uint64_t window = 0; // Capture this window (id from list_windows()) instead of a display.
    std::optional<Region> region; // Capture only this part of the display.
    std::uint32_t width = 1920; // bounding box, 0/0 selects native resolution
    std::uint32_t height = 1080;
    std::uint32_t fps = 60;
    std::uint32_t bitrate_mbps = 0; // 0 = selected from resolution
    Codec codec = Codec::automatic;
    Scaler scaler = Scaler::lanczos;
    bool cursor = true;
    // Mark mouse presses on the main capture: a circle at the pointer, yellow for the left
    // button and blue for the right, that fades out after release.
    bool clicks = false;
    bool test_pattern = false; // Synthetic flashing frame, for hardware-encoder E2E without screen permission.
    bool test_tone = false; // Synthetic beep as system audio, once a second with the flash (requires test_pattern).
    bool test_mic = false; // Synthetic quiet microphone source (requires test_pattern).
    bool system_audio = false; // Opt-in ScreenCaptureKit system audio, 48 kHz stereo AAC.
    bool microphone = false; // Opt-in ScreenCaptureKit microphone (macOS 15+).
    std::string microphone_device = "default"; // "default", unique ID, or name substring.
    AudioTracks audio_tracks = AudioTracks::both;
    // Volume and mute act on the program audio: the mix track, or the only audio track when
    // one source is recorded, and the stream and replay. Stems keep each source as captured.
    double system_gain_db = 0; // -60 to 24.
    double mic_gain_db = 0;
    bool system_muted = false;
    bool mic_muted = false;
    std::optional<double> system_audio_offset_ms; // Unset: default output device latency (0 for test tone).
    double mic_offset_ms = 0;
    double duration_seconds = 10.0; // 0 = until stop()
    // Live streams of the same timeline, encoded once beside the file and sent to each server
    // (at most kMaxStreams with canvas2's): rtmp[s]://host[:port]/app/stream-key.
    std::vector<std::string> stream_urls;
    std::uint32_t stream_kbps = 6000; // Stream video bitrate; audio is 160 kb/s AAC.
    // Keep the last N seconds (5 to 300) of the stream encode in memory for save_replay();
    // 0 = off. Without stream_url the stream encoders run for the replay alone.
    std::uint32_t replay_seconds = 0;
    // Where the capture above (display, window, region or test pattern) sits in the frame,
    // keeping its aspect ratio, and whether it shows. The rest of the frame is black.
    Area screen_area;
    bool screen_visible = true;
    Area screen_crop; // The part of the capture shown, in fractions of it.
    std::vector<Overlay> overlays; // At most 15, drawn over the capture.
    Canvas canvas2; // A second canvas, usually vertical; off while its width is 0.
    // Make preview() frames up to this many pixels wide, in the output's shape; 0 = no preview.
    std::uint32_t preview_width = 0;
    // Capture and draw the overlays for preview() only: no file, audio, stream or replay, and
    // no duration. Needs preview_width; output and the audio options are ignored.
    bool preview_only = false;
};

struct Display {
    std::uint32_t id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string name;
};

struct Window {
    std::uint64_t id = 0;
    std::uint32_t width = 0; // Pixels.
    std::uint32_t height = 0;
    std::string title;
    std::string app;
};

struct AudioDevice {
    std::string id;
    std::string name;
};

struct Camera {
    std::string id;
    std::string name;
};

inline constexpr std::size_t kMaxStreams = 4;

// One server of a live stream.
struct StreamStats {
    bool live = false;
    bool reconnecting = false;
    std::uint32_t reconnects = 0;
    std::uint64_t frames = 0;  // Video frames queued for this server.
    std::uint64_t dropped = 0; // Video frames this server skipped to keep up.
    std::uint64_t bytes = 0;
    std::uint32_t queued_ms = 0;
};

struct Stats {
    std::uint64_t captured = 0;
    std::uint64_t written = 0;
    std::uint64_t dropped = 0;
    std::uint64_t audio_written = 0; // System audio buffers.
    std::uint64_t audio_dropped = 0;
    std::uint64_t mic_written = 0;
    std::uint64_t mic_dropped = 0;
    std::uint64_t mix_written = 0;
    // Linear peak (0 to 1) of each source since the previous stats() call, for level meters.
    float system_peak = 0;
    float mic_peak = 0;
    double system_audio_offset_ms = 0;
    bool paused = false;
    double paused_seconds = 0; // Total time paused, excluded from the movie.
    bool window_closed = false; // The recorded window closed; that ended the recording, which was saved.
    // The live stream across its servers; streams has each one, in Options::stream_urls order.
    bool streaming = false; // Connected and sending to at least one server.
    bool stream_reconnecting = false; // Lost a server and trying to reconnect.
    std::uint32_t stream_reconnects = 0; // Times a server reconnected after a loss.
    bool stream_hardware = false; // The stream uses a hardware video encoder.
    std::uint64_t stream_frames = 0; // Video frames sent, to the server that took the most.
    std::uint64_t stream_dropped = 0; // Video frames skipped: by the encoder plus the most any server skipped.
    std::uint64_t stream_bytes = 0; // Sent to all servers.
    // Media queued and not yet sent, at the server furthest behind.
    std::uint32_t stream_queued_ms = 0;
    std::array<StreamStats, kMaxStreams> streams{};
    std::uint32_t stream_count = 0;
    double replay_seconds = 0; // Media the replay buffer holds now.
    std::uint64_t camera_frames = 0; // Frames received from camera overlays.
    // The second canvas: frames composed and encoded, and frames skipped because its encoder
    // fell behind. Its servers follow the main canvas's in streams.
    std::uint64_t canvas2_frames = 0;
    std::uint64_t canvas2_dropped = 0;
    // A source that stopped delivering (unplugged, switched off, or its device failed). Its
    // track stays silent meanwhile, so the recording continues; an audio device is reopened
    // when it, or for the default device another one, is available again.
    bool system_audio_lost = false;
    bool microphone_lost = false;
    bool camera_lost = false;          // A camera overlay stopped; it keeps showing its last frame.
    std::uint32_t audio_reconnects = 0; // Audio devices reopened after a loss.
};

struct Probe {
    std::uint64_t frames = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double duration_seconds = 0;
    std::string codec;
    std::uint32_t audio_tracks = 0;
    std::string audio_codec; // First (default) audio track.
    std::uint64_t audio_buffers = 0;
    double audio_duration_seconds = 0;
    // Flash/beep pairs: a dark-to-bright video frame matched with an audio onset within 500 ms.
    std::uint32_t sync_events = 0;
    double av_offset_ms = 0; // Mean audio minus video; positive means audio is late.
    double av_offset_max_ms = 0; // Largest absolute offset.
    std::vector<double> flashes, beeps;  // Every detected flash and beep, in seconds.
    struct Color {
        std::uint8_t r = 0, g = 0, b = 0;
        double spread = 0; // Standard deviation of full-range luma (0 to 255) across the rectangle.
    };
    // With a sample rectangle: its mean color in the first frame of each second.
    std::vector<Color> samples;
};

// A small copy of the frame as it is encoded, overlays included, for showing while recording.
struct PreviewFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels; // BGRA, top row first, width * 4 bytes a row.
    std::uint64_t number = 0; // Grows with each new frame; 0 before the first.
};

struct Result {
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};

enum class Permission { screen, microphone, camera };
enum class Access { granted, denied, not_determined, restricted };

// What the system allows this process now. Windows asks no permission for the screen, and
// reports the microphone or camera denied when "Let desktop apps access" it is off.
Access permission_status(Permission permission) noexcept;
// Shows the system prompt when the user has not decided yet, and returns at once. For the
// screen, macOS only lists the app in System Settings; the grant applies after a relaunch.
void request_permission(Permission permission);
// Opens the system settings page that grants the permission.
Result open_permission_settings(Permission permission);

// A failure sets error and returns an empty list.
std::vector<Display> list_displays(std::string& error);
// Normal app windows that are on screen and can be captured, excluding rec's own.
std::vector<Window> list_windows(std::string& error);
std::vector<AudioDevice> list_microphones(std::string& error);
std::vector<Camera> list_cameras(std::string& error);
// Time between macOS mixing system audio and the default output device playing it. 0 if unknown.
double default_output_latency_ms() noexcept;
Result probe_file(const std::string& path, Probe& probe, std::optional<Region> sample = std::nullopt);

class Recorder {
public:
    explicit Recorder(Options options);
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    Result start();
    Result stop(); // Finalizes the output before returning.
    // The movie skips paused time: video and audio continue gaplessly after resume().
    Result pause();
    Result resume();
    bool running() const noexcept;
    bool finished() const noexcept; // Duration reached or capture failed.
    Stats stats() const noexcept;
    std::string failure() const;
    // Why stream servers ended early, each named when there are several. The recording
    // continues without them.
    std::string stream_failure() const;
    // Writes the replay buffer to a new MP4 at path while recording continues: the last
    // replay_seconds, from a keyframe, so up to 2 s more. seconds receives the saved length.
    Result save_replay(const std::string& path, double* seconds = nullptr);
    // Shows, hides or moves Options::overlays[index] from the next frame on, while recording.
    Result set_overlay(std::size_t index, bool visible, std::optional<Area> area = std::nullopt);
    // Shows, hides or moves the main capture (Options::screen_area) from the next frame on.
    Result set_screen(bool visible, std::optional<Area> area = std::nullopt);
    // Places a source (an index into Options::overlays, or kScreenLayer) in a canvas
    // (kMainCanvas or kSecondCanvas) from the next frame on.
    Result set_layout(std::size_t canvas, std::size_t layer, const Layout& layout);
    // Changes a camera overlay's shape, border and shadow from the next frame on.
    Result set_style(std::size_t index, const Style& style);
    // Changes a source's volume (-60 to 24 dB) and mute in the program audio while
    // recording, ramped over one audio buffer.
    Result set_volume(AudioInput input, double gain_db, bool muted);
    // Copies the newest preview frame (at most 30 a second, with Options::preview_width set)
    // into frame when it is newer than frame.number. Returns whether frame changed.
    bool preview(PreviewFrame& frame) const;
    // The same for the second canvas, in its shape.
    bool preview(PreviewFrame& frame, std::size_t canvas) const;
    // Stops or resumes making preview frames, for example while the preview is hidden. On
    // at start.
    void show_preview(bool on) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rec
