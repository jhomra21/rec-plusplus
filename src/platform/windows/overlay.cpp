#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "platform/windows/overlay.hpp"
#include "core/overlay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace rec::win {
namespace {
using Microsoft::WRL::ComPtr;

// Overlay pixels are premultiplied BGRA. They are converted to BT.709 studio-range YCbCr
// before blending (ONE, INV_SRC_ALPHA): the conversion is affine, so blending the converted
// values equals converting the blended RGB. Luma and chroma are drawn into the NV12
// frame's two planes in separate passes.
constexpr char kShaders[] = R"(
cbuffer Placement : register(b0) {
    float4 dest;    // left, top, right, bottom in clip space
    float4 source;  // u0, v0, u1, v1
    float opaque;   // 1: the texture has no alpha (a camera)
    float3 unused;
};
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vs(uint id : SV_VertexID) {
    float2 t = float2(id & 1, id >> 1);
    Vertex v;
    v.position = float4(lerp(dest.x, dest.z, t.x), lerp(dest.y, dest.w, t.y), 0, 1);
    v.uv = float2(lerp(source.x, source.z, t.x), lerp(source.y, source.w, t.y));
    return v;
}
Texture2D picture : register(t0);
SamplerState smooth : register(s0);
float4 color(float2 uv) {
    float4 c = picture.Sample(smooth, uv);
    return opaque > 0.5 ? float4(c.rgb, 1) : c;
}
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
float4 luma(Vertex v) : SV_Target {
    float4 c = color(v.uv);
    return float4(c.a * 16.0 / 255 + 219.0 / 255 * dot(c.rgb, kLuma), 0, 0, c.a);
}
float4 chroma(Vertex v) : SV_Target {
    float4 c = color(v.uv);
    float y = dot(c.rgb, kLuma);
    float2 uv = float2((c.b - y) / 1.8556, (c.r - y) / 1.5748);
    return float4(c.a * 128.0 / 255 + 224.0 / 255 * uv, 0, c.a);
}
)";

struct Constants {
    float dest[4];
    float source[4];
    float opaque;
    float unused[3];
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
// core::test_camera_frame at 30 fps.
class CameraFeed {
public:
    ~CameraFeed() { stop(); }

    Result open(const std::string& wanted) {
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
        if (!chosen) return {"camera not found: " + wanted + "; run `rec cameras`"};
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
        thread_ = std::thread([this] { reader_ ? read() : generate(); });
    }

    void stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        reader_.Reset();
        if (source_) { source_->Shutdown(); source_.Reset(); }
    }

    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::uint64_t frames() const { return frames_.load(); }

    // Copies the newest frame to a mapped texture when it is newer than seen.
    template <class Map> bool upload(std::uint64_t& seen, Map&& map) {
        if (frames_.load() == seen) return false;
        std::lock_guard lock(mutex_);
        seen = frames_.load();
        return map(pixels_.data(), static_cast<std::size_t>(width_) * 4);
    }

private:
    void generate() {
        std::vector<std::uint8_t> frame(pixels_.size());
        const auto begin = std::chrono::steady_clock::now();
        for (std::uint64_t n = 0; !stop_; ++n) {
            core::test_camera_frame(n, frame.data(), static_cast<std::size_t>(width_) * 4);
            {
                std::lock_guard lock(mutex_);
                pixels_.swap(frame);
                frames_.fetch_add(1);
            }
            std::this_thread::sleep_until(begin + std::chrono::microseconds((n + 1) * 1000000 / core::kTestCameraFps));
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
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) break;
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
    std::atomic_bool stop_{false};
    std::thread thread_;
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

class Overlays::Impl {
public:
    struct Layer {
        Overlay overlay;
        UINT width = 0, height = 0;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        std::unique_ptr<CameraFeed> camera;
        std::uint64_t seen = 0;
        bool ready = false;  // Has a picture to draw.
    };
    struct Target {
        ComPtr<ID3D11Texture2D> texture;
        unsigned subresource = 0;
        ComPtr<ID3D11RenderTargetView> luma, chroma;
    };

    Result start(ID3D11Device* device, std::uint32_t width, std::uint32_t height, const std::vector<Overlay>& overlays) {
        if (auto error = core::check_overlays(overlays); !error.empty()) return {error};
        device_ = device;
        width_ = width;
        height_ = height;
        UINT support = 0;
        if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_NV12, &support)) || !(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
            return {"this graphics driver cannot draw overlays into video frames"};
        if (auto made = create_pipeline(); !made) return made;
        for (const auto& overlay : overlays) {
            Layer layer;
            layer.overlay = overlay;
            if (overlay.kind == Overlay::Kind::image) {
                if (auto loaded = load_image(device, overlay.source, layer.texture, layer.width, layer.height); !loaded)
                    return loaded;
                layer.ready = true;
            } else {
                layer.camera = std::make_unique<CameraFeed>();
                if (auto opened = layer.camera->open(overlay.source); !opened) return opened;
                layer.width = layer.camera->width();
                layer.height = layer.camera->height();
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = layer.width;
                desc.Height = layer.height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                const HRESULT hr = device->CreateTexture2D(&desc, nullptr, &layer.texture);
                if (FAILED(hr)) return {failed("could not allocate the camera picture", hr)};
            }
            const HRESULT hr = device->CreateShaderResourceView(layer.texture.Get(), nullptr, &layer.view);
            if (FAILED(hr)) return {failed("could not use an overlay picture", hr)};
            layers_.push_back(std::move(layer));
        }
        for (auto& layer : layers_)
            if (layer.camera) layer.camera->start();
        return {};
    }

    Result set(std::size_t index, bool visible, const std::optional<Area>& area) {
        std::lock_guard lock(mutex_);
        if (index >= layers_.size()) return {"no overlay " + std::to_string(index + 1)};
        if (area) {
            if (auto error = core::check_area(index, *area); !error.empty()) return {error};
            layers_[index].overlay.area = *area;
        }
        layers_[index].overlay.visible = visible;
        return {};
    }

    std::uint64_t camera_frames() const {
        std::lock_guard lock(mutex_);
        std::uint64_t total = 0;
        for (const auto& layer : layers_)
            if (layer.camera) total += layer.camera->frames();
        return total;
    }

    bool draw(ID3D11DeviceContext* context, ID3D11Texture2D* frame, unsigned subresource) {
        struct Job { Layer* layer; Area area; };
        Job jobs[core::kMaxOverlays];
        std::size_t count = 0;
        {
            std::lock_guard lock(mutex_);
            for (auto& layer : layers_)
                if (layer.overlay.visible) jobs[count++] = {&layer, layer.overlay.area};
        }
        if (!count) return true;
        ComPtr<ID3D11Multithread> multithread;
        context->QueryInterface(IID_PPV_ARGS(&multithread));
        if (multithread) multithread->Enter();
        const bool drawn = draw_locked(context, frame, subresource, jobs, count);
        if (multithread) multithread->Leave();
        return drawn;
    }

    void stop() {
        std::lock_guard lock(mutex_);
        for (auto& layer : layers_)
            if (layer.camera) layer.camera->stop();
        layers_.clear();
        targets_.clear();
    }

private:
    template <class Job>
    bool draw_locked(ID3D11DeviceContext* context, ID3D11Texture2D* frame, unsigned subresource, Job* jobs, std::size_t count) {
        Target* target = target_for(frame, subresource);
        if (!target) return false;
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->IASetInputLayout(nullptr);
        context->VSSetShader(vertex_.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        context->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context->RSSetState(raster_.Get());
        context->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);
        for (std::size_t i = 0; i < count; ++i) {
            Layer& layer = *jobs[i].layer;
            if (layer.camera) {
                layer.camera->upload(layer.seen, [&](const std::uint8_t* pixels, std::size_t row) {
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if (FAILED(context->Map(layer.texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
                    for (UINT y = 0; y < layer.height; ++y)
                        std::memcpy(static_cast<std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                                    pixels + y * row, row);
                    context->Unmap(layer.texture.Get(), 0);
                    layer.ready = true;
                    return true;
                });
            }
            if (!layer.ready) continue;
            const auto p = core::place(layer.overlay.kind, jobs[i].area, width_, height_, layer.width, layer.height);
            const Constants constants{
                {static_cast<float>(2 * p.left / width_ - 1), static_cast<float>(1 - 2 * p.top / height_),
                 static_cast<float>(2 * p.right / width_ - 1), static_cast<float>(1 - 2 * p.bottom / height_)},
                {static_cast<float>(p.u0), static_cast<float>(p.v0), static_cast<float>(p.u1), static_cast<float>(p.v1)},
                layer.camera ? 1.0f : 0.0f,
                {}};
            context->UpdateSubresource(constants_.Get(), 0, nullptr, &constants, 0, 0);
            context->PSSetShaderResources(0, 1, layer.view.GetAddressOf());
            const D3D11_VIEWPORT full{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
            const D3D11_VIEWPORT half{0, 0, static_cast<float>(width_ / 2), static_cast<float>(height_ / 2), 0, 1};
            context->OMSetRenderTargets(1, target->luma.GetAddressOf(), nullptr);
            context->RSSetViewports(1, &full);
            context->PSSetShader(luma_.Get(), nullptr, 0);
            context->Draw(4, 0);
            context->OMSetRenderTargets(1, target->chroma.GetAddressOf(), nullptr);
            context->RSSetViewports(1, &half);
            context->PSSetShader(chroma_.Get(), nullptr, 0);
            context->Draw(4, 0);
        }
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        return true;
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
        ComPtr<ID3DBlob> vs, luma, chroma;
        if (auto c = compile("vs", "vs_4_0", vs); !c) return c;
        if (auto c = compile("luma", "ps_4_0", luma); !c) return c;
        if (auto c = compile("chroma", "ps_4_0", chroma); !c) return c;
        HRESULT hr = device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_);
        if (SUCCEEDED(hr)) hr = device_->CreatePixelShader(luma->GetBufferPointer(), luma->GetBufferSize(), nullptr, &luma_);
        if (SUCCEEDED(hr)) hr = device_->CreatePixelShader(chroma->GetBufferPointer(), chroma->GetBufferSize(), nullptr, &chroma_);
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
    std::uint32_t width_ = 0, height_ = 0;
    ComPtr<ID3D11VertexShader> vertex_;
    ComPtr<ID3D11PixelShader> luma_, chroma_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11RasterizerState> raster_;
    mutable std::mutex mutex_;  // Guards the layer list and each layer's visibility and rectangle.
    std::vector<Layer> layers_;
    std::vector<Target> targets_;
};

Overlays::Overlays() : impl_(std::make_unique<Impl>()) {}
Overlays::~Overlays() { impl_->stop(); }
Result Overlays::start(ID3D11Device* device, std::uint32_t width, std::uint32_t height, const std::vector<Overlay>& overlays) {
    return impl_->start(device, width, height, overlays);
}
bool Overlays::draw(ID3D11DeviceContext* context, ID3D11Texture2D* frame, unsigned subresource) {
    return impl_->draw(context, frame, subresource);
}
Result Overlays::set(std::size_t index, bool visible, const std::optional<Area>& area) { return impl_->set(index, visible, area); }
std::uint64_t Overlays::camera_frames() const { return impl_->camera_frames(); }
void Overlays::stop() { impl_->stop(); }

} // namespace rec::win
