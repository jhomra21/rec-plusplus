#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>

#include <audioclient.h>
#include <d3d11_4.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <initguid.h>
#include <codecapi.h>

#include "rec/rec.hpp"
#include "core/mixer.hpp"
#include "core/overlay.hpp"
#include "core/pause.hpp"
#include "core/sync.hpp"
#include "platform/windows/overlay.hpp"
#include "platform/windows/stream.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rec {
namespace {
using Microsoft::WRL::ComPtr;
using core::kMixRate;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;

constexpr std::int64_t kTicks = 10'000'000;  // 100 ns units, the unit of QPC-derived WGC and WASAPI times.
// Video is emitted this far behind real time so the frame on screen at each slot instant
// has already been delivered and converted.
constexpr auto kEmitDelay = std::chrono::milliseconds(40);
constexpr std::int64_t kMixWaitFrames = kMixRate * 3 / 10;
constexpr std::size_t kChunkFrames = 2400;  // 50 ms at 48 kHz.
// Device.FriendlyName, defined here to avoid instantiating every property key.
constexpr PROPERTYKEY kFriendlyName{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string failed(const char* what, HRESULT hr) {
    char code[16];
    std::snprintf(code, sizeof code, "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(what) + " (" + code + ")";
}

std::string utf8(const wchar_t* text) {
    if (!text) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string out(static_cast<std::size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& text) {
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (chars <= 1) return {};
    std::wstring out(static_cast<std::size_t>(chars - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), chars);
    return out;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// QPC in 100 ns units: the clock behind WGC SystemRelativeTime and WASAPI QPC positions.
std::int64_t now_ticks() {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart / frequency * kTicks + counter.QuadPart % frequency * kTicks / frequency;
}

struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

struct Size { std::uint32_t width, height; };

Size output_size(const Options& options, std::uint32_t native_w, std::uint32_t native_h) {
    if (!options.width || !options.height) return {native_w & ~1u, native_h & ~1u};
    const double factor = std::min({1.0, static_cast<double>(options.width) / native_w,
                                    static_cast<double>(options.height) / native_h});
    return {std::max(2u, static_cast<std::uint32_t>(native_w * factor) & ~1u),
            std::max(2u, static_cast<std::uint32_t>(native_h * factor) & ~1u)};
}

struct Monitor {
    HMONITOR handle = nullptr;
    std::string device;
    std::uint32_t width = 0, height = 0;
    bool primary = false;
};

std::string process_name(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH];
    DWORD length = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(process, 0, path, &length)) {
        std::wstring full(path, length);
        const auto slash = full.find_last_of(L"\\/");
        if (slash != std::wstring::npos) full.erase(0, slash + 1);
        if (full.size() > 4 && _wcsicmp(full.c_str() + full.size() - 4, L".exe") == 0) full.resize(full.size() - 4);
        name = utf8(full.c_str());
    }
    CloseHandle(process);
    return name;
}

// Top-level windows a person would pick, as the system capture picker shows them.
bool capturable(HWND window) {
    if (!IsWindowVisible(window) || IsIconic(window) || GetWindow(window, GW_OWNER)) return false;
    if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    if (GetWindowTextLengthW(window) == 0) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId()) return false;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return false;
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    const std::wstring_view kind(name);
    return kind != L"Progman" && kind != L"WorkerW" && kind != L"Shell_TrayWnd" && kind != L"Shell_SecondaryTrayWnd";
}

// The largest rectangle of the source's shape that fits the output, centered on even pixels.
RECT fit(LONG source_w, LONG source_h, std::uint32_t width, std::uint32_t height) {
    const double scale = std::min(static_cast<double>(width) / source_w, static_cast<double>(height) / source_h);
    auto w = static_cast<LONG>(std::lround(source_w * scale));
    auto h = static_cast<LONG>(std::lround(source_h * scale));
    if (static_cast<LONG>(width) - w <= 2) w = static_cast<LONG>(width);
    if (static_cast<LONG>(height) - h <= 2) h = static_cast<LONG>(height);
    const LONG x = ((static_cast<LONG>(width) - w) / 2) & ~1L;
    const LONG y = ((static_cast<LONG>(height) - h) / 2) & ~1L;
    return {x, y, x + w, y + h};
}

// Primary display first, so display 1 is the main display as on macOS.
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

ComPtr<IMMDeviceEnumerator> device_enumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    return enumerator;
}

std::string endpoint_id(IMMDevice* device) {
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id))) return {};
    std::string out = utf8(id);
    CoTaskMemFree(id);
    return out;
}

std::string endpoint_name(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string out;
    if (SUCCEEDED(store->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR) out = utf8(value.pwszVal);
    PropVariantClear(&value);
    return out;
}

ComPtr<IMMDevice> find_microphone(const std::string& query, std::string& error) {
    auto enumerator = device_enumerator();
    if (!enumerator) { error = "could not open the Windows audio device list"; return nullptr; }
    ComPtr<IMMDevice> device;
    if (query.empty() || query == "default") {
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) error = "no microphone found";
        return device;
    }
    ComPtr<IMMDeviceCollection> devices;
    UINT count = 0;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) devices->GetCount(&count);
    const auto needle = lower(query);
    ComPtr<IMMDevice> by_name;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> candidate;
        if (FAILED(devices->Item(i, &candidate))) continue;
        if (endpoint_id(candidate.Get()) == query) return candidate;
        if (!by_name && lower(endpoint_name(candidate.Get())).find(needle) != std::string::npos) by_name = candidate;
    }
    if (!by_name) error = "microphone not found: " + query + " (run `rec devices`)";
    return by_name;
}

ComPtr<IDXGIAdapter1> adapter_for(HMONITOR monitor) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) return adapter;
            output.Reset();
        }
        adapter.Reset();
    }
    return {};
}

// Seconds between the system mix that loopback captures and the default device playing it.
double output_latency_seconds() {
    ComScope com;
    auto enumerator = device_enumerator();
    ComPtr<IMMDevice> device;
    if (!enumerator || FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return 0;
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return 0;
    WAVEFORMATEX* format = nullptr;
    if (FAILED(client->GetMixFormat(&format))) return 0;
    REFERENCE_TIME latency = 0, period = 0, minimum = 0;
    if (SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, format, nullptr))) {
        client->GetStreamLatency(&latency);
        client->GetDevicePeriod(&period, &minimum);
    }
    CoTaskMemFree(format);
    return std::clamp(static_cast<double>(latency + period) / kTicks, 0.0, 0.5);
}

// Continuous linear resampler for interleaved stereo float, any rate to 48 kHz.
class Resampler {
public:
    void reset(std::uint32_t rate) {
        step_ = static_cast<double>(rate) / kMixRate;
        position_ = 0;
        previous_ = {0, 0};
    }
    void process(const float* in, std::size_t frames, std::vector<float>& out) {
        out.clear();
        if (!frames) return;
        // position_ is in input frames relative to this buffer; -1 is the previous buffer's last frame.
        for (; position_ < static_cast<double>(frames) - 1.0; position_ += step_) {
            const auto base = static_cast<std::ptrdiff_t>(std::floor(position_));
            const auto t = static_cast<float>(position_ - static_cast<double>(base));
            for (int c = 0; c < 2; ++c) {
                const float a = base < 0 ? previous_[c] : in[base * 2 + c];
                const float b = in[(base + 1) * 2 + c];
                out.push_back(a + (b - a) * t);
            }
        }
        position_ -= static_cast<double>(frames);
        previous_ = {in[(frames - 1) * 2], in[(frames - 1) * 2 + 1]};
    }

private:
    double step_ = 1;
    double position_ = 0;
    std::array<float, 2> previous_{};
};

struct AudioChunk {
    std::int64_t time = 0;  // QPC 100 ns units of the first frame.
    std::uint32_t frames = 0;
    std::array<float, kChunkFrames * 2> data{};
};

// A WASAPI capture stream (microphone, or loopback of the default output) on its own
// polling thread. Packets become 48 kHz stereo float chunks in a fixed 16-chunk queue;
// overflow drops the oldest chunk.
class AudioCapture {
public:
    ~AudioCapture() { stop(); }

    Result open(IMMDevice* device, bool loopback) {
        const DWORD mode = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        WAVEFORMATEXTENSIBLE wanted{};
        wanted.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wanted.Format.nChannels = 2;
        wanted.Format.nSamplesPerSec = kMixRate;
        wanted.Format.wBitsPerSample = 32;
        wanted.Format.nBlockAlign = 8;
        wanted.Format.nAvgBytesPerSec = kMixRate * 8;
        wanted.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wanted.Samples.wValidBitsPerSample = 32;
        wanted.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wanted.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
        if (FAILED(hr)) return {failed("could not open the audio device", hr)};
        // Let the audio engine convert to 48 kHz stereo float; fall back to the mix format.
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 mode | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 2'000'000, 0, &wanted.Format, nullptr);
        if (SUCCEEDED(hr)) {
            read_format(&wanted.Format);
        } else {
            client_.Reset();
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
            if (FAILED(hr)) return {failed("could not open the audio device", hr)};
            WAVEFORMATEX* mix = nullptr;
            hr = client_->GetMixFormat(&mix);
            if (FAILED(hr)) return {failed("could not read the audio format", hr)};
            const bool readable = read_format(mix);
            hr = readable ? client_->Initialize(AUDCLNT_SHAREMODE_SHARED, mode, 2'000'000, 0, mix, nullptr) : E_FAIL;
            CoTaskMemFree(mix);
            if (!readable) return {"unsupported audio device format"};
        }
        if (hr == E_ACCESSDENIED)
            return {"microphone access denied. Turn on \"Let desktop apps access your microphone\" in "
                    "Settings > Privacy & security > Microphone, then retry."};
        if (FAILED(hr)) return {failed("could not start audio capture", hr)};
        hr = client_->GetService(IID_PPV_ARGS(&capture_));
        if (FAILED(hr)) return {failed("could not open the audio capture service", hr)};
        if (loopback) {
            // Loopback delivers no packets while nothing plays, which stalls the movie's
            // audio clock. Playing silence on the same endpoint keeps packets flowing.
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &render_);
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(hr)) hr = render_->GetMixFormat(&mix);
            if (SUCCEEDED(hr)) hr = render_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2'000'000, 0, mix, nullptr);
            CoTaskMemFree(mix);
            if (SUCCEEDED(hr)) hr = render_->GetBufferSize(&render_frames_);
            if (SUCCEEDED(hr)) hr = render_->GetService(IID_PPV_ARGS(&renderer_));
            if (FAILED(hr)) return {failed("could not open the output device for system audio", hr)};
        }
        resampler_.reset(rate_);
        stereo_.reserve(kMixRate * 2);
        resampled_.reserve(kMixRate * 2);
        return {};
    }

    Result start() {
        stop_.store(false);
        HRESULT hr = S_OK;
        if (render_) {
            feed_silence();
            hr = render_->Start();
            if (FAILED(hr)) return {failed("could not start the output device for system audio", hr)};
        }
        hr = client_->Start();
        if (FAILED(hr)) return {failed("could not start audio capture", hr)};
        thread_ = std::thread([this] { run(); });
        return {};
    }

    void stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        if (client_) client_->Stop();
        if (render_) render_->Stop();
    }

    bool pop(AudioChunk& out) {
        std::lock_guard lock(mutex_);
        if (!count_) return false;
        out = ring_[head_];
        head_ = (head_ + 1) % ring_.size();
        --count_;
        return true;
    }

    std::uint64_t overflowed() const noexcept { return overflowed_.load(); }

private:
    bool read_format(const WAVEFORMATEX* format) {
        rate_ = format->nSamplesPerSec;
        channels_ = format->nChannels;
        bits_ = format->wBitsPerSample;
        block_align_ = format->nBlockAlign;
        if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            is_float_ = true;
        } else if (format->wFormatTag == WAVE_FORMAT_PCM) {
            is_float_ = false;
        } else if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
            const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
            if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) is_float_ = true;
            else if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) is_float_ = false;
            else return false;
        } else {
            return false;
        }
        return rate_ > 0 && channels_ > 0 && (is_float_ ? bits_ == 32 : (bits_ == 16 || bits_ == 24 || bits_ == 32));
    }

    float sample(const BYTE* frame, std::uint32_t channel) const {
        const BYTE* p = frame + std::min(channel, channels_ - 1) * (bits_ / 8);
        if (is_float_) { float v; std::memcpy(&v, p, 4); return v; }
        if (bits_ == 16) { std::int16_t v; std::memcpy(&v, p, 2); return v / 32768.0f; }
        if (bits_ == 24) {
            const std::int32_t v = static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) << 8 |
                                                             static_cast<std::uint32_t>(p[1]) << 16 |
                                                             static_cast<std::uint32_t>(p[2]) << 24) >> 8;
            return v / 8388608.0f;
        }
        std::int32_t v; std::memcpy(&v, p, 4); return v / 2147483648.0f;
    }

    void feed_silence() {
        UINT32 padding = 0;
        if (FAILED(render_->GetCurrentPadding(&padding)) || padding >= render_frames_) return;
        BYTE* data = nullptr;
        const UINT32 free = render_frames_ - padding;
        if (SUCCEEDED(renderer_->GetBuffer(free, &data))) renderer_->ReleaseBuffer(free, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    void push(std::int64_t time, const float* stereo, std::size_t frames) {
        std::lock_guard lock(mutex_);
        while (frames) {
            const auto n = std::min(frames, kChunkFrames);
            if (count_ == ring_.size()) {
                head_ = (head_ + 1) % ring_.size();
                --count_;
                overflowed_.fetch_add(1);
            }
            auto& chunk = ring_[(head_ + count_) % ring_.size()];
            chunk.time = time;
            chunk.frames = static_cast<std::uint32_t>(n);
            std::memcpy(chunk.data.data(), stereo, n * 2 * sizeof(float));
            ++count_;
            stereo += n * 2;
            frames -= n;
            time += static_cast<std::int64_t>(n) * kTicks / kMixRate;
        }
    }

    void run() {
        ComScope com;
        while (!stop_.load()) {
            Sleep(5);
            if (render_) feed_silence();
            for (;;) {
                UINT32 packet = 0;
                if (FAILED(capture_->GetNextPacketSize(&packet)) || !packet) break;
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 position = 0, qpc = 0;
                const HRESULT hr = capture_->GetBuffer(&data, &frames, &flags, &position, &qpc);
                if (FAILED(hr) || hr == AUDCLNT_S_BUFFER_EMPTY) break;
                const std::int64_t time = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || !qpc
                    ? now_ticks() - static_cast<std::int64_t>(frames) * kTicks / rate_
                    : static_cast<std::int64_t>(qpc);
                stereo_.resize(static_cast<std::size_t>(frames) * 2);
                for (UINT32 i = 0; i < frames; ++i) {
                    const BYTE* frame = data + static_cast<std::size_t>(i) * block_align_;
                    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data;
                    stereo_[i * 2] = silent ? 0.0f : sample(frame, 0);
                    stereo_[i * 2 + 1] = silent ? 0.0f : sample(frame, 1);
                }
                capture_->ReleaseBuffer(frames);
                if (rate_ == static_cast<std::uint32_t>(kMixRate)) {
                    push(time, stereo_.data(), frames);
                } else {
                    resampler_.process(stereo_.data(), frames, resampled_);
                    push(time, resampled_.data(), resampled_.size() / 2);
                }
            }
        }
    }

    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    ComPtr<IAudioClient> render_;
    ComPtr<IAudioRenderClient> renderer_;
    UINT32 render_frames_ = 0;
    std::uint32_t rate_ = kMixRate, channels_ = 2, bits_ = 32, block_align_ = 8;
    bool is_float_ = true;
    Resampler resampler_;
    std::vector<float> stereo_, resampled_;
    std::array<AudioChunk, 16> ring_{};
    std::size_t head_ = 0, count_ = 0;
    std::mutex mutex_;
    std::thread thread_;
    std::atomic_bool stop_{false};
    std::atomic<std::uint64_t> overflowed_{0};
};

enum Source : std::size_t { kSystem = 0, kMic = 1 };

struct AudioSource {
    bool enabled = false;
    std::unique_ptr<AudioCapture> capture;
    int stream = -1;                    // Stem stream in the sink writer, or -1.
    std::uint32_t stem_channels = 2;
    std::int64_t offset = 0;            // 100 ns units.
    core::Fader fader;                  // Volume and mute of the program audio.
    std::int64_t next = INT64_MIN;      // Next 48 kHz sample index; keeps the source continuous.
    std::atomic<std::uint64_t> written{0}, dropped{0};
    mutable std::atomic<float> peak{0};  // Level meter; stats() takes it.
};

enum class SlotState { free, filling, pending, current };

struct VideoSlot {
    ComPtr<ID3D11Texture2D> texture;  // NV12 at the output size.
    ComPtr<ID3D11VideoProcessorOutputView> view;
    std::int64_t time = 0;
    SlotState state = SlotState::free;
};

ComPtr<IMFMediaType> video_type(const GUID& subtype, Size size, std::uint32_t fps) {
    ComPtr<IMFMediaType> type;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, size.width, size.height);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    return type;
}

ComPtr<IMFMediaType> audio_type(const GUID& subtype, std::uint32_t channels, std::uint32_t bytes_per_second) {
    ComPtr<IMFMediaType> type;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kMixRate);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytes_per_second);
    if (subtype == MFAudioFormat_PCM) {
        type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
        type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    } else {
        type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    }
    return type;
}

ComPtr<IMFMediaBuffer> pattern_frame(Size size, bool flash) {
    const DWORD bytes = size.width * size.height * 3 / 2;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buffer))) return nullptr;
    BYTE* data = nullptr;
    if (FAILED(buffer->Lock(&data, nullptr, nullptr))) return nullptr;
    for (std::uint32_t row = 0; row < size.height; ++row)
        std::memset(data + static_cast<std::size_t>(row) * size.width, flash ? 235 : (row % 24 < 12 ? 120 : 40), size.width);
    std::memset(data + static_cast<std::size_t>(size.width) * size.height, 128, static_cast<std::size_t>(size.width) * size.height / 2);
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);
    return buffer;
}
} // namespace

std::vector<Display> list_displays(std::string& error) {
    error.clear();
    std::vector<Display> result;
    const auto list = monitors();
    for (std::size_t i = 0; i < list.size(); ++i)
        result.push_back({static_cast<std::uint32_t>(i + 1), list[i].width, list[i].height,
                          "Display " + std::to_string(i + 1) + (list[i].primary ? " (main) " : " ") + list[i].device});
    if (result.empty()) error = "no displays found";
    return result;
}

std::vector<Window> list_windows(std::string& error) {
    error.clear();
    // Physical pixels whatever the caller's DPI awareness, as capture delivers them.
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::vector<Window> result;
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        if (!capturable(window)) return TRUE;
        RECT bounds{};
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof bounds)))
            GetWindowRect(window, &bounds);
        if (bounds.right - bounds.left < 2 || bounds.bottom - bounds.top < 2) return TRUE;
        const int length = GetWindowTextLengthW(window);
        std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
        title.resize(static_cast<std::size_t>(GetWindowTextW(window, title.data(), length + 1)));
        reinterpret_cast<std::vector<Window>*>(data)->push_back(
            {static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(window)),
             static_cast<std::uint32_t>(bounds.right - bounds.left), static_cast<std::uint32_t>(bounds.bottom - bounds.top),
             utf8(title.c_str()), process_name(window)});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    if (previous) SetThreadDpiAwarenessContext(previous);
    return result;
}

std::vector<AudioDevice> list_microphones(std::string& error) {
    ComScope com;
    error.clear();
    std::vector<AudioDevice> result;
    auto enumerator = device_enumerator();
    ComPtr<IMMDeviceCollection> devices;
    UINT count = 0;
    if (!enumerator || FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) {
        error = "could not list microphones";
        return result;
    }
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(devices->Item(i, &device)))
            result.push_back({endpoint_id(device.Get()), endpoint_name(device.Get())});
    }
    return result;
}

std::vector<Camera> list_cameras(std::string& error) {
    ComScope com;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { error = "Media Foundation is unavailable"; return {}; }
    auto result = win::cameras(error);
    MFShutdown();
    return result;
}

double default_output_latency_ms() noexcept {
    try { return output_latency_seconds() * 1000.0; } catch (...) { return 0; }
}

// Capture callbacks only convert and enqueue. One recording thread writes video on a
// constant-frame-rate grid, routes audio to stems and the mix, and never blocks a callback.
class CaptureState {
public:
    explicit CaptureState(Options options) : options_(std::move(options)) {}
    ~CaptureState() { stop(); }

    Result start() {
        ComScope com;
        if (active_) return {"already recording"};
        window_closed_.store(false);
        if (options_.preview_only) {
            if (!options_.preview_width) return {"a preview needs a preview width"};
            options_.system_audio = options_.microphone = options_.test_tone = options_.test_mic = false;
            options_.stream_url.clear();
            options_.replay_seconds = 0;
            options_.duration_seconds = 0;
        }
        if (options_.fps == 0 || options_.fps > 120) return {"invalid frame rate"};
        if ((options_.test_tone || options_.test_mic) && !options_.test_pattern)
            return {"synthetic audio needs the test pattern"};
        if ((options_.system_audio || options_.microphone) && options_.test_pattern)
            return {"system audio and microphone need a real screen capture"};
        if ((options_.window || options_.region) && options_.test_pattern)
            return {"window and region capture need a real screen capture"};
        if (options_.window && options_.region) return {"capture a window or a region, not both"};
        if (!options_.preview_only) {
            if (options_.output.empty()) return {"output path is empty"};
            const auto path = wide(options_.output);
            if (path.empty()) return {"invalid output path"};
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                return {"output file already exists: " + options_.output};
        }
        const HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(mf)) return {failed("Media Foundation is unavailable", mf)};
        mf_started_ = true;

        audio_[kSystem].enabled = options_.system_audio || options_.test_tone;
        audio_[kMic].enabled = options_.microphone || options_.test_mic;
        system_offset_ms_ = options_.system_audio_offset_ms.value_or(
            options_.test_pattern ? 0.0 : output_latency_seconds() * 1000.0);
        audio_[kSystem].offset = static_cast<std::int64_t>(std::llround(system_offset_ms_ * kTicks / 1000.0));
        audio_[kMic].offset = static_cast<std::int64_t>(std::llround(options_.mic_offset_ms * kTicks / 1000.0));
        audio_[kSystem].fader.reset(core::gain_of(options_.system_gain_db, options_.system_muted));
        audio_[kMic].fader.reset(core::gain_of(options_.mic_gain_db, options_.mic_muted));

        if (options_.test_pattern) {
            size_ = {(options_.width ? options_.width : 1920u) & ~1u, (options_.height ? options_.height : 1080u) & ~1u};
            if (size_.width < 2 || size_.height < 2) { stop(); return {"invalid synthetic frame dimensions"}; }
            pattern_[0] = pattern_frame(size_, false);
            pattern_[1] = pattern_frame(size_, true);
            if (!pattern_[0] || !pattern_[1]) { stop(); return {"synthetic frame allocation failed"}; }
            if (!options_.overlays.empty() || options_.preview_width)
                if (auto created = create_pattern_device(); !created) { stop(); return created; }
            captured_.store(1);
        } else {
            if (auto opened = open_audio(); !opened) { stop(); return opened; }
            if (auto started = start_capture(); !started) { stop(); return started; }
        }
        if (!options_.overlays.empty()) {
            overlays_ = std::make_unique<win::Overlays>();
            if (auto started = overlays_->start(device_.Get(), size_.width, size_.height, options_.overlays); !started) {
                stop();
                return started;
            }
        }
        if (options_.preview_width && device_) {
            const auto size = core::preview_size(size_.width, size_.height, options_.preview_width);
            previewer_ = std::make_unique<win::FramePreview>();
            // A driver that cannot draw the preview still records.
            if (previewer_->start(device_.Get(), size_.width, size_.height, size)) preview_.reset(size);
            else previewer_.reset();
        }
        if (options_.preview_only) {
            if (!previewer_) { stop(); return {"this graphics driver cannot draw the preview"}; }
            // Frames are drawn here instead of into the encoder's pool.
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = size_.width;
            desc.Height = size_.height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_NV12;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &composed_); FAILED(hr)) {
                stop();
                return {failed("could not allocate the preview frame", hr)};
            }
        }
        if (!options_.stream_url.empty() || options_.replay_seconds) {
            if (options_.stream_kbps < 500 || options_.stream_kbps > 50000) {
                stop();
                return {"stream bitrate must be between 500 and 50000 kb/s"};
            }
            if (options_.replay_seconds && (options_.replay_seconds < 5 || options_.replay_seconds > 300)) {
                stop();
                return {"replay must be between 5 and 300 seconds"};
            }
            win::LiveSettings live;
            live.url = options_.stream_url;
            live.replay_seconds = options_.replay_seconds;
            live.width = size_.width;
            live.height = size_.height;
            live.fps = options_.fps;
            live.video_kbps = options_.stream_kbps;
            live.manager = manager_.Get();
            live.vendor = vendor_;
            live_ = std::make_unique<win::LiveStream>();
            if (auto started = live_->start(live); !started) {
                stop();
                return {(options_.stream_url.empty() ? "replay: " : "stream: ") + started.error};
            }
        }
        active_.store(true);
        if (!options_.preview_only)
            if (auto ready = create_writer(); !ready) { stop(); return ready; }
        finished_.store(false);
        stop_requested_.store(false);
        origin_ = now_ticks();
        timeline_.reset(origin_);
        audio_cap_ = options_.duration_seconds > 0
            ? std::llround(options_.duration_seconds * options_.fps) * kMixRate / options_.fps : INT64_MAX;
        begin_ = std::chrono::steady_clock::now();
        recording_.store(true);
        for (auto& source : audio_) {
            if (!source.capture) continue;
            if (auto started = source.capture->start(); !started) { stop(); return started; }
        }
        pacer_ = std::thread([this] { run(); });
        return {};
    }

    Result stop() {
        ComScope com;
        if (pacer_.joinable()) { stop_requested_.store(true); pacer_.join(); }
        for (auto& source : audio_)
            if (source.capture) source.capture->stop();
        if (session_ || pool_) {
            {
                std::lock_guard guard(callback_mutex_);
                accepting_ = false;
            }
            try {
                if (pool_) pool_.FrameArrived(frame_token_);
                if (session_) session_.Close();
                if (pool_) pool_.Close();
            } catch (...) {
            }
            session_ = nullptr;
            pool_ = nullptr;
            item_ = nullptr;
        }
        Result result;
        if (writer_) {
            if (writing_) {
                pump_audio(true);
                const HRESULT hr = writer_->Finalize();
                if (FAILED(hr)) result = {failed("could not finalize the recording", hr)};
            }
            writing_ = false;
            writer_.Reset();
        }
        if (live_) live_->stop();
        if (allocator_) { allocator_->UninitializeSampleAllocator(); allocator_.Reset(); }
        if (overlays_) {
            camera_frames_ = overlays_->camera_frames();
            overlays_->stop();
            overlays_.reset();
        }
        previewer_.reset();
        composed_.Reset();
        for (auto& slot : slots_) slot = {};
        pending_count_ = 0;
        current_ = -1;
        for (auto& buffer : pattern_) buffer.Reset();
        input_view_.Reset();
        intermediate_.Reset();
        processor_.Reset();
        processor_enum_.Reset();
        video_context_.Reset();
        video_device_.Reset();
        manager_.Reset();
        winrt_device_ = nullptr;
        context_.Reset();
        device_.Reset();
        for (auto& source : audio_) source.capture.reset();
        if (mf_started_) { MFShutdown(); mf_started_ = false; }
        recording_.store(false);
        active_ = false;
        return result;
    }

    bool running() const noexcept { return active_ && !finished_.load(); }
    bool finished() const noexcept { return finished_.load(); }
    Stats stats() const noexcept {
        Stats stats;
        stats.captured = captured_.load();
        stats.written = written_.load();
        stats.dropped = dropped_.load();
        stats.audio_written = audio_[kSystem].written.load();
        stats.audio_dropped = audio_[kSystem].dropped.load() +
            (audio_[kSystem].capture ? audio_[kSystem].capture->overflowed() : 0);
        stats.mic_written = audio_[kMic].written.load();
        stats.mic_dropped = audio_[kMic].dropped.load() +
            (audio_[kMic].capture ? audio_[kMic].capture->overflowed() : 0);
        stats.mix_written = mix_written_.load();
        stats.system_peak = audio_[kSystem].peak.exchange(0);
        stats.mic_peak = audio_[kMic].peak.exchange(0);
        stats.system_audio_offset_ms = audio_[kSystem].enabled ? system_offset_ms_ : 0;
        stats.paused = timeline_.paused();
        stats.paused_seconds = static_cast<double>(timeline_.paused_total(now_ticks())) / kTicks;
        stats.window_closed = window_closed_.load();
        if (live_) {
            const auto live = live_->counters();
            stats.streaming = live.live;
            stats.stream_reconnecting = live.reconnecting;
            stats.stream_reconnects = live.reconnects;
            stats.stream_hardware = live.hardware;
            stats.stream_frames = live.frames;
            stats.stream_dropped = live.dropped;
            stats.stream_bytes = live.bytes;
            stats.stream_queued_ms = live.queued_ms;
            stats.replay_seconds = live.replay_seconds;
        }
        stats.camera_frames = overlays_ ? overlays_->camera_frames() : camera_frames_;
        return stats;
    }
    std::string failure() const { std::lock_guard lock(mutex_); return failure_; }
    std::string stream_failure() const { return live_ ? live_->failure() : std::string(); }
    Result save_replay(const std::string& path, double& seconds) const {
        seconds = 0;
        if (!live_ || !options_.replay_seconds) return {"the replay buffer is off"};
        return live_->save_replay(path, seconds);
    }
    Result set_overlay(std::size_t index, bool visible, const std::optional<Area>& area) {
        if (!overlays_) return {"no overlay " + std::to_string(index + 1)};
        return overlays_->set(index, visible, area);
    }
    Result set_volume(AudioInput input, double gain_db, bool muted) {
        if (!(gain_db >= -60 && gain_db <= 24)) return {"volume must be between -60 and 24 dB"};
        auto& source = audio_[input == AudioInput::system ? kSystem : kMic];
        if (!recording_.load() || !source.enabled)
            return {input == AudioInput::system ? "system audio is not being recorded" : "the microphone is not being recorded"};
        source.fader.set(core::gain_of(gain_db, muted));
        return {};
    }

    bool preview(PreviewFrame& frame) const { return preview_.take(frame); }
    void show_preview(bool on) { preview_.show(on); }
    Result pause() {
        if (!recording_.load() || finished_.load()) return {"not recording"};
        if (!timeline_.pause(now_ticks())) return {"already paused"};
        return {};
    }
    Result resume() {
        if (!recording_.load() || finished_.load()) return {"not recording"};
        if (!timeline_.resume(now_ticks())) return {"not paused"};
        return {};
    }

private:
    void fail(std::string text) {
        {
            std::lock_guard lock(mutex_);
            if (failure_.empty()) failure_ = std::move(text);
        }
        finished_.store(true);
        frames_ready_.notify_one();
    }

    Result open_audio() {
        if (!options_.system_audio && !options_.microphone) return {};
        auto enumerator = device_enumerator();
        if (!enumerator) return {"could not open the Windows audio device list"};
        if (options_.system_audio) {
            ComPtr<IMMDevice> output;
            if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &output)))
                return {"no audio output device for system audio"};
            audio_[kSystem].capture = std::make_unique<AudioCapture>();
            if (auto opened = audio_[kSystem].capture->open(output.Get(), true); !opened)
                return {"system audio: " + opened.error};
        }
        if (options_.microphone) {
            std::string error;
            auto device = find_microphone(options_.microphone_device, error);
            if (!device) return {error};
            audio_[kMic].capture = std::make_unique<AudioCapture>();
            if (auto opened = audio_[kMic].capture->open(device.Get(), false); !opened) return opened;
        }
        return {};
    }

    // The synthetic frames as NV12 textures on a default device, so overlays draw on the GPU
    // exactly as they do over a capture. Machines without a GPU get WARP.
    Result create_pattern_device() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = E_FAIL;
        for (const auto type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
            for (const UINT flags : {static_cast<UINT>(D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT),
                                     static_cast<UINT>(D3D11_CREATE_DEVICE_BGRA_SUPPORT)}) {
                hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &device_, nullptr, &context_);
                if (SUCCEEDED(hr)) break;
            }
            if (SUCCEEDED(hr)) break;
        }
        if (FAILED(hr)) return {failed("could not create a Direct3D 11 device", hr)};
        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(context_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
        hr = MFCreateDXGIDeviceManager(&reset_token_, &manager_);
        if (SUCCEEDED(hr)) hr = manager_->ResetDevice(device_.Get(), reset_token_);
        if (FAILED(hr)) return {failed("could not share the device with Media Foundation", hr)};
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = size_.width;
        desc.Height = size_.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        for (int i = 0; i < 2; ++i) {
            BYTE* data = nullptr;
            if (FAILED(pattern_[i]->Lock(&data, nullptr, nullptr))) return {"synthetic frame allocation failed"};
            const D3D11_SUBRESOURCE_DATA initial{data, size_.width, 0};
            hr = device_->CreateTexture2D(&desc, &initial, &slots_[i].texture);
            pattern_[i]->Unlock();
            if (FAILED(hr)) return {failed("could not allocate video frames", hr)};
        }
        return {};
    }

    Result start_capture() {
        try {
            if (!wgc::GraphicsCaptureSession::IsSupported()) return {"screen capture needs Windows 10 version 1903 or newer"};
        } catch (...) {
            return {"screen capture needs Windows 10 version 1903 or newer"};
        }
        HWND window = nullptr;
        HMONITOR monitor = nullptr;
        if (options_.window) {
            window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(options_.window));
            if (!IsWindow(window)) return {"window not found; run `rec windows`"};
            if (IsIconic(window)) return {"the window is minimized; restore it, then retry"};
            monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        } else {
            const auto list = monitors();
            if (options_.display == 0 || options_.display > list.size()) return {"display index not found; run `rec devices`"};
            monitor = list[options_.display - 1].handle;
        }

        // Capture, scale, and encode on the GPU that scans out the monitor, so frames never
        // cross adapters on hybrid systems.
        const auto adapter = adapter_for(monitor);
        DXGI_ADAPTER_DESC1 adapter_desc{};
        if (adapter && SUCCEEDED(adapter->GetDesc1(&adapter_desc))) vendor_ = adapter_desc.VendorId;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                       levels, 2, D3D11_SDK_VERSION, &device_, nullptr, &context_);
        if (FAILED(hr)) return {failed("could not create a Direct3D 11 video device", hr)};
        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(context_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
        if (FAILED(device_.As(&video_device_)) || FAILED(context_.As(&video_context_)))
            return {"the graphics driver has no video processor"};
        hr = MFCreateDXGIDeviceManager(&reset_token_, &manager_);
        if (SUCCEEDED(hr)) hr = manager_->ResetDevice(device_.Get(), reset_token_);
        if (FAILED(hr)) return {failed("could not share the device with Media Foundation", hr)};

        try {
            ComPtr<IDXGIDevice> dxgi;
            device_.As(&dxgi);
            winrt::com_ptr<::IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
            winrt_device_ = inspectable.as<wgdx::Direct3D11::IDirect3DDevice>();
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            if (window)
                winrt::check_hresult(interop->CreateForWindow(window, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                             winrt::put_abi(item_)));
            else
                winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                              winrt::put_abi(item_)));
        } catch (winrt::hresult_error const& e) {
            return {failed(window ? "could not capture the window" : "could not capture the display", e.code())};
        }
        const auto capture_size = item_.Size();
        if (capture_size.Width < 2 || capture_size.Height < 2)
            return {window ? "the window has no usable size" : "display has no usable pixel dimensions"};
        auto source_w = static_cast<std::uint32_t>(capture_size.Width);
        auto source_h = static_cast<std::uint32_t>(capture_size.Height);
        if (options_.region) {
            const auto& r = *options_.region;
            if (r.x < 0 || r.y < 0 || r.width < 2 || r.height < 2 || r.x + r.width > source_w || r.y + r.height > source_h)
                return {"region " + std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.width) + "," +
                        std::to_string(r.height) + " does not fit display " + std::to_string(options_.display) + " (" +
                        std::to_string(source_w) + "x" + std::to_string(source_h) + ")"};
            source_w = r.width;
            source_h = r.height;
        }
        size_ = output_size(options_, source_w, source_h);
        if (auto ready = create_processor(capture_size); !ready) return ready;

        try {
            try {
                wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get();
            } catch (...) {
            }
            pool_size_ = capture_size;
            pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrt_device_, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, capture_size);
            accepting_ = true;
            frame_token_ = pool_.FrameArrived({this, &CaptureState::on_frame});
            session_ = pool_.CreateCaptureSession(item_);
            try { session_.IsCursorCaptureEnabled(options_.cursor); } catch (...) {}
            try { session_.IsBorderRequired(false); } catch (...) {}
            session_.StartCapture();
        } catch (winrt::hresult_error const& e) {
            return {failed("could not start screen capture", e.code())};
        }
        std::unique_lock lock(mutex_);
        if (!frames_ready_.wait_for(lock, std::chrono::seconds(8),
                                    [this] { return pending_count_ > 0 || !failure_.empty(); }))
            return {"no screen frames arrived within 8 seconds"};
        if (!failure_.empty()) return {failure_};
        return {};
    }

    Result create_processor(winrt::Windows::Graphics::SizeInt32 input) {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {options_.fps, 1};
        content.InputWidth = static_cast<UINT>(input.Width);
        content.InputHeight = static_cast<UINT>(input.Height);
        content.OutputFrameRate = {options_.fps, 1};
        content.OutputWidth = size_.width;
        content.OutputHeight = size_.height;
        content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
        HRESULT hr = video_device_->CreateVideoProcessorEnumerator(&content, &processor_enum_);
        if (SUCCEEDED(hr)) hr = video_device_->CreateVideoProcessor(processor_enum_.Get(), 0, &processor_);
        if (FAILED(hr)) return {failed("could not create the GPU scaler", hr)};
        UINT support = 0;
        if (FAILED(processor_enum_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support)) ||
            !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
            return {"the GPU scaler cannot output NV12"};
        video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
        video_context_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        ComPtr<ID3D11VideoContext1> context1;
        if (SUCCEEDED(video_context_.As(&context1))) {
            context1->VideoProcessorSetStreamColorSpace1(processor_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            context1->VideoProcessorSetOutputColorSpace1(processor_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        } else {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
            in.RGB_Range = 0;
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
            out.YCbCr_Matrix = 1;
            out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &in);
            video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &out);
        }
        const RECT target{0, 0, static_cast<LONG>(size_.width), static_cast<LONG>(size_.height)};
        video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &target);
        video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &target);
        // A window resized to another shape is letterboxed on black.
        D3D11_VIDEO_COLOR black{};
        black.RGBA = {0.0f, 0.0f, 0.0f, 1.0f};
        video_context_->VideoProcessorSetOutputBackgroundColor(processor_.Get(), FALSE, &black);

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = size_.width;
        desc.Height = size_.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};
        view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        for (auto& slot : slots_) {
            hr = device_->CreateTexture2D(&desc, nullptr, &slot.texture);
            if (SUCCEEDED(hr))
                hr = video_device_->CreateVideoProcessorOutputView(slot.texture.Get(), processor_enum_.Get(), &view, &slot.view);
            if (FAILED(hr)) return {failed("could not allocate video frames", hr)};
        }
        return {};
    }

    // Scales and converts one captured BGRA surface into an NV12 slot on the GPU.
    bool convert(ID3D11Texture2D* surface, wgc::Direct3D11CaptureFrame const& frame, int slot) {
        D3D11_TEXTURE2D_DESC desc;
        surface->GetDesc(&desc);
        if (!intermediate_ || desc.Width != intermediate_size_.width || desc.Height != intermediate_size_.height) {
            input_view_.Reset();
            intermediate_.Reset();
            D3D11_TEXTURE2D_DESC copy = desc;
            copy.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            copy.MiscFlags = 0;
            copy.CPUAccessFlags = 0;
            copy.Usage = D3D11_USAGE_DEFAULT;
            if (FAILED(device_->CreateTexture2D(&copy, nullptr, &intermediate_))) return false;
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view{};
            view.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            if (FAILED(video_device_->CreateVideoProcessorInputView(intermediate_.Get(), processor_enum_.Get(), &view,
                                                                   &input_view_))) {
                intermediate_.Reset();
                return false;
            }
            intermediate_size_ = {desc.Width, desc.Height};
        }
        // Capture-pool surfaces are only valid until the frame closes; copy before converting.
        context_->CopyResource(intermediate_.Get(), surface);
        const auto content = frame.ContentSize();
        RECT source{0, 0, static_cast<LONG>(std::min<UINT>(desc.Width, static_cast<UINT>(content.Width))),
                    static_cast<LONG>(std::min<UINT>(desc.Height, static_cast<UINT>(content.Height)))};
        if (options_.region) {
            const auto& r = *options_.region;
            source = {r.x, r.y, std::min(source.right, static_cast<LONG>(r.x + static_cast<LONG>(r.width))),
                      std::min(source.bottom, static_cast<LONG>(r.y + static_cast<LONG>(r.height)))};
        }
        if (source.right - source.left < 2 || source.bottom - source.top < 2) return false;
        video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &source);
        const RECT dest = fit(source.right - source.left, source.bottom - source.top, size_.width, size_.height);
        video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &dest);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input_view_.Get();
        return SUCCEEDED(video_context_->VideoProcessorBlt(processor_.Get(), slots_[slot].view.Get(), 0, 1, &stream));
    }

    void on_frame(wgc::Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
        std::lock_guard guard(callback_mutex_);
        if (!accepting_) return;
        try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;
            const std::int64_t time = frame.SystemRelativeTime().count();
            const auto content = frame.ContentSize();
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> surface;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&surface)));
            int slot = -1;
            {
                std::lock_guard lock(mutex_);
                for (int i = 0; i < static_cast<int>(slots_.size()) && slot < 0; ++i)
                    if (slots_[i].state == SlotState::free) slot = i;
                if (slot >= 0) slots_[slot].state = SlotState::filling;
            }
            if (slot >= 0) {
                const bool converted = convert(surface.Get(), frame, slot);
                std::lock_guard lock(mutex_);
                if (!converted) {
                    slots_[slot].state = SlotState::free;
                } else {
                    slots_[slot].time = time;
                    slots_[slot].state = SlotState::pending;
                    if (pending_count_ == pending_.size()) {
                        // A newer frame supersedes the oldest pending one.
                        slots_[pending_[pending_head_]].state = SlotState::free;
                        pending_head_ = (pending_head_ + 1) % pending_.size();
                        --pending_count_;
                    }
                    pending_[(pending_head_ + pending_count_) % pending_.size()] = slot;
                    ++pending_count_;
                    captured_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            frame.Close();
            frames_ready_.notify_one();
            if (content.Width != pool_size_.Width || content.Height != pool_size_.Height) {
                pool_size_ = content;
                sender.Recreate(winrt_device_, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, content);
            }
        } catch (...) {
        }
    }

    // The frame on screen at the slot's instant: the newest converted frame whose
    // timestamp is not after it.
    int select_frame(std::uint64_t slot, std::int64_t due) {
        if (options_.test_pattern) return core::test_flash(slot, options_.fps) ? 1 : 0;
        std::lock_guard lock(mutex_);
        while (pending_count_ && (current_ < 0 || slots_[pending_[pending_head_]].time <= due)) {
            if (current_ >= 0) slots_[current_].state = SlotState::free;
            current_ = pending_[pending_head_];
            slots_[current_].state = SlotState::current;
            pending_head_ = (pending_head_ + 1) % pending_.size();
            --pending_count_;
        }
        return current_;
    }

    Result create_writer() {
        ComPtr<IMFAttributes> attributes;
        MFCreateAttributes(&attributes, 3);
        attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        // Fragmented MP4: a crash or power loss leaves every completed fragment playable.
        // Windows' fragmented sink takes H.264 only, so HEVC uses the regular MP4 sink.
        // A .mov output is the same ISO structure, which QuickTime reads.
        const bool hevc = options_.codec == Codec::hevc ||
            (options_.codec == Codec::automatic && size_.height > 1080);
        attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, hevc ? MFTranscodeContainerType_MPEG4 : MFTranscodeContainerType_FMPEG4);
        if (manager_) attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, manager_.Get());
        HRESULT hr = MFCreateSinkWriterFromURL(wide(options_.output).c_str(), nullptr, attributes.Get(), &writer_);
        if (FAILED(hr)) return {failed("could not create output movie", hr)};

        // The MP4 sink stores tracks in reverse order of AddStream. Add the microphone stem,
        // system stem, mix, then video, so the file reads video, mix (the default audio
        // track players pick), system, microphone.
        const bool system = audio_[kSystem].enabled, mic = audio_[kMic].enabled;
        const bool file_mix = system && mic && options_.audio_tracks != AudioTracks::stems;
        // A stream always carries the mix, even when the file keeps only stems.
        mixing_ = file_mix || (system && mic && live_);
        const bool stems = !file_mix || options_.audio_tracks == AudioTracks::both;
        auto add_audio = [&](std::uint32_t channels, std::uint32_t bytes_per_second, int& index) -> Result {
            DWORD added = 0;
            HRESULT result = writer_->AddStream(audio_type(MFAudioFormat_AAC, channels, bytes_per_second).Get(), &added);
            if (SUCCEEDED(result))
                result = writer_->SetInputMediaType(added, audio_type(MFAudioFormat_PCM, channels, kMixRate * channels * 2).Get(), nullptr);
            if (FAILED(result)) return {failed("the AAC encoder cannot accept the audio", result)};
            index = static_cast<int>(added);
            return {};
        };
        if (mic && stems) {
            audio_[kMic].stem_channels = 1;
            if (auto added = add_audio(1, 16000, audio_[kMic].stream); !added) return added;
        }
        if (system && stems) {
            if (auto added = add_audio(2, 24000, audio_[kSystem].stream); !added) return added;
        }
        if (file_mix) {
            if (auto added = add_audio(2, 24000, mix_stream_); !added) return added;
        }
        if (mixing_) mixer_.reset(static_cast<std::size_t>(kMixRate) * 2);

        const std::uint32_t bitrate = (options_.bitrate_mbps ? options_.bitrate_mbps
            : (size_.height > 1440 ? 50u : size_.height > 1080 ? 30u : 20u)) * 1000000u;
        auto output = video_type(hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264, size_, options_.fps);
        output->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        output->SetUINT32(MF_MT_MPEG2_PROFILE, hevc ? eAVEncH265VProfile_Main_420_8 : eAVEncH264VProfile_High);
        DWORD stream = 0;
        hr = writer_->AddStream(output.Get(), &stream);
        if (FAILED(hr)) return {failed(hevc ? "no HEVC encoder is available" : "no H.264 encoder is available", hr)};
        video_stream_ = stream;
        auto input = video_type(MFVideoFormat_NV12, size_, options_.fps);
        input->SetUINT32(MF_MT_DEFAULT_STRIDE, size_.width);
        ComPtr<IMFAttributes> encoder;
        MFCreateAttributes(&encoder, 2);
        encoder->SetUINT32(CODECAPI_AVEncMPVGOPSize, options_.fps * 2);
        encoder->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        hr = writer_->SetInputMediaType(video_stream_, input.Get(), encoder.Get());
        if (FAILED(hr)) return {failed(hevc ? "the HEVC encoder cannot accept the selected settings"
                                            : "the H.264 encoder cannot accept the selected settings", hr)};

        if (device_) {
            hr = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator_));
            if (SUCCEEDED(hr)) hr = allocator_->SetDirectXManager(manager_.Get());
            ComPtr<IMFAttributes> pool;
            MFCreateAttributes(&pool, 1);
            pool->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET);
            // The stream encoder holds frames too.
            if (SUCCEEDED(hr)) hr = allocator_->InitializeSampleAllocatorEx(2, live_ ? 16 : 8, pool.Get(), input.Get());
            if (FAILED(hr)) return {failed("could not allocate encoder frames", hr)};
        }
        hr = writer_->BeginWriting();
        if (FAILED(hr)) return {failed("could not start writing the movie", hr)};
        writing_ = true;
        return {};
    }

    void draw_preview(int frame) {
        context_->CopySubresourceRegion(composed_.Get(), 0, 0, 0, 0, slots_[frame].texture.Get(), 0, nullptr);
        if (overlays_ && !overlays_->draw(context_.Get(), composed_.Get(), 0)) {
            fail("could not draw the overlays");
            return;
        }
        previewer_->render(context_.Get(), composed_.Get(), 0, preview_);
    }

    enum class Write { done, busy, failed };

    Write write_video(std::uint64_t slot, int frame) {
        ComPtr<IMFSample> sample;
        if (!allocator_) {
            if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(pattern_[frame].Get()))) {
                fail("could not create a video sample");
                return Write::failed;
            }
        } else {
            const HRESULT hr = allocator_->AllocateSample(&sample);
            // Every pooled frame is still inside the encoder: transient backpressure.
            if (hr == MF_E_SAMPLEALLOCATOR_EMPTY) return Write::busy;
            if (FAILED(hr)) { fail(failed("could not get an encoder frame", hr)); return Write::failed; }
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMFDXGIBuffer> dxgi;
            ComPtr<ID3D11Texture2D> target;
            UINT subresource = 0;
            if (FAILED(sample->GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&dxgi)) ||
                FAILED(dxgi->GetResource(IID_PPV_ARGS(&target))) || FAILED(dxgi->GetSubresourceIndex(&subresource))) {
                fail("could not map an encoder frame");
                return Write::failed;
            }
            context_->CopySubresourceRegion(target.Get(), subresource, 0, 0, 0, slots_[frame].texture.Get(), 0, nullptr);
            if (overlays_ && !overlays_->draw(context_.Get(), target.Get(), subresource)) {
                fail("could not draw the overlays");
                return Write::failed;
            }
            if (previewer_ && preview_.due(slot, options_.fps))
                previewer_->render(context_.Get(), target.Get(), subresource, preview_);
            ComPtr<IMF2DBuffer> planar;
            DWORD length = 0;
            if (SUCCEEDED(buffer.As(&planar)) && SUCCEEDED(planar->GetContiguousLength(&length)))
                buffer->SetCurrentLength(length);
        }
        sample->SetSampleTime(static_cast<std::int64_t>(slot) * kTicks / options_.fps);
        sample->SetSampleDuration(kTicks / options_.fps);
        const HRESULT hr = writer_->WriteSample(video_stream_, sample.Get());
        if (FAILED(hr)) { fail(failed("video encoding failed", hr)); return Write::failed; }
        if (live_) live_->video(sample.Get());
        return Write::done;
    }

    // Writes interleaved stereo float (or silence when data is null) as 16-bit PCM.
    bool write_pcm(int stream, const float* data, std::size_t frames, std::uint32_t channels, std::int64_t start) {
        while (frames) {
            const auto n = std::min<std::size_t>(frames, kMixRate / 10);
            const DWORD bytes = static_cast<DWORD>(n * channels * sizeof(std::int16_t));
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMFSample> sample;
            BYTE* out = nullptr;
            if (FAILED(MFCreateMemoryBuffer(bytes, &buffer)) || FAILED(buffer->Lock(&out, nullptr, nullptr))) return false;
            auto* pcm = reinterpret_cast<std::int16_t*>(out);
            auto clip = [](float v) {
                return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 32767.0f / 32768.0f) * 32768.0f));
            };
            for (std::size_t i = 0; i < n; ++i) {
                const float left = data ? data[i * 2] : 0.0f, right = data ? data[i * 2 + 1] : 0.0f;
                if (channels == 1) pcm[i] = clip((left + right) * 0.5f);
                else { pcm[i * 2] = clip(left); pcm[i * 2 + 1] = clip(right); }
            }
            buffer->Unlock();
            buffer->SetCurrentLength(bytes);
            if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer.Get()))) return false;
            sample->SetSampleTime(start * kTicks / kMixRate);
            sample->SetSampleDuration(static_cast<std::int64_t>(n) * kTicks / kMixRate);
            const HRESULT hr = writer_->WriteSample(static_cast<DWORD>(stream), sample.Get());
            if (FAILED(hr)) { fail(failed("audio encoding failed", hr)); return false; }
            if (data) data += n * 2;
            frames -= n;
            start += static_cast<std::int64_t>(n);
        }
        return true;
    }

    // Places one chunk of 48 kHz stereo on the recording timeline: its stem (with silence
    // filling any gap, so tracks stay continuous) and the mix.
    void place(Source kind, std::int64_t start, const float* data, std::size_t frames) {
        auto& source = audio_[kind];
        // Timestamps jitter by a sample or two; keep a continuous stream continuous.
        if (source.next != INT64_MIN && std::llabs(start - source.next) <= kMixRate / 1000) start = source.next;
        const std::int64_t floor = source.next == INT64_MIN ? 0 : source.next;
        if (start < floor) {
            const auto skip = static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(frames), floor - start));
            data += skip * 2;
            frames -= skip;
            start += static_cast<std::int64_t>(skip);
        }
        // Audio arrives ahead of the delayed video; nothing past the movie's end is kept.
        if (start >= audio_cap_) return;
        frames = static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(frames), audio_cap_ - start));
        if (!frames) return;
        // Meters show the source before its volume, so a muted microphone still shows it works.
        core::raise_peak(source.peak, data, frames * 2);
        const float* program = data;
        if (const auto ramp = source.fader.next(); !ramp.unity()) {
            program_.assign(data, data + frames * 2);
            ramp.apply(program_.data(), frames);
            program = program_.data();
        }
        // With one source its track is the program; with two, the stems stay as captured.
        const bool alone = !(audio_[kSystem].enabled && audio_[kMic].enabled);
        bool ok = true;
        if (source.stream >= 0) {
            if (start > floor) ok = write_pcm(source.stream, nullptr, static_cast<std::size_t>(start - floor), source.stem_channels, floor);
            ok = ok && write_pcm(source.stream, alone ? program : data, frames, source.stem_channels, start);
        }
        if (mixing_) mixer_.add(kind, start, program, frames);
        else if (live_) live_->audio(start, program, frames);
        source.next = start + static_cast<std::int64_t>(frames);
        if (ok) source.written.fetch_add(1);
        else source.dropped.fetch_add(1);
    }

    // Captured audio is stamped on QPC; paused spans are cut out at the same instants as video.
    void route(Source kind, const AudioChunk& chunk) {
        const auto host = chunk.time + audio_[kind].offset;
        const auto length = static_cast<std::int64_t>(chunk.frames) * kTicks / kMixRate;
        const auto to_frames = [](std::int64_t ticks) { return std::llround(static_cast<double>(ticks) * kMixRate / kTicks); };
        for (const auto& piece : timeline_.kept(host, length)) {
            const auto skip = std::min<std::int64_t>(to_frames(piece.skip), chunk.frames);
            const auto count = piece.skip == 0 && piece.length == length
                ? static_cast<std::int64_t>(chunk.frames)
                : std::min<std::int64_t>(to_frames(piece.length), chunk.frames - skip);
            if (count > 0)
                place(kind, to_frames(piece.output), chunk.data.data() + skip * 2, static_cast<std::size_t>(count));
        }
    }

    void emit_mix(bool flush) {
        if (!mixing_) return;
        std::int64_t target = flush ? video_end_samples() : std::min(mixer_.end(kSystem), mixer_.end(kMic));
        if (!flush && !options_.test_pattern)
            target = std::max(target, timeline_.to_output(now_ticks()).output * kMixRate / kTicks - kMixWaitFrames);
        target = std::min(target, audio_cap_);
        while (mixer_.emitted() < target) {
            const auto first = mixer_.emitted();
            const auto frames = static_cast<std::size_t>(std::min<std::int64_t>({target - first, 4800, mixer_.capacity()}));
            mixer_.take(frames, mixed_);
            if (live_) live_->audio(first, mixed_.data(), frames);
            if (mix_stream_ < 0) continue;
            if (!write_pcm(mix_stream_, mixed_.data(), frames, 2, first)) return;
            mix_written_.fetch_add(1);
        }
    }

    std::int64_t video_end_samples() const {
        return static_cast<std::int64_t>(video_end_slot_) * kMixRate / options_.fps;
    }

    void pump_audio(bool flush) {
        if (flush) audio_cap_ = std::min(audio_cap_, video_end_samples());
        for (auto kind : {kSystem, kMic}) {
            auto& source = audio_[kind];
            if (!source.capture) continue;
            for (int chunks = 0; chunks < 64 && source.capture->pop(chunk_); ++chunks)
                route(kind, chunk_);
        }
        if (flush) {
            // Pad stems to the end of the video so every track spans the recording.
            const auto end = video_end_samples();
            for (auto& source : audio_) {
                if (source.stream < 0) continue;
                const auto from = source.next == INT64_MIN ? 0 : source.next;
                if (from < end) write_pcm(source.stream, nullptr, static_cast<std::size_t>(end - from), source.stem_channels, from);
                source.next = std::max(from, end);
            }
        }
        emit_mix(flush);
    }

    // Synthetic sources follow the written video, so CI gets a deterministic A/V timeline.
    void write_synthetic_audio(std::uint64_t until) {
        std::array<float, 2048> tone{}, mic{};
        while (synthetic_samples_ < until && !stop_requested_.load() && !finished_.load()) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(1024, until - synthetic_samples_));
            const auto start = static_cast<std::int64_t>(synthetic_samples_);
            const auto shifted = [&](Source kind) {
                return start + std::llround(static_cast<double>(audio_[kind].offset) * kMixRate / kTicks);
            };
            for (std::size_t i = 0; i < count; ++i) {
                tone[i * 2] = tone[i * 2 + 1] = core::test_tone(synthetic_samples_ + i);
                mic[i * 2] = mic[i * 2 + 1] = core::test_mic(synthetic_samples_ + i);
            }
            if (options_.test_tone) place(kSystem, shifted(kSystem), tone.data(), count);
            if (options_.test_mic) place(kMic, shifted(kMic), mic.data(), count);
            synthetic_samples_ += count;
        }
    }

    void run() {
        ComScope com;
        using namespace std::chrono;
        const auto delay = options_.test_pattern ? nanoseconds(0) : duration_cast<nanoseconds>(kEmitDelay);
        const auto limit = options_.duration_seconds > 0
            ? static_cast<std::uint64_t>(std::llround(options_.duration_seconds * options_.fps)) : UINT64_MAX;
        for (std::uint64_t slot = 0; slot < limit && !stop_requested_.load() && !finished_; ++slot) {
            const auto output = static_cast<std::int64_t>(slot) * kTicks / options_.fps;
            std::int64_t due = 0;
            steady_clock::time_point tick;
            // Wait for the slot's host instant. A pause can begin while waiting; then the
            // slot waits for resume() and lands after it, so the movie has no gap.
            for (;;) {
                if (stop_requested_.load() || finished_.load()) break;
                const auto host = timeline_.to_host(output);
                if (!host) {
                    pump_audio(false);
                    std::this_thread::sleep_for(milliseconds(5));
                    continue;
                }
                due = *host;
                tick = begin_ + nanoseconds((due - origin_) * 100) + delay;
                const auto now = steady_clock::now();
                if (now >= tick) break;
                std::this_thread::sleep_until(std::min(tick, now + milliseconds(10)));
            }
            if (stop_requested_.load() || finished_) break;
            // The recorded window closed: end like a reached duration, so the movie is saved.
            // GraphicsCaptureItem.Closed did not arrive for a closed window here.
            if (options_.window && !IsWindow(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(options_.window)))) {
                window_closed_.store(true);
                break;
            }
            const int frame = select_frame(slot, due);
            if (options_.preview_only) {
                if (frame >= 0 && preview_.due(slot, options_.fps)) draw_preview(frame);
                continue;
            }
            // Retry the same timestamp for up to 500 ms on transient backpressure.
            bool appended = false;
            const auto deadline = tick + milliseconds(500);
            while (frame >= 0 && !stop_requested_.load() && !finished_.load() && steady_clock::now() < deadline) {
                const auto result = write_video(slot, frame);
                if (result == Write::done) { appended = true; break; }
                if (result == Write::failed) break;
                pump_audio(false);
                std::this_thread::sleep_for(milliseconds(2));
            }
            if (appended) {
                written_.fetch_add(1);
                video_end_slot_ = slot + 1;
                if (options_.test_tone || options_.test_mic)
                    write_synthetic_audio(((slot + 1) * static_cast<std::uint64_t>(kMixRate)) / options_.fps);
                // A stream without audio sources still carries a silent AAC track; services expect one.
                if (live_ && !audio_[kSystem].enabled && !audio_[kMic].enabled)
                    live_->audio(0, nullptr, static_cast<std::size_t>(video_end_samples()));
            } else if (!stop_requested_.load() && !finished_.load()) {
                dropped_.fetch_add(1);
            }
            pump_audio(false);
        }
        finished_.store(true);
    }

    Options options_;
    Size size_{};
    mutable std::mutex mutex_;
    std::condition_variable frames_ready_;
    std::string failure_;
    bool mf_started_ = false;
    bool writing_ = false;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11VideoProcessorEnumerator> processor_enum_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11Texture2D> intermediate_;
    ComPtr<ID3D11VideoProcessorInputView> input_view_;
    Size intermediate_size_{};
    ComPtr<IMFDXGIDeviceManager> manager_;
    UINT reset_token_ = 0;
    wgdx::Direct3D11::IDirect3DDevice winrt_device_{nullptr};
    wgc::GraphicsCaptureItem item_{nullptr};
    wgc::Direct3D11CaptureFramePool pool_{nullptr};
    wgc::GraphicsCaptureSession session_{nullptr};
    winrt::event_token frame_token_{};
    winrt::Windows::Graphics::SizeInt32 pool_size_{};
    std::mutex callback_mutex_;
    bool accepting_ = false;          // Guarded by callback_mutex_.

    std::array<VideoSlot, 5> slots_;  // Guarded by mutex_ (state and time).
    std::array<int, 3> pending_{};
    std::size_t pending_head_ = 0, pending_count_ = 0;
    int current_ = -1;
    std::array<ComPtr<IMFMediaBuffer>, 2> pattern_;
    std::unique_ptr<win::Overlays> overlays_;
    std::uint64_t camera_frames_ = 0;  // The count when the overlays were released.
    std::unique_ptr<win::FramePreview> previewer_;
    ComPtr<ID3D11Texture2D> composed_;  // The frame as drawn, in preview-only mode.
    core::PreviewBuffer preview_;
    ComPtr<IMFVideoSampleAllocatorEx> allocator_;

    ComPtr<IMFSinkWriter> writer_;
    DWORD video_stream_ = 0;
    std::unique_ptr<win::LiveStream> live_;  // Kept after stop() for its counters.
    std::uint32_t vendor_ = 0;
    int mix_stream_ = -1;
    std::array<AudioSource, 2> audio_;
    double system_offset_ms_ = 0;
    bool mixing_ = false;
    core::Mixer mixer_;
    AudioChunk chunk_;
    std::vector<float> mixed_, program_;
    std::uint64_t synthetic_samples_ = 0;
    std::uint64_t video_end_slot_ = 0;
    core::PauseTimeline timeline_;
    std::int64_t audio_cap_ = INT64_MAX;  // Recording-timeline samples; audio at or past it is dropped.

    std::int64_t origin_ = 0;
    std::chrono::steady_clock::time_point begin_{};
    std::thread pacer_;
    std::atomic_bool stop_requested_{false};
    std::atomic_bool active_{false};
    std::atomic_bool recording_{false};
    std::atomic_bool finished_{false};
    std::atomic_bool window_closed_{false};
    std::atomic<std::uint64_t> captured_{0}, written_{0}, dropped_{0}, mix_written_{0};
};

Result probe_file(const std::string& path, Probe& probe, std::optional<Region> sample) {
    ComScope com;
    probe = {};
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return {"Media Foundation is unavailable"};
    struct Shutdown { ~Shutdown() { MFShutdown(); } } shutdown;
    const auto url = wide(path);
    auto open = [&](ComPtr<IMFSourceReader>& reader) {
        return MFCreateSourceReaderFromURL(url.c_str(), nullptr, &reader);
    };
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = open(reader);
    if (FAILED(hr)) return {failed("could not open movie", hr)};
    int video = -1, audio = -1;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        hr = reader->GetNativeMediaType(i, 0, &type);
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        if (FAILED(hr)) continue;
        GUID major{}, subtype{};
        type->GetGUID(MF_MT_MAJOR_TYPE, &major);
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (major == MFMediaType_Video && video < 0) {
            video = static_cast<int>(i);
            probe.codec = subtype == MFVideoFormat_H264 ? "h264" : subtype == MFVideoFormat_HEVC ? "hevc" : "other";
            UINT32 width = 0, height = 0;
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
            // HEVC frames are coded in 8- or 16-pixel blocks; the display aperture is the real size.
            MFVideoArea aperture{};
            if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&aperture), sizeof(aperture), nullptr)) &&
                aperture.Area.cx > 0 && aperture.Area.cy > 0) {
                width = static_cast<UINT32>(aperture.Area.cx);
                height = static_cast<UINT32>(aperture.Area.cy);
            }
            probe.width = width;
            probe.height = height;
        } else if (major == MFMediaType_Audio) {
            ++probe.audio_tracks;
            if (audio < 0) {
                audio = static_cast<int>(i);
                probe.audio_codec = subtype == MFAudioFormat_AAC ? "aac" : "other";
            }
        }
    }
    if (video < 0) return {"no video track"};

    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(static_cast<DWORD>(video), TRUE);
    {
        ComPtr<IMFMediaType> nv12;
        MFCreateMediaType(&nv12);
        nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        hr = reader->SetCurrentMediaType(static_cast<DWORD>(video), nullptr, nv12.Get());
        if (FAILED(hr)) return {failed("could not decode video (is a decoder for this codec installed?)", hr)};
    }
    if (sample) {
        const auto& r = *sample;
        if (r.x < 0 || r.y < 0 || r.width < 2 || r.height < 2 || r.x + r.width > probe.width || r.y + r.height > probe.height)
            return {"the sample rectangle does not fit the " + std::to_string(probe.width) + "x" + std::to_string(probe.height) + " video"};
    }
    // Decoded NV12 rows are padded to the coded height; the chroma plane starts after it.
    UINT32 coded_height = probe.height;
    auto read_coded_height = [&] {
        ComPtr<IMFMediaType> current;
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(video), &current)) &&
            SUCCEEDED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h)) && h >= probe.height)
            coded_height = h;
    };
    read_coded_height();
    LONGLONG next_sample = 0;
    core::FlashDetector flashes;
    LONGLONG previous = LLONG_MIN, end = 0;
    for (;;) {
        DWORD stream = 0, flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> decoded;
        hr = reader->ReadSample(static_cast<DWORD>(video), 0, &stream, &flags, &time, &decoded);
        if (FAILED(hr)) return {failed("video decode failed", hr)};
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) read_coded_height();
        if (decoded) {
            if (previous != LLONG_MIN && time <= previous)
                return {"video timestamps are not strictly increasing: frame " + std::to_string(probe.frames) + " at " +
                        std::to_string(time / 10000) + " ms follows " + std::to_string(previous / 10000) + " ms"};
            previous = time;
            ++probe.frames;
            LONGLONG duration = 0;
            decoded->GetSampleDuration(&duration);
            end = std::max(end, time + duration);
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMF2DBuffer> planar;
            BYTE* scan = nullptr;
            LONG pitch = 0;
            double brightness = 0;
            if (SUCCEEDED(decoded->GetBufferByIndex(0, &buffer)) && SUCCEEDED(buffer.As(&planar)) &&
                SUCCEEDED(planar->Lock2D(&scan, &pitch))) {
                std::uint64_t sum = 0, count = 0;
                const auto step_x = std::max<UINT32>(1, probe.width / 64), step_y = std::max<UINT32>(1, probe.height / 36);
                for (UINT32 y = step_y / 2; y < probe.height; y += step_y)
                    for (UINT32 x = step_x / 2; x < probe.width; x += step_x, ++count)
                        sum += scan[static_cast<std::ptrdiff_t>(y) * pitch + x];
                // Studio-range luma: 16 is black, 235 is white.
                if (count) brightness = (static_cast<double>(sum) / count - 16.0) / 219.0;
                if (sample && time >= next_sample) {
                    const auto& r = *sample;
                    const BYTE* chroma = scan + static_cast<std::ptrdiff_t>(coded_height) * pitch;
                    double y_sum = 0, u_sum = 0, v_sum = 0;
                    for (UINT32 y = 0; y < r.height; ++y)
                        for (UINT32 x = 0; x < r.width; ++x)
                            y_sum += scan[static_cast<std::ptrdiff_t>(r.y + y) * pitch + r.x + x];
                    const UINT32 cw = r.width / 2, ch = r.height / 2;
                    for (UINT32 y = 0; y < ch; ++y)
                        for (UINT32 x = 0; x < cw; ++x) {
                            const BYTE* uv = chroma + static_cast<std::ptrdiff_t>(r.y / 2 + y) * pitch + (r.x / 2 + x) * 2;
                            u_sum += uv[0];
                            v_sum += uv[1];
                        }
                    probe.samples.push_back(core::studio_rgb(y_sum / (static_cast<double>(r.width) * r.height),
                                                             u_sum / std::max(1u, cw * ch), v_sum / std::max(1u, cw * ch)));
                    next_sample = (time / kTicks + 1) * kTicks;
                }
                planar->Unlock2D();
            }
            flashes.feed(static_cast<double>(time) / kTicks, brightness);
        }        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (!probe.frames) return {"no decoded frames"};
    probe.duration_seconds = static_cast<double>(end) / kTicks;

    core::OnsetDetector beeps;
    if (audio >= 0) {
        ComPtr<IMFSourceReader> audio_reader;
        hr = open(audio_reader);
        if (FAILED(hr)) return {failed("could not open audio track", hr)};
        audio_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        audio_reader->SetStreamSelection(static_cast<DWORD>(audio), TRUE);
        ComPtr<IMFMediaType> pcm;
        MFCreateMediaType(&pcm);
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        hr = audio_reader->SetCurrentMediaType(static_cast<DWORD>(audio), nullptr, pcm.Get());
        if (FAILED(hr)) return {failed("could not decode audio", hr)};
        ComPtr<IMFMediaType> actual;
        audio_reader->GetCurrentMediaType(static_cast<DWORD>(audio), &actual);
        const UINT32 channels = std::max<UINT32>(1, MFGetAttributeUINT32(actual.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2));
        const UINT32 rate = std::max<UINT32>(1, MFGetAttributeUINT32(actual.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, kMixRate));
        LONGLONG audio_end = 0;
        for (;;) {
            DWORD stream = 0, flags = 0;
            LONGLONG time = 0;
            ComPtr<IMFSample> sample;
            hr = audio_reader->ReadSample(static_cast<DWORD>(audio), 0, &stream, &flags, &time, &sample);
            if (FAILED(hr)) return {failed("audio decode failed", hr)};
            if (sample) {
                ++probe.audio_buffers;
                ComPtr<IMFMediaBuffer> buffer;
                BYTE* data = nullptr;
                DWORD length = 0;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) &&
                    SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                    const auto* samples = reinterpret_cast<const float*>(data);
                    const std::size_t frames = length / sizeof(float) / channels;
                    for (std::size_t frame = 0; frame < frames; ++frame) {
                        float peak = 0;
                        for (UINT32 c = 0; c < channels; ++c) peak = std::max(peak, std::fabs(samples[frame * channels + c]));
                        beeps.feed(static_cast<double>(time) / kTicks + static_cast<double>(frame) / rate, peak);
                    }
                    buffer->Unlock();
                    audio_end = std::max<LONGLONG>(audio_end, time + static_cast<LONGLONG>(frames) * kTicks / rate);
                }
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        if (!probe.audio_buffers) return {"audio track has no decoded samples"};
        probe.audio_duration_seconds = static_cast<double>(audio_end) / kTicks;
    }
    core::measure_sync(flashes.onsets, beeps.onsets, probe);
    return {};
}

class Recorder::Impl {
public:
    explicit Impl(Options options) : capture(std::move(options)) {}
    CaptureState capture;
};
Recorder::Recorder(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}
Recorder::~Recorder() = default;
Result Recorder::start() { return impl_->capture.start(); }
Result Recorder::stop() { return impl_->capture.stop(); }
Result Recorder::pause() { return impl_->capture.pause(); }
Result Recorder::resume() { return impl_->capture.resume(); }
bool Recorder::running() const noexcept { return impl_->capture.running(); }
bool Recorder::finished() const noexcept { return impl_->capture.finished(); }
Stats Recorder::stats() const noexcept { return impl_->capture.stats(); }
std::string Recorder::failure() const { return impl_->capture.failure(); }
std::string Recorder::stream_failure() const { return impl_->capture.stream_failure(); }
Result Recorder::save_replay(const std::string& path, double* seconds) {
    double saved = 0;
    auto result = impl_->capture.save_replay(path, saved);
    if (seconds) *seconds = saved;
    return result;
}
Result Recorder::set_overlay(std::size_t index, bool visible, std::optional<Area> area) {
    return impl_->capture.set_overlay(index, visible, area);
}
Result Recorder::set_volume(AudioInput input, double gain_db, bool muted) {
    return impl_->capture.set_volume(input, gain_db, muted);
}
bool Recorder::preview(PreviewFrame& frame) const { return impl_->capture.preview(frame); }
void Recorder::show_preview(bool on) noexcept { impl_->capture.show_preview(on); }

} // namespace rec
