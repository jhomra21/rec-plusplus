#pragma once

#include "rec/rec.hpp"
#include "core/preview.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace rec::win {

// Composes NV12 frames on the GPU: the main capture in its area, then the image, camera,
// display and window overlays. The recording thread calls draw(); set() and set_screen()
// may come from any thread. Each camera has a reader thread that only replaces one CPU copy
// of its newest frame, and each display or window capture only retains its newest frame;
// draw() uploads what changed.
class Overlays {
public:
    Overlays();
    ~Overlays();
    Overlays(const Overlays&) = delete;
    Overlays& operator=(const Overlays&) = delete;

    // Uses options.overlays, screen_area, screen_visible and cursor.
    Result start(ID3D11Device* device, std::uint32_t width, std::uint32_t height, const Options& options);
    // Draws into frame, whose index in a texture array is subresource (as IMFDXGIBuffer
    // reports it). screen is the main capture: an NV12 frame of the same size that can be
    // sampled per plane.
    bool draw(ID3D11DeviceContext* context, ID3D11Texture2D* screen, ID3D11Texture2D* frame, unsigned subresource);
    Result set(std::size_t index, bool visible, const std::optional<Area>& area);
    Result set_screen(bool visible, const std::optional<Area>& area);
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
