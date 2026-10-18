#pragma once

#include "rec/rec.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace rec::win {

// Draws image and camera overlays into NV12 frames on the GPU, in place. The recording
// thread calls draw(); set() may come from any thread. Each camera has a reader thread that
// only replaces one CPU copy of its newest frame; draw() uploads it when it changed.
class Overlays {
public:
    Overlays();
    ~Overlays();
    Overlays(const Overlays&) = delete;
    Overlays& operator=(const Overlays&) = delete;

    Result start(ID3D11Device* device, std::uint32_t width, std::uint32_t height, const std::vector<Overlay>& overlays);
    // subresource is the frame's index in a texture array, as IMFDXGIBuffer reports it.
    bool draw(ID3D11DeviceContext* context, ID3D11Texture2D* frame, unsigned subresource);
    Result set(std::size_t index, bool visible, const std::optional<Area>& area);
    std::uint64_t camera_frames() const;
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Media Foundation video capture devices. The caller has started Media Foundation and COM.
std::vector<Camera> cameras(std::string& error);

} // namespace rec::win
