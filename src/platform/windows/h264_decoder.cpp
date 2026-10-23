#include "platform/windows/h264_decoder.hpp"

#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace rec::win {
namespace {
using Microsoft::WRL::ComPtr;

std::string failed(const char* what, HRESULT hr) {
    char text[160];
    std::snprintf(text, sizeof text, "%s (0x%08lX)", what, static_cast<unsigned long>(hr));
    return text;
}

std::uint8_t clamp8(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); }
} // namespace

class H264Decoder::Impl {
public:
    ~Impl() {
        if (mft_) mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        mft_.Reset();
        if (mf_) MFShutdown();
        if (com_) CoUninitialize();
    }

    std::string start() {
        com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) return failed("could not start Media Foundation", hr);
        mf_ = true;
        hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_));
        if (FAILED(hr)) return failed("this computer has no H.264 decoder", hr);
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(mft_->GetAttributes(&attributes))) attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        ComPtr<IMFMediaType> input;
        MFCreateMediaType(&input);
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = mft_->SetInputType(0, input.Get(), 0);
        if (FAILED(hr)) return failed("the H.264 decoder refused its input", hr);
        if (auto e = choose_output(); !e.empty()) return e;
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return {};
    }

    bool decode(const std::uint8_t* data, std::size_t size, const Picture& picture) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer))) return false;
        BYTE* bytes = nullptr;
        buffer->Lock(&bytes, nullptr, nullptr);
        std::memcpy(bytes, data, size);
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(size));
        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(time_);
        sample->SetSampleDuration(333333);
        time_ += 333333;
        HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            drain(picture);
            hr = mft_->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) return false;
        return drain(picture);
    }

private:
    std::string choose_output() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> type;
            const HRESULT hr = mft_->GetOutputAvailableType(0, i, &type);
            if (FAILED(hr)) return failed("the H.264 decoder has no NV12 output", hr);
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype != MFVideoFormat_NV12) continue;
            if (FAILED(mft_->SetOutputType(0, type.Get(), 0))) continue;
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
            width_ = w;
            height_ = h;
            visible_w_ = w;
            visible_h_ = h;
            MFVideoArea area{};
            if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof area, nullptr)) &&
                area.Area.cx > 0 && area.Area.cy > 0) {
                visible_w_ = std::min<UINT32>(w, static_cast<UINT32>(area.Area.cx));
                visible_h_ = std::min<UINT32>(h, static_cast<UINT32>(area.Area.cy));
            }
            UINT32 stride = 0;
            stride_ = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? static_cast<LONG>(stride) : static_cast<LONG>(w);
            return {};
        }
    }

    bool drain(const Picture& picture) {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            mft_->GetOutputStreamInfo(0, &info);
            MFT_OUTPUT_DATA_BUFFER out{};
            ComPtr<IMFSample> sample;
            if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                ComPtr<IMFMediaBuffer> buffer;
                if (FAILED(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 1), &buffer))) return false;
                MFCreateSample(&sample);
                sample->AddBuffer(buffer.Get());
                out.pSample = sample.Get();
            }
            DWORD status = 0;
            const HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents) out.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!choose_output().empty()) return false;
                continue;
            }
            if (FAILED(hr)) return false;
            ComPtr<IMFSample> produced = out.pSample;
            if (!sample && out.pSample) out.pSample->Release();
            if (produced) convert(produced.Get(), picture);
        }
    }

    void convert(IMFSample* sample, const Picture& picture) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return;
        ComPtr<IMF2DBuffer> planar;
        BYTE* scan = nullptr;
        LONG pitch = 0;
        const bool locked2d = SUCCEEDED(buffer.As(&planar)) && SUCCEEDED(planar->Lock2D(&scan, &pitch));
        DWORD length = 0;
        if (!locked2d) {
            if (FAILED(buffer->Lock(&scan, nullptr, &length))) return;
            pitch = stride_;
            if (length < static_cast<DWORD>(pitch) * height_ * 3 / 2) { buffer->Unlock(); return; }
        }
        const std::uint32_t w = visible_w_ & ~1u, h = visible_h_ & ~1u;
        bgra_.resize(static_cast<std::size_t>(w) * h * 4);
        const BYTE* luma = scan;
        const BYTE* chroma = scan + static_cast<std::size_t>(pitch) * height_;
        for (std::uint32_t y = 0; y < h; ++y) {
            const BYTE* yr = luma + static_cast<std::size_t>(pitch) * y;
            const BYTE* cr = chroma + static_cast<std::size_t>(pitch) * (y / 2);
            std::uint8_t* o = bgra_.data() + static_cast<std::size_t>(y) * w * 4;
            for (std::uint32_t x = 0; x < w; ++x) {
                const int c = 298 * (yr[x] - 16), d = cr[x & ~1u] - 128, e = cr[(x & ~1u) + 1] - 128;
                o[x * 4 + 0] = clamp8((c + 516 * d + 128) >> 8);
                o[x * 4 + 1] = clamp8((c - 100 * d - 208 * e + 128) >> 8);
                o[x * 4 + 2] = clamp8((c + 409 * e + 128) >> 8);
                o[x * 4 + 3] = 255;
            }
        }
        if (locked2d) planar->Unlock2D();
        else buffer->Unlock();
        picture(bgra_.data(), w, h);
    }

    bool com_ = false, mf_ = false;
    ComPtr<IMFTransform> mft_;
    UINT32 width_ = 0, height_ = 0, visible_w_ = 0, visible_h_ = 0;
    LONG stride_ = 0;
    LONGLONG time_ = 0;
    std::vector<std::uint8_t> bgra_;
};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;
std::string H264Decoder::start() { return impl_->start(); }
bool H264Decoder::decode(const std::uint8_t* annexb, std::size_t size, const Picture& picture) {
    return impl_->decode(annexb, size, picture);
}

} // namespace rec::win
