#pragma once

#include "rec/rec.hpp"
#include "core/preview.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace rec::win {

// Where the main capture is on the desktop, for click marks: a monitor (or a region of it in
// its pixels) or a window. HMONITOR and HWND.
struct ClickSource {
    void* monitor = nullptr;
    void* window = nullptr;
    std::optional<Region> region;
};

// Composes NV12 frames on the GPU: the main capture in its area, click marks, then the
// image, camera, display and window overlays. Each canvas (the main one and, with
// options.canvas2, a second one) has its own size and layout of the same sources. The
// recording thread calls draw(); set(), set_screen(), set_layout() and set_style() may come from any thread. Each camera has a reader thread that only replaces
// one CPU copy of its newest frame, and each display or window capture only retains its
// newest frame; draw() uploads what changed. With options.clicks a low-level mouse hook on
// its own thread records presses.
class Overlays {
public:
    Overlays();
    ~Overlays();
    Overlays(const Overlays&) = delete;
    Overlays& operator=(const Overlays&) = delete;

    // Uses options.overlays, screen_area, screen_visible, screen_crop, canvas2, cursor and
    // clicks. width and height are the main canvas's.
    Result start(ID3D11Device* device, std::uint32_t width, std::uint32_t height, const Options& options,
                 const ClickSource& clicks = {});
    // Draws a canvas (kMainCanvas or kSecondCanvas) into frame, an NV12 texture of that
    // canvas's size whose index in a texture array is subresource (as IMFDXGIBuffer reports
    // it). screen is the main capture: an NV12 frame of the main canvas's size that can be
    // sampled per plane. at is the instant the frame shows, for click marks.
    bool draw(std::size_t canvas, ID3D11DeviceContext* context, ID3D11Texture2D* screen, ID3D11Texture2D* frame,
              unsigned subresource, std::chrono::steady_clock::time_point at);
    // The main canvas: shows, hides or moves an overlay or the main capture, keeping its crop.
    Result set(std::size_t index, bool visible, const std::optional<Area>& area);
    Result set_screen(bool visible, const std::optional<Area>& area);
    // layer: an index into options.overlays, or kScreenLayer.
    Result set_layout(std::size_t canvas, std::size_t layer, const Layout& layout);
    // A camera's shape, border and shadow, in both canvases.
    Result set_style(std::size_t index, const Style& style);
    std::uint64_t camera_frames() const;
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Media Foundation video capture devices. The caller has started Media Foundation and COM.
std::vector<Camera> cameras(std::string& error);

// Scales NV12 frames into small BGRA pictures on the GPU for the preview. The recording
// thread calls render() with each frame it wants shown, after the overlays are drawn.
class FramePreview {
public:
    FramePreview();
    ~FramePreview();
    FramePreview(const FramePreview&) = delete;
    FramePreview& operator=(const FramePreview&) = delete;

    // width and height are the frames'; size is the preview's.
    Result start(ID3D11Device* device, std::uint32_t width, std::uint32_t height, core::PreviewSize size);
    // Publishes the picture rendered on the previous call to out.
    void render(ID3D11DeviceContext* context, ID3D11Texture2D* frame, unsigned subresource, core::PreviewBuffer& out);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rec::win
