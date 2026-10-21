#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>

#include <d3d11_4.h>
#include <dwmapi.h>
#include <d3dcompiler.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include "platform/windows/displays.hpp"
#include "platform/windows/h264_decoder.hpp"
#include "platform/windows/overlay.hpp"
#include "core/clicks.hpp"
#include "core/overlay.hpp"
#include "core/phone.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace rec::win {
namespace {
using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;

// Overlay pixels are premultiplied BGRA. They are converted to BT.709 studio-range YCbCr
// before blending (ONE, INV_SRC_ALPHA): the conversion is affine, so blending the converted
// values equals converting the blended RGB. Luma and chroma are drawn into the NV12
// frame's two planes in separate passes. The main capture is already NV12; its planes are
// sampled directly when it is drawn smaller than the frame.
constexpr char kShaders[] = R"(
cbuffer Placement : register(b0) {
    float4 dest;    // left, top, right, bottom in clip space
    float4 source;  // u0, v0, u1, v1
    float opaque;   // 1: the texture has no alpha (a camera)
    float fade;     // Opacity of a picture with alpha.
    float mode;     // 0: the picture, 1: a solid fill (a border), 2: a drop shadow
    float shape;    // Style::Shape, or -1 for none
    float4 box;     // Pixels from the shape box's top-left at the quad's left, top, right, bottom
    float4 size;    // The shape box's width and height, its roundness (a squircle's exponent), the shadow's reach
    float4 fill;    // A border's color
    float4 color0;
    float4 color1;
    float4 color2;
    float4 color_offset;
    float color_active;
};
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; float2 local : TEXCOORD1; };
Vertex vs(uint id : SV_VertexID) {
    float2 t = float2(id & 1, id >> 1);
    Vertex v;
    v.position = float4(lerp(dest.x, dest.z, t.x), lerp(dest.y, dest.w, t.y), 0, 1);
    v.uv = float2(lerp(source.x, source.z, t.x), lerp(source.y, source.w, t.y));
    v.local = float2(lerp(box.x, box.z, t.x), lerp(box.y, box.w, t.y));
    return v;
}
Texture2D picture : register(t0);
SamplerState smooth : register(s0);
// core::shape_distance: signed distance in pixels (negative inside) to the shape's edge.
float edge(float2 p) {
    float2 hs = size.xy / 2;
    float least = min(hs.x, hs.y);
    float2 a = abs(p - hs);
    int s = (int)(shape + 0.5);
    if (s == 5) {
        float n = size.z;
        return (pow(pow(a.x / hs.x, n) + pow(a.y / hs.y, n), 1 / n) - 1) * least;
    }
    if (s == 2) return (length(a / hs) - 1) * least;
    float r = s == 4 ? least : s == 0 ? 0 : size.z * least;
    float2 q = a - (hs - r);
    return length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
}
float4 color(Vertex v) {
    float cover = 1;
    if (shape >= 0) {
        float d = edge(v.local);
        cover = mode > 1.5 ? 0.45 * (1 - smoothstep(-size.w, size.w, d)) : saturate(0.5 - d);
    }
    if (mode > 1.5) return float4(0, 0, 0, cover);
    if (mode > 0.5) return float4(fill.rgb, 1) * cover;
    float4 c = picture.Sample(smooth, v.uv);
    return (opaque > 0.5 ? float4(c.rgb, 1) : c * fade) * cover;
}
float4 adjusted_color(Vertex v) {
    float cover = 1;
    if (shape >= 0) {
        float d = edge(v.local);
        cover = mode > 1.5 ? 0.45 * (1 - smoothstep(-size.w, size.w, d)) : saturate(0.5 - d);
    }
    if (mode > 1.5) return float4(0, 0, 0, cover);
    if (mode > 0.5) return float4(fill.rgb, 1) * cover;
    float4 c = picture.Sample(smooth, v.uv);
    float4 result = opaque > 0.5 ? float4(c.rgb, 1) : c * fade;
    result.rgb = saturate(float3(dot(result.rgb, color0.xyz), dot(result.rgb, color1.xyz), dot(result.rgb, color2.xyz)) + color_offset.xyz);
    return result * cover;
}
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
float4 luma(Vertex v) : SV_Target {
    float4 c = color(v);
    return float4(c.a * 16.0 / 255 + 219.0 / 255 * dot(c.rgb, kLuma), 0, 0, c.a);
}
float4 chroma(Vertex v) : SV_Target {
    float4 c = color(v);
    float y = dot(c.rgb, kLuma);
    float2 uv = float2((c.b - y) / 1.8556, (c.r - y) / 1.5748);
    return float4(c.a * 128.0 / 255 + 224.0 / 255 * uv, 0, c.a);
}
float4 adjusted_luma(Vertex v) : SV_Target {
    float4 c = adjusted_color(v);
    return float4(c.a * 16.0 / 255 + 219.0 / 255 * dot(c.rgb, kLuma), 0, 0, c.a);
}
float4 adjusted_chroma(Vertex v) : SV_Target {
    float4 c = adjusted_color(v);
    float y = dot(c.rgb, kLuma);
    float2 uv = float2((c.b - y) / 1.8556, (c.r - y) / 1.5748);
    return float4(c.a * 128.0 / 255 + 224.0 / 255 * uv, 0, c.a);
}
Texture2D<float> plane_y : register(t1);
Texture2D<float2> plane_uv : register(t2);
float4 screen_luma(Vertex v) : SV_Target { return float4(plane_y.Sample(smooth, v.uv), 0, 0, 1); }
float4 screen_chroma(Vertex v) : SV_Target { return float4(plane_uv.Sample(smooth, v.uv), 0, 1); }
float3 adjusted_screen_rgb(Vertex v) {
    float y = (plane_y.Sample(smooth, v.uv).r - 16.0 / 255) * 255.0 / 219;
    float2 c = (plane_uv.Sample(smooth, v.uv).rg - 128.0 / 255) * 255.0 / 224;
    float3 rgb = saturate(float3(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x));
    return saturate(float3(dot(rgb, color0.xyz), dot(rgb, color1.xyz), dot(rgb, color2.xyz)) + color_offset.xyz);
}
float4 adjusted_screen_luma(Vertex v) : SV_Target {
    float3 c = adjusted_screen_rgb(v);
    return float4(16.0 / 255 + 219.0 / 255 * dot(c, kLuma), 0, 0, 1);
}
float4 adjusted_screen_chroma(Vertex v) : SV_Target {
    float3 c = adjusted_screen_rgb(v); float y = dot(c, kLuma);
    float2 uv = float2((c.b - y) / 1.8556, (c.r - y) / 1.5748);
    return float4(128.0 / 255 + 224.0 / 255 * uv, 0, 1);
}
)";

struct Constants {
    float dest[4];
    float source[4];
    float opaque;
    float fade;
    float mode;
    float shape;
    float box[4];
    float size[4];
    float fill[4];
    float color0[4], color1[4], color2[4], color_offset[4];
    float color_active;
    float padding[3];
};

void color_constants(const ColorAdjust& c, Constants& out) {
    if (!c.brightness && !c.contrast && !c.saturation) return;
    out.color_active = 1;
    const float saturation = 1.0f + c.saturation / 100.0f;
    const float contrast = 1.0f + c.contrast / 100.0f;
    const float luma[3] = {0.2126f, 0.7152f, 0.0722f};
    float* rows[3] = {out.color0, out.color1, out.color2};
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
            rows[r][col] = contrast * (saturation * (r == col ? 1.0f : 0.0f) + (1.0f - saturation) * luma[col]);
    const float offset = 0.5f * (1.0f - contrast) + c.brightness / 100.0f;
    out.color_offset[0] = out.color_offset[1] = out.color_offset[2] = offset;
}

// How a quad is cut to a shape; see the shader's Placement.
struct Shaping {
    float mode = 0, shape = -1;
    float box[4] = {};
    float size[4] = {};
    float fill[4] = {};
};

std::string failed(const char* what, HRESULT hr) {
    char code[16];
    std::snprintf(code, sizeof code, "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(what) + " (" + code + ")";
}

std::wstring wide(const std::string& text) {
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (chars <= 1) return {};
    std::wstring out(static_cast<std::size_t>(chars - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), chars);
    return out;
}

std::string utf8(const wchar_t* text) {
    if (!text) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string out(static_cast<std::size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string device_string(IMFActivate* device, const GUID& key) {
    WCHAR* value = nullptr;
    UINT32 length = 0;
    if (FAILED(device->GetAllocatedString(key, &value, &length))) return {};
    auto text = utf8(value);
    CoTaskMemFree(value);
    return text;
}

struct DeviceList {
    IMFActivate** items = nullptr;
    UINT32 count = 0;
    ~DeviceList() {
        for (UINT32 i = 0; i < count; ++i) items[i]->Release();
        CoTaskMemFree(items);
    }
};

HRESULT enumerate(DeviceList& list) {
    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr)) hr = MFEnumDeviceSources(attributes.Get(), &list.items, &list.count);
    return hr;
}

// The newest frame of one camera, BGRA, top-down. A device camera reads on its own thread
// through a Media Foundation source reader that converts to RGB32; the test camera draws
// core::test_camera_frame at 30 fps; a phone ("phone:TOKEN") is decoded on its own thread
// and has no size until its first picture.
class CameraFeed {
public:
    ~CameraFeed() { stop(); }

    Result open(const std::string& wanted) {
        if (wanted.rfind("phone:", 0) == 0) {
            std::string error;
            phone_ = core::PhoneCamera::open(wanted.substr(6), error);
            if (!phone_) return {error};
            name_ = "phone";
            return {};
        }
        if (wanted == "test") {
            width_ = core::kTestCameraWidth;
            height_ = core::kTestCameraHeight;
            pixels_.assign(static_cast<std::size_t>(width_) * height_ * 4, 0);
            return {};
        }
        DeviceList list;
        HRESULT hr = enumerate(list);
        if (FAILED(hr)) return {failed("could not list cameras", hr)};
        if (!list.count) return {"no camera found"};
        IMFActivate* chosen = nullptr;
        for (UINT32 i = 0; i < list.count && !chosen; ++i) {
            const auto id = device_string(list.items[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
            const auto name = device_string(list.items[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
            if (wanted == "default" || wanted == id || lower(name).find(lower(wanted)) != std::string::npos) {
                chosen = list.items[i];
                name_ = name;
            }
        }
        if (!chosen) return {"camera not found: " + wanted + "; run `rec devices`"};
        hr = chosen->ActivateObject(IID_PPV_ARGS(&source_));
        if (hr == E_ACCESSDENIED)
            return {"camera access is off: allow desktop apps in Settings > Privacy & security > Camera"};
        if (FAILED(hr)) return {failed(("could not open the camera " + name_).c_str(), hr)};
        ComPtr<IMFAttributes> attributes;
        MFCreateAttributes(&attributes, 1);
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        hr = MFCreateSourceReaderFromMediaSource(source_.Get(), attributes.Get(), &reader_);
        if (FAILED(hr)) return {failed("could not read the camera", hr)};

        // The largest mode up to 1080p, preferring 30 fps or more.
        constexpr DWORD video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        ComPtr<IMFMediaType> best;
        std::uint64_t best_score = 0;
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> type;
            if (FAILED(reader_->GetNativeMediaType(video, i, &type))) break;
            UINT32 w = 0, h = 0, num = 0, den = 0;
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den);
            if (w > 1920 || h > 1080 || !w || !h) continue;
            const double fps = den ? static_cast<double>(num) / den : 0;
            const std::uint64_t score = (fps >= 29 ? 1ull << 40 : 0) + static_cast<std::uint64_t>(w) * h * 64 +
                                        static_cast<std::uint64_t>(std::min(fps, 63.0));
            if (score > best_score) { best_score = score; best = type; }
        }
        if (best) reader_->SetCurrentMediaType(video, nullptr, best.Get());
        ComPtr<IMFMediaType> rgb;
        MFCreateMediaType(&rgb);
        rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = reader_->SetCurrentMediaType(video, nullptr, rgb.Get());
        if (FAILED(hr)) return {failed(("the camera " + name_ + " has no usable video format").c_str(), hr)};
        ComPtr<IMFMediaType> current;
        reader_->GetCurrentMediaType(video, &current);
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h);
        if (w < 2 || h < 2) return {"the camera " + name_ + " reported no frame size"};
        width_ = w;
        height_ = h;
        UINT32 stride = 0;
        if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) stride_ = static_cast<LONG>(stride);
        else {
            LONG computed = 0;
            MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &computed);
            stride_ = computed ? computed : static_cast<LONG>(w * 4);
        }
        pixels_.assign(static_cast<std::size_t>(width_) * height_ * 4, 0);
        return {};
    }

    void start() {
        stop_ = false;
        if (phone_) {
            phone_->set_sink([this](const std::uint8_t* data, std::size_t size, bool key) {
                {
                    std::lock_guard lock(units_mutex_);
                    // The decoder fell behind by two seconds: start again at the next keyframe.
                    if (units_.size() >= 60) {
                        units_.clear();
                        skip_to_key_ = true;
                    }
                    if (skip_to_key_ && !key) { phone_->request_keyframe(); return; }
                    skip_to_key_ = false;
                    units_.emplace_back(data, data + size);
                }
                units_ready_.notify_one();
            });
        }
        thread_ = std::thread([this] { phone_ ? decode() : reader_ ? read() : generate(); });
    }

    void stop() {
        if (phone_) phone_->set_sink(nullptr);
        stop_ = true;
        units_ready_.notify_all();
        if (thread_.joinable()) thread_.join();
        phone_.reset();
        reader_.Reset();
        if (source_) { source_->Shutdown(); source_.Reset(); }
    }

    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::uint64_t frames() const { return frames_.load(); }
    bool lost() const { return lost_.load(); }

    // Copies the newest frame to a mapped texture when it is newer than seen: map(pixels, row
    // bytes, width, height).
    template <class Map> bool upload(std::uint64_t& seen, Map&& map) {
        if (frames_.load() == seen) return false;
        std::lock_guard lock(mutex_);
        seen = frames_.load();
        if (!width_ || !height_) return false;
        return map(pixels_.data(), static_cast<std::size_t>(width_) * 4, width_, height_);
    }

private:
    void generate() {
        std::vector<std::uint8_t> frame(pixels_.size());
        const auto begin = std::chrono::steady_clock::now();
        // REC_E2E_LOSE_CAMERA=MS stops the test camera as an unplug would.
        char* lose = nullptr;
        std::size_t size = 0;
        std::uint64_t lose_frame = UINT64_MAX;
        if (!_dupenv_s(&lose, &size, "REC_E2E_LOSE_CAMERA") && lose) {
            lose_frame = std::strtoull(lose, nullptr, 10) * core::kTestCameraFps / 1000;
            std::free(lose);
        }
        for (std::uint64_t n = 0; !stop_; ++n) {
            if (n == lose_frame) { lost_.store(true); return; }
            core::test_camera_frame(n, frame.data(), static_cast<std::size_t>(width_) * 4);
            {
                std::lock_guard lock(mutex_);
                pixels_.swap(frame);
                frames_.fetch_add(1);
            }
            std::this_thread::sleep_until(begin + std::chrono::microseconds((n + 1) * 1000000 / core::kTestCameraFps));
        }
    }

    void decode() {
        H264Decoder decoder;
        if (auto error = decoder.start(); !error.empty()) {
            lost_.store(true);
            return;
        }
        const auto picture = [this](const std::uint8_t* bgra, std::uint32_t w, std::uint32_t h) {
            std::lock_guard lock(mutex_);
            width_ = w;
            height_ = h;
            pixels_.assign(bgra, bgra + static_cast<std::size_t>(w) * h * 4);
            frames_.fetch_add(1);
        };
        for (;;) {
            std::vector<std::uint8_t> unit;
            {
                std::unique_lock lock(units_mutex_);
                units_ready_.wait(lock, [this] { return stop_.load() || !units_.empty(); });
                if (stop_) return;
                unit = std::move(units_.front());
                units_.pop_front();
            }
            if (!decoder.decode(unit.data(), unit.size(), picture)) phone_->request_keyframe();
        }
    }

    void read() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const std::size_t row = static_cast<std::size_t>(width_) * 4;
        while (!stop_) {
            DWORD stream = 0, flags = 0;
            LONGLONG time = 0;
            ComPtr<IMFSample> sample;
            const HRESULT hr = reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream,
                                                   &flags, &time, &sample);
            // An unplugged camera ends here; its last frame stays on screen.
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
                lost_.store(true);
                break;
            }
            if (!sample) continue;
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->GetBufferByIndex(0, &buffer))) continue;
            ComPtr<IMF2DBuffer> planar;
            BYTE* scan = nullptr;
            LONG pitch = 0;
            bool locked2d = SUCCEEDED(buffer.As(&planar)) && SUCCEEDED(planar->Lock2D(&scan, &pitch));
            BYTE* data = nullptr;
            DWORD length = 0;
            if (!locked2d) {
                if (FAILED(buffer->Lock(&data, nullptr, &length))) continue;
                pitch = stride_;
                const std::size_t needed = static_cast<std::size_t>(std::abs(pitch)) * height_;
                if (length < needed || static_cast<std::size_t>(std::abs(pitch)) < row) { buffer->Unlock(); continue; }
                scan = pitch < 0 ? data + static_cast<std::size_t>(-pitch) * (height_ - 1) : data;
            }
            {
                std::lock_guard lock(mutex_);
                for (std::uint32_t y = 0; y < height_; ++y)
                    std::memcpy(pixels_.data() + y * row, scan + static_cast<std::ptrdiff_t>(y) * pitch, row);
                frames_.fetch_add(1);
            }
            if (locked2d) planar->Unlock2D();
            else buffer->Unlock();
        }
        CoUninitialize();
    }

    std::string name_;
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFSourceReader> reader_;
    std::uint32_t width_ = 0, height_ = 0;
    LONG stride_ = 0;
    std::mutex mutex_;
    std::vector<std::uint8_t> pixels_;
    std::atomic<std::uint64_t> frames_{0};
    std::atomic_bool stop_{false}, lost_{false};
    std::thread thread_;
    std::shared_ptr<core::PhoneCamera> phone_;
    std::mutex units_mutex_;
    std::condition_variable units_ready_;
    std::deque<std::vector<std::uint8_t>> units_;  // Access units waiting for the decoder.
    bool skip_to_key_ = false;
};

// The newest frame of a display or window, captured with Windows.Graphics.Capture on the
// recording device. The callback only retains the frame (replacing the older one, which
// returns to the two-buffer pool); draw() copies it into the layer's texture and closes it.
class ScreenFeed {
public:
    ~ScreenFeed() { stop(); }

    Result open(const wgdx::Direct3D11::IDirect3DDevice& device, const Overlay& overlay, bool cursor) {
        device_ = device;
        const bool window = overlay.kind == Overlay::Kind::window;
        const auto name = window ? std::string("the window") : "display " + std::to_string(overlay.display);
        try {
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            if (window) {
                const auto handle = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(overlay.window));
                if (!IsWindow(handle)) return {"window not found; run `rec windows`"};
                if (IsIconic(handle)) return {"the window is minimized; restore it, then retry"};
                winrt::check_hresult(interop->CreateForWindow(handle, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item_)));
            } else {
                const auto list = monitors();
                if (overlay.display > list.size()) return {name + " not found; run `rec devices`"};
                winrt::check_hresult(interop->CreateForMonitor(list[overlay.display - 1].handle, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                               winrt::put_abi(item_)));
            }
        } catch (winrt::hresult_error const& e) {
            return {failed(("could not capture " + name).c_str(), e.code())};
        }
        pool_size_ = item_.Size();
        if (pool_size_.Width < 2 || pool_size_.Height < 2) return {name + " has no usable size"};
        if (overlay.region) {
            const auto& r = *overlay.region;
            if (r.x + static_cast<std::int64_t>(r.width) > pool_size_.Width || r.y + static_cast<std::int64_t>(r.height) > pool_size_.Height)
                return {"the region does not fit " + name + " (" + std::to_string(pool_size_.Width) + "x" +
                        std::to_string(pool_size_.Height) + ")"};
            region_ = r;
        }
        try {
            try {
                wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get();
            } catch (...) {
            }
            pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(device_, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, pool_size_);
            accepting_ = true;
            token_ = pool_.FrameArrived({this, &ScreenFeed::on_frame});
            session_ = pool_.CreateCaptureSession(item_);
            try { session_.IsCursorCaptureEnabled(cursor); } catch (...) {}
            try { session_.IsBorderRequired(false); } catch (...) {}
        } catch (winrt::hresult_error const& e) {
            return {failed(("could not capture " + name).c_str(), e.code())};
        }
        return {};
    }

    Result start() {
        try {
            session_.StartCapture();
        } catch (winrt::hresult_error const& e) {
            return {failed("could not start a screen capture", e.code())};
        }
        return {};
    }

    void stop() {
        wgc::Direct3D11CaptureFrame held{nullptr};
        {
            std::lock_guard lock(mutex_);
            accepting_ = false;
            held = std::exchange(latest_, nullptr);
        }
        try {
            if (held) held.Close();
            if (pool_) pool_.FrameArrived(token_);
            if (session_) session_.Close();
            if (pool_) pool_.Close();
        } catch (...) {
        }
        session_ = nullptr;
        pool_ = nullptr;
        item_ = nullptr;
    }

    // The newest frame, when one arrived since the last call. The caller closes it.
    wgc::Direct3D11CaptureFrame take() {
        std::lock_guard lock(mutex_);
        return std::exchange(latest_, nullptr);
    }

    const std::optional<Region>& region() const { return region_; }

private:
    void on_frame(wgc::Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
        try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;
            const auto content = frame.ContentSize();
            wgc::Direct3D11CaptureFrame replaced{nullptr};
            bool resized = false;
            {
                std::lock_guard lock(mutex_);
                if (!accepting_) { frame.Close(); return; }
                resized = content.Width != pool_size_.Width || content.Height != pool_size_.Height;
                // A resized pool starts from the next frame, so none from the old one is held.
                if (resized) {
                    pool_size_ = content;
                    replaced = std::exchange(latest_, nullptr);
                } else {
                    replaced = std::exchange(latest_, frame);
                }
            }
            if (replaced) replaced.Close();
            if (resized) {
                frame.Close();
                if (content.Width >= 2 && content.Height >= 2)
                    sender.Recreate(device_, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, content);
            }
        } catch (...) {
        }
    }

    wgdx::Direct3D11::IDirect3DDevice device_{nullptr};
    wgc::GraphicsCaptureItem item_{nullptr};
    wgc::Direct3D11CaptureFramePool pool_{nullptr};
    wgc::GraphicsCaptureSession session_{nullptr};
    winrt::event_token token_{};
    std::optional<Region> region_;
    std::mutex mutex_;  // Guards accepting_, latest_ and pool_size_.
    bool accepting_ = false;
    wgc::Direct3D11CaptureFrame latest_{nullptr};
    winrt::Windows::Graphics::SizeInt32 pool_size_{};
};
Result load_image(ID3D11Device* device, const std::string& path, ComPtr<ID3D11Texture2D>& texture, UINT& width, UINT& height) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return {failed("the Windows imaging component is unavailable", hr)};
    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(wide(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return {failed(("could not open the image " + path).c_str(), hr)};
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = converter->GetSize(&width, &height);
    if (FAILED(hr)) return {failed(("could not decode the image " + path).c_str(), hr)};
    if (width < 1 || height < 1 || width > 8192 || height > 8192) return {"the image " + path + " must be at most 8192x8192"};
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr)) return {failed(("could not decode the image " + path).c_str(), hr)};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{pixels.data(), width * 4, 0};
    hr = device->CreateTexture2D(&desc, &data, &texture);
    if (FAILED(hr)) return {failed("could not upload the image", hr)};
    return {};
}
} // namespace

std::vector<Camera> cameras(std::string& error) {
    error.clear();
    std::vector<Camera> result;
    DeviceList list;
    const HRESULT hr = enumerate(list);
    if (FAILED(hr)) { error = failed("could not list cameras", hr); return result; }
    for (UINT32 i = 0; i < list.count; ++i)
        result.push_back({device_string(list.items[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK),
                          device_string(list.items[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME)});
    return result;
}

std::vector<Monitor> monitors() {
    std::vector<Monitor> list;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR handle, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFOEXW info{};
        info.cbSize = sizeof info;
        if (!GetMonitorInfoW(handle, &info)) return TRUE;
        DEVMODEW mode{};
        mode.dmSize = sizeof mode;
        Monitor monitor;
        monitor.handle = handle;
        monitor.device = utf8(info.szDevice);
        monitor.primary = info.dwFlags & MONITORINFOF_PRIMARY;
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
            monitor.width = mode.dmPelsWidth;
            monitor.height = mode.dmPelsHeight;
        } else {
            monitor.width = static_cast<std::uint32_t>(info.rcMonitor.right - info.rcMonitor.left);
            monitor.height = static_cast<std::uint32_t>(info.rcMonitor.bottom - info.rcMonitor.top);
        }
        reinterpret_cast<std::vector<Monitor>*>(data)->push_back(monitor);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&list));
    std::stable_partition(list.begin(), list.end(), [](const Monitor& m) { return m.primary; });
    return list;
}

thread_local core::Clicks* t_clicks = nullptr;

LRESULT CALLBACK mouse_hook(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && t_clicks) {
        const auto& m = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        const auto now = core::Clicks::Clock::now();
        switch (message) {
        case WM_LBUTTONDOWN: t_clicks->press(now, m.pt.x, m.pt.y, false); break;
        case WM_RBUTTONDOWN: t_clicks->press(now, m.pt.x, m.pt.y, true); break;
        case WM_LBUTTONUP: t_clicks->release(now, false); break;
        case WM_RBUTTONUP: t_clicks->release(now, true); break;
        default: break;
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

// Records mouse presses into clicks. A low-level hook runs on the thread that set it, which
// must pump messages; the hook only stores the press, so it never holds up the pointer.
class ClickWatch {
public:
    ~ClickWatch() { stop(); }

    Result start(core::Clicks& clicks) {
        std::promise<DWORD> ready;
        auto started = ready.get_future();
        thread_ = std::thread([&clicks, &ready] {
            MSG message;
            PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // Creates the queue.
            t_clicks = &clicks;
            HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, GetModuleHandleW(nullptr), 0);
            ready.set_value(hook ? GetCurrentThreadId() : 0);
            if (!hook) return;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            }
            UnhookWindowsHookEx(hook);
        });
        thread_id_ = started.get();
        if (!thread_id_) {
            thread_.join();
            return {"could not watch mouse clicks"};
        }
        return {};
    }

    void stop() {
        if (!thread_.joinable()) return;
        PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
        thread_.join();
    }

private:
    std::thread thread_;
    DWORD thread_id_ = 0;
};

class Overlays::Impl {
public:
    struct Layer {
        Overlay overlay;
        UINT width = 0, height = 0;  // The picture shown: a screen layer's crop.
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        std::unique_ptr<CameraFeed> camera;
        std::unique_ptr<ScreenFeed> screen;
        double crop[4] = {0, 0, 1, 1};  // A screen layer's picture in its texture: u0, v0, u1, v1.
        std::uint64_t seen = 0;
        bool ready = false;  // Has a picture to draw.
    };
    struct Target {
        ComPtr<ID3D11Texture2D> texture;
        unsigned subresource = 0;
        ComPtr<ID3D11RenderTargetView> luma, chroma;
    };
    struct Planes {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> luma, chroma;
    };
    // A canvas's size and where it shows each source.
    struct Canvas {
        std::uint32_t width = 0, height = 0;
        Layout screen;
        std::vector<Layout> layers;  // One per layers_.
    };

    Result start(ID3D11Device* device, std::uint32_t width, std::uint32_t height, const Options& options,
                 const ClickSource& clicks) {
        if (auto error = core::check_overlays(options.overlays); !error.empty()) return {error};
        if (auto error = core::check_color(options.screen_color); !error.empty()) return {error};
        if (auto error = core::check_color(options.camera_color); !error.empty()) return {error};
        screen_color_ = options.screen_color;
        camera_color_ = options.camera_color;
        if (!core::valid(options.screen_area)) return {"the screen area must lie inside the frame: x, y, width and height are fractions of it"};
        if (auto error = core::check_crop("the screen", options.screen_crop); !error.empty()) return {error};
        if (auto error = core::check_canvas(options); !error.empty()) return {error};
        device_ = device;
        width_ = width;
        height_ = height;
        auto& main = canvases_[kMainCanvas];
        main = {width, height, {options.screen_visible, options.screen_area, options.screen_crop}, {}};
        for (const auto& overlay : options.overlays) main.layers.push_back({overlay.visible, overlay.area, overlay.crop});
        canvases_[kSecondCanvas] = {};
        if (options.canvas2.width) {
            auto& second = canvases_[kSecondCanvas];
            second = {options.canvas2.width, options.canvas2.height, options.canvas2.screen, {}};
            for (std::size_t i = 0; i < options.overlays.size(); ++i) second.layers.push_back(core::canvas_layout(options, i));
        }
        UINT support = 0;
        if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_NV12, &support)) || !(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
            return {"this graphics driver cannot draw overlays into video frames"};
        if (auto made = create_pipeline(); !made) return made;
        if (options.clicks) {
            click_source_ = clicks;
            for (int right = 0; right < 2; ++right) {
                constexpr UINT size = 128;
                const auto pixels = core::click_mark_pixels(size, right);
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = desc.Height = size;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_IMMUTABLE;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                const D3D11_SUBRESOURCE_DATA data{pixels.data(), size * 4, 0};
                ComPtr<ID3D11Texture2D> texture;
                HRESULT hr = device->CreateTexture2D(&desc, &data, &texture);
                if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(texture.Get(), nullptr, &click_marks_[right]);
                if (FAILED(hr)) return {failed("could not make the click marks", hr)};
            }
            clicks_ = std::make_unique<core::Clicks>();
            watch_ = std::make_unique<ClickWatch>();
            if (auto watching = watch_->start(*clicks_); !watching) return watching;
        }
        for (const auto& overlay : options.overlays) {
            Layer layer;
            layer.overlay = overlay;
            switch (overlay.kind) {
            case Overlay::Kind::image:
                if (auto loaded = load_image(device, overlay.source, layer.texture, layer.width, layer.height); !loaded)
                    return loaded;
                layer.ready = true;
                break;
            case Overlay::Kind::camera: {
                layer.camera = std::make_unique<CameraFeed>();
                if (auto opened = layer.camera->open(overlay.source); !opened) return opened;
                if (layer.camera->width() && layer.camera->height())
                    if (auto made = camera_texture(layer, layer.camera->width(), layer.camera->height()); !made) return made;
                break;
            }
            case Overlay::Kind::display:
            case Overlay::Kind::window:
                if (auto ready = capture_device(); !ready) return ready;
                layer.screen = std::make_unique<ScreenFeed>();
                if (auto opened = layer.screen->open(winrt_device_, overlay, options.cursor); !opened) return opened;
                break;
            }
            if (layer.texture && !layer.view) {
                const HRESULT hr = device->CreateShaderResourceView(layer.texture.Get(), nullptr, &layer.view);
                if (FAILED(hr)) return {failed("could not use an overlay picture", hr)};
            }
            layers_.push_back(std::move(layer));
        }
        for (auto& layer : layers_) {
            if (layer.camera) layer.camera->start();
            if (layer.screen)
                if (auto started = layer.screen->start(); !started) return started;
        }
        return {};
    }

    Result set(std::size_t index, bool visible, const std::optional<Area>& area) {
        std::lock_guard lock(mutex_);
        auto& layers = canvases_[kMainCanvas].layers;
        if (index >= layers.size()) return {"no overlay " + std::to_string(index + 1)};
        if (area) {
            if (auto error = core::check_area(index, *area); !error.empty()) return {error};
            layers[index].area = *area;
        }
        layers[index].visible = visible;
        return {};
    }

    Result set_screen(bool visible, const std::optional<Area>& area) {
        if (area && !core::valid(*area)) return {"the screen area must lie inside the frame: x, y, width and height are fractions of it"};
        std::lock_guard lock(mutex_);
        auto& screen = canvases_[kMainCanvas].screen;
        if (area) screen.area = *area;
        screen.visible = visible;
        return {};
    }

    Result set_layout(std::size_t canvas, std::size_t layer, const Layout& layout) {
        if (auto error = core::check_layout(layer, layout); !error.empty()) return {error};
        std::lock_guard lock(mutex_);
        if (canvas > kSecondCanvas || !canvases_[canvas].width) return {"this recording has no second canvas"};
        auto& c = canvases_[canvas];
        if (layer == kScreenLayer) c.screen = layout;
        else if (layer < c.layers.size()) c.layers[layer] = layout;
        else return {"no overlay " + std::to_string(layer + 1)};
        return {};
    }

    Result set_style(std::size_t index, const Style& style) {
        if (auto error = core::check_style(style); !error.empty()) return {error};
        std::lock_guard lock(mutex_);
        if (index >= layers_.size()) return {"no overlay " + std::to_string(index + 1)};
        if (layers_[index].overlay.kind != Overlay::Kind::camera) return {"shapes, borders and shadows are for a camera"};
        layers_[index].overlay.style = style;
        return {};
    }

    Result set_color(ColorTarget target, const ColorAdjust& color) {
        if (auto error = core::check_color(color); !error.empty()) return {error};
        std::lock_guard lock(mutex_);
        (target == ColorTarget::screen ? screen_color_ : camera_color_) = color;
        return {};
    }

    bool needs_composition(std::size_t canvas) const {
        std::lock_guard lock(mutex_);
        if (canvas > kSecondCanvas || !canvases_[canvas].width) return false;
        const auto& c = canvases_[canvas];
        const bool screen_adjusted = screen_color_.brightness || screen_color_.contrast || screen_color_.saturation;
        const auto full = [](const Area& a) { return a.x <= 0 && a.y <= 0 && a.width >= 1 && a.height >= 1; };
        if (clicks_ || !c.screen.visible ||
            (c.screen.visible && (!full(c.screen.area) || !full(c.screen.crop) || screen_adjusted)) ||
            (canvas != kMainCanvas && c.screen.visible && (c.width != width_ || c.height != height_))) return true;
        for (std::size_t i = 0; i < layers_.size(); ++i) {
            if (!c.layers[i].visible) continue;
            const auto kind = layers_[i].overlay.kind;
            if (kind == Overlay::Kind::camera)
                return true;
            if (kind == Overlay::Kind::display || kind == Overlay::Kind::window)
                return true;
            if (kind == Overlay::Kind::image)
                return true;
        }
        return false;
    }

    std::uint64_t camera_frames() const {
        std::lock_guard lock(mutex_);
        std::uint64_t total = 0;
        for (const auto& layer : layers_)
            if (layer.camera) total += layer.camera->frames();
        return total;
    }

    bool camera_lost() const {
        std::lock_guard lock(mutex_);
        return std::any_of(layers_.begin(), layers_.end(), [](const auto& layer) { return layer.camera && layer.camera->lost(); });
    }

    bool draw(std::size_t canvas, ID3D11DeviceContext* context, ID3D11Texture2D* screen, ID3D11Texture2D* frame,
              unsigned subresource, std::chrono::steady_clock::time_point at) {
        struct Job { Layer* layer; Layout layout; Style style; ColorAdjust color; };
        Job jobs[core::kMaxOverlays];
        std::size_t count = 0;
        Layout shown;
        ColorAdjust screen_adjustment;
        {
            std::lock_guard lock(mutex_);
            if (canvas > kSecondCanvas || !canvases_[canvas].width) return false;
            const auto& c = canvases_[canvas];
            for (std::size_t i = 0; i < layers_.size(); ++i)
                if (c.layers[i].visible) {
                    const auto kind = layers_[i].overlay.kind;
                    const ColorAdjust adjustment = kind == Overlay::Kind::camera ? camera_color_ :
                        (kind == Overlay::Kind::display || kind == Overlay::Kind::window) ? screen_color_ : ColorAdjust{};
                    jobs[count++] = {&layers_[i], c.layers[i], layers_[i].overlay.style, adjustment};
                }
            shown = c.screen;
            screen_adjustment = screen_color_;
            draw_width_ = c.width;
            draw_height_ = c.height;
        }
        std::array<core::ClickMark, core::Clicks::kMax> marks;
        const std::size_t mark_count = clicks_ && shown.visible
            ? clicks_->marks(at, click_area(), width_, height_, draw_width_, draw_height_, shown.area, shown.crop, marks) : 0;
        const auto full = [](const Area& a) { return a.x <= 0 && a.y <= 0 && a.width >= 1 && a.height >= 1; };
        const bool whole = canvas == kMainCanvas && shown.visible && full(shown.area) && full(shown.crop) &&
                           !screen_adjustment.brightness && !screen_adjustment.contrast && !screen_adjustment.saturation;
        if (whole) context->CopySubresourceRegion(frame, subresource, 0, 0, 0, screen, 0, nullptr);
        if (whole && !count && !mark_count) return true;
        // Before taking the device lock: closing a capture frame takes its frame pool's lock,
        // and the pool's present thread holds that while it waits for the device lock.
        for (std::size_t i = 0; i < count; ++i)
            if (jobs[i].layer->screen) upload_screen(context, *jobs[i].layer);
        ComPtr<ID3D11Multithread> multithread;
        context->QueryInterface(IID_PPV_ARGS(&multithread));
        if (multithread) multithread->Enter();
        bool drawn = false;
        if (Target* target = target_for(frame, subresource)) {
            begin(context);
            drawn = true;
            if (!whole) {
                const float black[4] = {16.0f / 255, 0, 0, 0}, neutral[4] = {128.0f / 255, 128.0f / 255, 0, 0};
                context->ClearRenderTargetView(target->luma.Get(), black);
                context->ClearRenderTargetView(target->chroma.Get(), neutral);
                if (shown.visible) drawn = draw_screen(context, *target, screen, shown, screen_adjustment);
            }
            for (std::size_t i = 0; i < mark_count && drawn; ++i) draw_mark(context, *target, marks[i]);
            for (std::size_t i = 0; i < count && drawn; ++i)
                draw_layer(context, *target, *jobs[i].layer, jobs[i].layout, jobs[i].style, jobs[i].color);
            end(context);
        }
        if (multithread) multithread->Leave();
        return drawn;
    }

    void stop() {
        watch_.reset();
        std::lock_guard lock(mutex_);
        for (auto& layer : layers_) {
            if (layer.camera) layer.camera->stop();
            if (layer.screen) layer.screen->stop();
        }
        layers_.clear();
        targets_.clear();
        planes_.clear();
    }

private:
    Result capture_device() {
        if (winrt_device_) return {};
        try {
            ComPtr<IDXGIDevice> dxgi;
            device_.As(&dxgi);
            winrt::com_ptr<::IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
            winrt_device_ = inspectable.as<wgdx::Direct3D11::IDirect3DDevice>();
        } catch (winrt::hresult_error const& e) {
            return {failed("could not share the graphics device with screen capture", e.code())};
        }
        return {};
    }

    void begin(ID3D11DeviceContext* context) {
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->IASetInputLayout(nullptr);
        context->VSSetShader(vertex_.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        context->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context->RSSetState(raster_.Get());
        context->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
    }

    void end(ID3D11DeviceContext* context) {
        ID3D11ShaderResourceView* none[3] = {};
        context->PSSetShaderResources(0, 3, none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    // One picture into both planes of the frame.
    void quad(ID3D11DeviceContext* context, Target& target, const core::Placement& p, const double (&uv)[4], bool opaque,
              ID3D11PixelShader* luma, ID3D11PixelShader* chroma, double fade = 1, const Shaping& shaping = {},
              const ColorAdjust& color_adjust = {}) {
        const double w = draw_width_, h = draw_height_;
        Constants constants{
            {static_cast<float>(2 * p.left / w - 1), static_cast<float>(1 - 2 * p.top / h),
             static_cast<float>(2 * p.right / w - 1), static_cast<float>(1 - 2 * p.bottom / h)},
            {static_cast<float>(uv[0]), static_cast<float>(uv[1]), static_cast<float>(uv[2]), static_cast<float>(uv[3])},
            opaque ? 1.0f : 0.0f,
            static_cast<float>(fade),
            shaping.mode,
            shaping.shape,
            {}, {}, {}, {}, {}, {}, {}};
        std::copy(std::begin(shaping.box), std::end(shaping.box), constants.box);
        std::copy(std::begin(shaping.size), std::end(shaping.size), constants.size);
        std::copy(std::begin(shaping.fill), std::end(shaping.fill), constants.fill);
        color_constants(color_adjust, constants);
        context->UpdateSubresource(constants_.Get(), 0, nullptr, &constants, 0, 0);
        const D3D11_VIEWPORT full{0, 0, static_cast<float>(draw_width_), static_cast<float>(draw_height_), 0, 1};
        const D3D11_VIEWPORT half{0, 0, static_cast<float>(draw_width_ / 2), static_cast<float>(draw_height_ / 2), 0, 1};
        context->OMSetRenderTargets(1, target.luma.GetAddressOf(), nullptr);
        context->RSSetViewports(1, &full);
        context->PSSetShader(luma, nullptr, 0);
        context->Draw(4, 0);
        context->OMSetRenderTargets(1, target.chroma.GetAddressOf(), nullptr);
        context->RSSetViewports(1, &half);
        context->PSSetShader(chroma, nullptr, 0);
        context->Draw(4, 0);
    }

    // The main capture, already fitted into a whole main-canvas frame, fitted again into its area.
    bool draw_screen(ID3D11DeviceContext* context, Target& target, ID3D11Texture2D* screen, const Layout& layout,
                     const ColorAdjust& adjustment) {
        Planes* planes = planes_for(screen);
        if (!planes) return false;
        ID3D11ShaderResourceView* views[2] = {planes->luma.Get(), planes->chroma.Get()};
        context->PSSetShaderResources(1, 2, views);
        const auto p = core::place(Overlay::Kind::image, layout.area, draw_width_, draw_height_, width_, height_, layout.crop);
        const double uv[4] = {p.u0, p.v0, p.u1, p.v1};
        const bool adjusted = adjustment.brightness || adjustment.contrast || adjustment.saturation;
        quad(context, target, p, uv, true, adjusted ? adjusted_screen_luma_.Get() : screen_luma_.Get(),
             adjusted ? adjusted_screen_chroma_.Get() : screen_chroma_.Get(), 1, {}, adjustment);
        return true;
    }

    void draw_mark(ID3D11DeviceContext* context, Target& target, const core::ClickMark& mark) {
        const double r = mark.diameter / 2, uv[4] = {0, 0, 1, 1};
        const core::Placement p{mark.x - r, mark.y - r, mark.x + r, mark.y + r};
        context->PSSetShaderResources(0, 1, click_marks_[mark.right ? 1 : 0].GetAddressOf());
        quad(context, target, p, uv, false, luma_.Get(), chroma_.Get(), mark.alpha);
    }

    // The main capture's rectangle on the desktop in physical pixels, looked up again every
    // half second or so, since a window moves.
    core::DesktopRect click_area() {
        if (click_lookups_++ % 16) return click_rect_;
        const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        RECT bounds{};
        bool found = false;
        if (auto* window = static_cast<HWND>(click_source_.window)) {
            found = SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof bounds)) ||
                    GetWindowRect(window, &bounds);
        } else if (auto* monitor = static_cast<HMONITOR>(click_source_.monitor)) {
            MONITORINFO info{sizeof info};
            found = GetMonitorInfoW(monitor, &info);
            bounds = info.rcMonitor;
        }
        if (previous) SetThreadDpiAwarenessContext(previous);
        if (!found) return click_rect_ = {};
        click_rect_ = {static_cast<double>(bounds.left), static_cast<double>(bounds.top),
                       static_cast<double>(bounds.right - bounds.left), static_cast<double>(bounds.bottom - bounds.top)};
        if (const auto& r = click_source_.region)
            click_rect_ = {click_rect_.x + r->x, click_rect_.y + r->y, static_cast<double>(r->width), static_cast<double>(r->height)};
        return click_rect_;
    }

    // A camera's picture; a phone's is made again when its size changes.
    Result camera_texture(Layer& layer, UINT width, UINT height) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        layer.texture.Reset();
        layer.view.Reset();
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &layer.texture);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(layer.texture.Get(), nullptr, &layer.view);
        if (FAILED(hr)) {
            layer.texture.Reset();
            layer.view.Reset();
            return {failed("could not allocate the camera picture", hr)};
        }
        layer.width = width;
        layer.height = height;
        return {};
    }

    void draw_layer(ID3D11DeviceContext* context, Target& target, Layer& layer, const Layout& layout, const Style& style,
                    const ColorAdjust& adjustment) {
        if (layer.camera) {
            layer.camera->upload(layer.seen, [&](const std::uint8_t* pixels, std::size_t row, UINT width, UINT height) {
                if ((width != layer.width || height != layer.height || !layer.texture) && !camera_texture(layer, width, height))
                    return false;
                context->UpdateSubresource(layer.texture.Get(), 0, nullptr, pixels, static_cast<UINT>(row), 0);
                layer.ready = true;
                return true;
            });
        }
        if (!layer.ready) return;
        const auto area = core::shaped_area(style, layout.area, draw_width_, draw_height_);
        const auto p = core::place(layer.overlay.kind, area, draw_width_, draw_height_, layer.width, layer.height, layout.crop);
        const double* c = layer.crop;
        const double uv[4] = {c[0] + p.u0 * (c[2] - c[0]), c[1] + p.v0 * (c[3] - c[1]), c[0] + p.u1 * (c[2] - c[0]),
                              c[1] + p.v1 * (c[3] - c[1])};
        Shaping inside;
        if (!core::plain(style)) {
            const auto d = core::decor(style, draw_height_);
            const double w = p.right - p.left, h = p.bottom - p.top;
            const auto shaping = [&](float mode, double inset, double pad) {
                Shaping s;
                s.mode = mode;
                s.shape = static_cast<float>(style.shape);
                const double box[4] = {-pad - inset, -pad - inset, w + pad - inset, h + pad - inset};
                const double size[4] = {w - 2 * inset, h - 2 * inset,
                                        style.shape == Style::Shape::squircle ? core::squircle_exponent(style.roundness) : style.roundness, pad};
                for (int i = 0; i < 4; ++i) {
                    s.box[i] = static_cast<float>(box[i]);
                    s.size[i] = static_cast<float>(size[i]);
                }
                s.fill[0] = (style.border_color >> 16 & 0xff) / 255.0f;
                s.fill[1] = (style.border_color >> 8 & 0xff) / 255.0f;
                s.fill[2] = (style.border_color & 0xff) / 255.0f;
                return s;
            };
            const double none[4] = {0, 0, 1, 1};
            if (style.shadow) {
                const core::Placement shadow{p.left - d.pad, p.top - d.pad + d.drop, p.right + d.pad, p.bottom + d.pad + d.drop};
                quad(context, target, shadow, none, false, luma_.Get(), chroma_.Get(), 1, shaping(2, 0, d.pad));
            }
            if (d.border > 0) quad(context, target, p, none, false, luma_.Get(), chroma_.Get(), 1, shaping(1, 0, 0));
            inside = shaping(0, d.border, 0);
        }
        context->PSSetShaderResources(0, 1, layer.view.GetAddressOf());
        const bool adjusted = adjustment.brightness || adjustment.contrast || adjustment.saturation;
        quad(context, target, p, uv, layer.camera || layer.screen, adjusted ? adjusted_luma_.Get() : luma_.Get(),
             adjusted ? adjusted_chroma_.Get() : chroma_.Get(), 1, inside, adjustment);
    }

    // Copies a display's or window's newest frame into the layer's texture.
    void upload_screen(ID3D11DeviceContext* context, Layer& layer) {
        auto frame = layer.screen->take();
        if (!frame) return;
        try {
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> surface;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&surface)));
            D3D11_TEXTURE2D_DESC desc;
            surface->GetDesc(&desc);
            D3D11_TEXTURE2D_DESC have{};
            if (layer.texture) layer.texture->GetDesc(&have);
            if (!layer.texture || have.Width != desc.Width || have.Height != desc.Height) {
                layer.view.Reset();
                layer.texture.Reset();
                layer.ready = false;
                D3D11_TEXTURE2D_DESC copy = desc;
                copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                copy.MiscFlags = 0;
                copy.CPUAccessFlags = 0;
                copy.Usage = D3D11_USAGE_DEFAULT;
                if (FAILED(device_->CreateTexture2D(&copy, nullptr, &layer.texture)) ||
                    FAILED(device_->CreateShaderResourceView(layer.texture.Get(), nullptr, &layer.view))) {
                    layer.texture.Reset();
                    frame.Close();
                    return;
                }
            }
            context->CopyResource(layer.texture.Get(), surface.Get());
            const auto content = frame.ContentSize();
            const double w = desc.Width, h = desc.Height;
            double x0 = 0, y0 = 0, x1 = std::min<double>(w, content.Width), y1 = std::min<double>(h, content.Height);
            if (const auto& r = layer.screen->region()) {
                x0 = std::min<double>(r->x, x1 - 2);
                y0 = std::min<double>(r->y, y1 - 2);
                x1 = std::min<double>(x1, r->x + static_cast<double>(r->width));
                y1 = std::min<double>(y1, r->y + static_cast<double>(r->height));
            }
            layer.width = static_cast<UINT>(std::max(2.0, x1 - x0));
            layer.height = static_cast<UINT>(std::max(2.0, y1 - y0));
            layer.crop[0] = x0 / w;
            layer.crop[1] = y0 / h;
            layer.crop[2] = x1 / w;
            layer.crop[3] = y1 / h;
            layer.ready = true;
        } catch (...) {
        }
        frame.Close();
    }

    // Encoder frames come from a fixed pool, so their views are kept.
    Target* target_for(ID3D11Texture2D* frame, unsigned subresource) {
        for (auto& target : targets_)
            if (target.texture.Get() == frame && target.subresource == subresource) return &target;
        if (targets_.size() >= 64) targets_.clear();
        D3D11_TEXTURE2D_DESC desc;
        frame->GetDesc(&desc);
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        if (desc.ArraySize > 1) {
            view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.FirstArraySlice = subresource;
            view.Texture2DArray.ArraySize = 1;
        } else {
            view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        }
        Target target;
        target.texture = frame;
        target.subresource = subresource;
        view.Format = DXGI_FORMAT_R8_UNORM;
        if (FAILED(device_->CreateRenderTargetView(frame, &view, &target.luma))) return nullptr;
        view.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(device_->CreateRenderTargetView(frame, &view, &target.chroma))) return nullptr;
        targets_.push_back(std::move(target));
        return &targets_.back();
    }

    // The main capture's frames are a fixed set of NV12 textures too.
    Planes* planes_for(ID3D11Texture2D* screen) {
        for (auto& planes : planes_)
            if (planes.texture.Get() == screen) return &planes;
        if (planes_.size() >= 16) planes_.clear();
        Planes planes;
        planes.texture = screen;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        view.Format = DXGI_FORMAT_R8_UNORM;
        if (FAILED(device_->CreateShaderResourceView(screen, &view, &planes.luma))) return nullptr;
        view.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(device_->CreateShaderResourceView(screen, &view, &planes.chroma))) return nullptr;
        planes_.push_back(std::move(planes));
        return &planes_.back();
    }

    Result create_pipeline() {
        auto compile = [](const char* entry, const char* profile, ComPtr<ID3DBlob>& code) -> Result {
            ComPtr<ID3DBlob> errors;
            const HRESULT hr = D3DCompile(kShaders, sizeof kShaders - 1, "overlay", nullptr, nullptr, entry, profile,
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (FAILED(hr)) {
                std::string why = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
                return {failed("could not compile the overlay shaders", hr) + (why.empty() ? "" : ": " + why)};
            }
            return {};
        };
        ComPtr<ID3DBlob> vs, luma, chroma, adjusted_luma, adjusted_chroma, screen_luma, screen_chroma,
            adjusted_screen_luma, adjusted_screen_chroma;
        if (auto c = compile("vs", "vs_4_0", vs); !c) return c;
        if (auto c = compile("luma", "ps_4_0", luma); !c) return c;
        if (auto c = compile("chroma", "ps_4_0", chroma); !c) return c;
        if (auto c = compile("adjusted_luma", "ps_4_0", adjusted_luma); !c) return c;
        if (auto c = compile("adjusted_chroma", "ps_4_0", adjusted_chroma); !c) return c;
        if (auto c = compile("screen_luma", "ps_4_0", screen_luma); !c) return c;
        if (auto c = compile("screen_chroma", "ps_4_0", screen_chroma); !c) return c;
        if (auto c = compile("adjusted_screen_luma", "ps_4_0", adjusted_screen_luma); !c) return c;
        if (auto c = compile("adjusted_screen_chroma", "ps_4_0", adjusted_screen_chroma); !c) return c;
        HRESULT hr = device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_);
        if (SUCCEEDED(hr)) hr = device_->CreatePixelShader(luma->GetBufferPointer(), luma->GetBufferSize(), nullptr, &luma_);
        if (SUCCEEDED(hr)) hr = device_->CreatePixelShader(chroma->GetBufferPointer(), chroma->GetBufferSize(), nullptr, &chroma_);
        if (SUCCEEDED(hr))
            hr = device_->CreatePixelShader(adjusted_luma->GetBufferPointer(), adjusted_luma->GetBufferSize(), nullptr, &adjusted_luma_);
        if (SUCCEEDED(hr))
            hr = device_->CreatePixelShader(adjusted_chroma->GetBufferPointer(), adjusted_chroma->GetBufferSize(), nullptr, &adjusted_chroma_);
        if (SUCCEEDED(hr))
            hr = device_->CreatePixelShader(screen_luma->GetBufferPointer(), screen_luma->GetBufferSize(), nullptr, &screen_luma_);
        if (SUCCEEDED(hr))
            hr = device_->CreatePixelShader(screen_chroma->GetBufferPointer(), screen_chroma->GetBufferSize(), nullptr, &screen_chroma_);
        if (SUCCEEDED(hr))
            hr = device_->CreatePixelShader(adjusted_screen_luma->GetBufferPointer(), adjusted_screen_luma->GetBufferSize(), nullptr, &adjusted_screen_luma_);
        if (SUCCEEDED(hr))
            hr = device_->CreatePixelShader(adjusted_screen_chroma->GetBufferPointer(), adjusted_screen_chroma->GetBufferSize(), nullptr, &adjusted_screen_chroma_);
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(Constants);
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(hr)) hr = device_->CreateBuffer(&buffer, nullptr, &constants_);
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (SUCCEEDED(hr)) hr = device_->CreateSamplerState(&sampler, &sampler_);
        D3D11_BLEND_DESC blend{};
        auto& rt = blend.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (SUCCEEDED(hr)) hr = device_->CreateBlendState(&blend, &blend_);
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        if (SUCCEEDED(hr)) hr = device_->CreateRasterizerState(&raster, &raster_);
        if (FAILED(hr)) return {failed("could not set up overlay drawing", hr)};
        return {};
    }

    ComPtr<ID3D11Device> device_;
    wgdx::Direct3D11::IDirect3DDevice winrt_device_{nullptr};
    std::uint32_t width_ = 0, height_ = 0;            // The main canvas, and the main capture's frames.
    std::uint32_t draw_width_ = 0, draw_height_ = 0;  // The canvas being drawn.
    ComPtr<ID3D11VertexShader> vertex_;
    ComPtr<ID3D11PixelShader> luma_, chroma_, adjusted_luma_, adjusted_chroma_, screen_luma_, screen_chroma_,
        adjusted_screen_luma_, adjusted_screen_chroma_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11RasterizerState> raster_;
    mutable std::mutex mutex_;  // Guards the layer list and the canvases.
    std::vector<Layer> layers_;
    std::array<Canvas, 2> canvases_;
    std::vector<Target> targets_;
    std::vector<Planes> planes_;
    std::unique_ptr<core::Clicks> clicks_;
    std::unique_ptr<ClickWatch> watch_;
    ClickSource click_source_;
    core::DesktopRect click_rect_;
    std::uint64_t click_lookups_ = 0;
    ColorAdjust screen_color_, camera_color_;
    ComPtr<ID3D11ShaderResourceView> click_marks_[2];
};

Overlays::Overlays() : impl_(std::make_unique<Impl>()) {}
Overlays::~Overlays() { impl_->stop(); }
Result Overlays::start(ID3D11Device* device, std::uint32_t width, std::uint32_t height, const Options& options,
                       const ClickSource& clicks) {
    return impl_->start(device, width, height, options, clicks);
}
bool Overlays::draw(std::size_t canvas, ID3D11DeviceContext* context, ID3D11Texture2D* screen, ID3D11Texture2D* frame,
                    unsigned subresource, std::chrono::steady_clock::time_point at) {
    return impl_->draw(canvas, context, screen, frame, subresource, at);
}
Result Overlays::set_layout(std::size_t canvas, std::size_t layer, const Layout& layout) {
    return impl_->set_layout(canvas, layer, layout);
}
Result Overlays::set_style(std::size_t index, const Style& style) { return impl_->set_style(index, style); }
Result Overlays::set_color(ColorTarget target, const ColorAdjust& color) { return impl_->set_color(target, color); }
bool Overlays::needs_composition(std::size_t canvas) const { return impl_->needs_composition(canvas); }
Result Overlays::set(std::size_t index, bool visible, const std::optional<Area>& area) { return impl_->set(index, visible, area); }
Result Overlays::set_screen(bool visible, const std::optional<Area>& area) { return impl_->set_screen(visible, area); }
std::uint64_t Overlays::camera_frames() const { return impl_->camera_frames(); }
bool Overlays::camera_lost() const { return impl_->camera_lost(); }
void Overlays::stop() { impl_->stop(); }

namespace {
// Studio-range BT.709 NV12 to RGB, drawn over the whole preview with bilinear sampling.
constexpr char kPreviewShaders[] = R"(
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vs(uint id : SV_VertexID) {
    float2 t = float2(id & 1, id >> 1);
    Vertex v;
    v.position = float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1);
    v.uv = t;
    return v;
}
Texture2D luma : register(t0);
Texture2D chroma : register(t1);
SamplerState smooth : register(s0);
float4 ps(Vertex v) : SV_Target {
    float y = (luma.Sample(smooth, v.uv).r - 16.0 / 255) * 255.0 / 219;
    float2 c = (chroma.Sample(smooth, v.uv).rg - 128.0 / 255) * 255.0 / 224;
    return float4(saturate(float3(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x)), 1);
}
)";
} // namespace

class FramePreview::Impl {
public:
    Result start(ID3D11Device* device, std::uint32_t width, std::uint32_t height, core::PreviewSize size) {
        size_ = size;
        auto compile = [](const char* entry, const char* profile, ComPtr<ID3DBlob>& code) -> HRESULT {
            return D3DCompile(kPreviewShaders, sizeof kPreviewShaders - 1, "preview", nullptr, nullptr, entry, profile,
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr);
        };
        ComPtr<ID3DBlob> vs, ps;
        HRESULT hr = compile("vs", "vs_4_0", vs);
        if (SUCCEEDED(hr)) hr = compile("ps", "ps_4_0", ps);
        if (SUCCEEDED(hr)) hr = device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_);
        if (SUCCEEDED(hr)) hr = device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_);
        if (FAILED(hr)) return {failed("could not compile the preview shaders", hr)};

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = device->CreateTexture2D(&desc, nullptr, &source_);
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        view.Format = DXGI_FORMAT_R8_UNORM;
        if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(source_.Get(), &view, &views_[0]);
        view.Format = DXGI_FORMAT_R8G8_UNORM;
        if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(source_.Get(), &view, &views_[1]);
        if (FAILED(hr)) return {failed("this graphics driver cannot read video frames for the preview", hr)};

        desc.Width = size.width;
        desc.Height = size.height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = device->CreateTexture2D(&desc, nullptr, &target_);
        if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(target_.Get(), nullptr, &target_view_);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& staging : staging_)
            if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&desc, nullptr, &staging);
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (SUCCEEDED(hr)) hr = device->CreateSamplerState(&sampler, &sampler_);
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        if (SUCCEEDED(hr)) hr = device->CreateRasterizerState(&raster, &raster_);
        if (FAILED(hr)) return {failed("could not allocate the preview", hr)};
        return {};
    }

    // Draws this frame into one staging texture and reads the other, which the GPU filled a
    // preview frame ago, so the recording thread never waits for the GPU.
    void render(ID3D11DeviceContext* context, ID3D11Texture2D* frame, unsigned subresource, core::PreviewBuffer& out) {
        ComPtr<ID3D11Multithread> multithread;
        context->QueryInterface(IID_PPV_ARGS(&multithread));
        if (multithread) multithread->Enter();
        context->CopySubresourceRegion(source_.Get(), 0, 0, 0, 0, frame, subresource, nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->IASetInputLayout(nullptr);
        context->VSSetShader(vertex_.Get(), nullptr, 0);
        context->PSSetShader(pixel_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* views[] = {views_[0].Get(), views_[1].Get()};
        context->PSSetShaderResources(0, 2, views);
        context->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context->RSSetState(raster_.Get());
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(size_.width), static_cast<float>(size_.height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, target_view_.GetAddressOf(), nullptr);
        context->Draw(4, 0);
        ID3D11ShaderResourceView* none[] = {nullptr, nullptr};
        context->PSSetShaderResources(0, 2, none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(staging_[next_].Get(), target_.Get());
        // Without an encoder submitting work (preview-only), the copy would wait in the
        // command buffer and the read below would never find it done.
        context->Flush();
        filled_[next_] = true;
        next_ ^= 1;
        if (filled_[next_]) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(staging_[next_].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
                out.publish(static_cast<const std::uint8_t*>(mapped.pData), mapped.RowPitch);
                context->Unmap(staging_[next_].Get(), 0);
                filled_[next_] = false;
            }
        }
        if (multithread) multithread->Leave();
    }

private:
    core::PreviewSize size_{};
    ComPtr<ID3D11VertexShader> vertex_;
    ComPtr<ID3D11PixelShader> pixel_;
    ComPtr<ID3D11Texture2D> source_, target_;
    ComPtr<ID3D11ShaderResourceView> views_[2];
    ComPtr<ID3D11RenderTargetView> target_view_;
    ComPtr<ID3D11Texture2D> staging_[2];
    bool filled_[2]{};
    int next_ = 0;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> raster_;
};

FramePreview::FramePreview() : impl_(std::make_unique<Impl>()) {}
FramePreview::~FramePreview() = default;
Result FramePreview::start(ID3D11Device* device, std::uint32_t width, std::uint32_t height, core::PreviewSize size) {
    return impl_->start(device, width, height, size);
}
void FramePreview::render(ID3D11DeviceContext* context, ID3D11Texture2D* frame, unsigned subresource, core::PreviewBuffer& out) {
    impl_->render(context, frame, subresource, out);
}
} // namespace rec::win
