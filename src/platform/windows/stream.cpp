#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <initguid.h>
#include <codecapi.h>

#include "platform/windows/stream.hpp"
#include "core/replay.hpp"
#include "core/rtmp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace rec::win {
namespace {
using Microsoft::WRL::ComPtr;

constexpr std::int64_t kTicks = 10'000'000;
constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kVideoQueue = 4;
constexpr std::size_t kAudioFrames = kRate * 2;

std::string failed(const char* what, HRESULT hr) {
    char code[16];
    std::snprintf(code, sizeof code, "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(what) + " (" + code + ")";
}

struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

ComPtr<IMFMediaType> video_type(const GUID& subtype, std::uint32_t width, std::uint32_t height, std::uint32_t fps) {
    ComPtr<IMFMediaType> type;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    return type;
}

ComPtr<IMFMediaType> audio_type(bool aac, std::uint32_t kbps) {
    ComPtr<IMFMediaType> type;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, aac ? MFAudioFormat_AAC : MFAudioFormat_PCM);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (aac) {
        // The Windows AAC encoder takes 96, 128, 160 or 192 kb/s.
        const std::uint32_t bytes = std::clamp<std::uint32_t>((kbps + 16) / 32 * 4000, 12000, 24000);
        type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytes);
        type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    } else {
        type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 4);
        type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    }
    return type;
}

void set_value(ICodecAPI* api, const GUID& property, ULONG value) {
    VARIANT v{};
    v.vt = VT_UI4;
    v.ulVal = value;
    api->SetValue(&property, &v);
}

void set_flag(ICodecAPI* api, const GUID& property, bool value) {
    VARIANT v{};
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&property, &v);
}

std::vector<ComPtr<IMFActivate>> encoders(const GUID& category, UINT32 flags, const MFT_REGISTER_TYPE_INFO& in,
                                          const MFT_REGISTER_TYPE_INFO& out) {
    std::vector<ComPtr<IMFActivate>> list;
    IMFActivate** found = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(category, flags | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &found, &count))) return list;
    for (UINT32 i = 0; i < count; ++i) {
        list.emplace_back(found[i]);
        found[i]->Release();
    }
    CoTaskMemFree(found);
    return list;
}

// One Media Foundation encoder: synchronous (software) or asynchronous (hardware, driven by
// NeedInput/HaveOutput events).
struct Transform {
    ComPtr<IMFTransform> mft;
    ComPtr<IMFActivate> activate;
    ComPtr<IMFMediaEventGenerator> events;
    bool async = false;
    int needs = 0;
    bool drained = false;
    bool provides = false;
    DWORD size = 0;

    void refresh() {
        MFT_OUTPUT_STREAM_INFO info{};
        if (SUCCEEDED(mft->GetOutputStreamInfo(0, &info))) {
            provides = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
            size = info.cbSize;
        }
    }
    void begin() {
        refresh();
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    bool ready() const { return !async || needs > 0; }
    HRESULT input(IMFSample* sample) {
        const HRESULT hr = mft->ProcessInput(0, sample, 0);
        if (SUCCEEDED(hr) && async) --needs;
        return hr;
    }
    // One encoded sample, or MF_E_TRANSFORM_NEED_MORE_INPUT.
    HRESULT output(ComPtr<IMFSample>& out, DWORD fallback) {
        for (int attempt = 0; attempt < 4; ++attempt) {
            MFT_OUTPUT_DATA_BUFFER buffer{};
            ComPtr<IMFSample> mine;
            if (!provides) {
                ComPtr<IMFMediaBuffer> memory;
                HRESULT hr = MFCreateSample(&mine);
                if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(std::max(size, fallback), &memory);
                if (SUCCEEDED(hr)) hr = mine->AddBuffer(memory.Get());
                if (FAILED(hr)) return hr;
                buffer.pSample = mine.Get();
            }
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &buffer, &status);
            if (buffer.pEvents) buffer.pEvents->Release();
            ComPtr<IMFSample> given;
            if (provides && buffer.pSample) given.Attach(buffer.pSample);
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                ComPtr<IMFMediaType> type;
                if (SUCCEEDED(mft->GetOutputAvailableType(0, 0, &type))) mft->SetOutputType(0, type.Get(), 0);
                refresh();
                continue;
            }
            if (FAILED(hr)) return hr;
            out = provides ? given : mine;
            return out ? S_OK : MF_E_TRANSFORM_NEED_MORE_INPUT;
        }
        return E_FAIL;
    }
    void release() {
        events.Reset();
        mft.Reset();
        if (activate) activate->ShutdownObject();
        activate.Reset();
    }
};
} // namespace

class LiveStream::Impl {
public:
    ~Impl() { stop(); }

    Result start(const LiveSettings& settings) {
        settings_ = settings;
        if (auto made = create_video(); !made) { release(); return made; }
        if (auto made = create_audio(); !made) { release(); return made; }
        video_.begin();
        audio_.begin();
        ring_.assign(kAudioFrames * 2, 0);
        if (settings_.replay_seconds)
            replay_ = std::make_unique<core::ReplayBuffer>(settings_.replay_seconds, settings_.width, settings_.height, settings_.fps);
        if (!settings_.movie.empty()) {
            movie_ = std::make_unique<core::MovieWriter>(settings_.width, settings_.height, settings_.fps);
            if (auto error = movie_->open(settings_.movie); !error.empty()) { movie_.reset(); release(); return {error}; }
        }
        core::StreamInfo info{settings_.width, settings_.height, settings_.fps, settings_.video_kbps, settings_.audio_kbps};
        if (!settings_.urls.empty())
            if (auto error = servers_.open(settings_.urls, info); !error.empty()) { release(); return {error}; }
        thread_ = std::thread([this] { run(); });
        return {};
    }

    void video(IMFSample* frame) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || !thread_.joinable()) return;
            if (frame_count_ == kVideoQueue) {
                frames_[frame_head_].Reset();
                frame_head_ = (frame_head_ + 1) % kVideoQueue;
                --frame_count_;
                dropped_.fetch_add(1);
            }
            frames_[(frame_head_ + frame_count_) % kVideoQueue] = frame;
            ++frame_count_;
        }
        wake_.notify_one();
    }

    void audio(std::int64_t start, const float* stereo, std::size_t frames) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || !thread_.joinable() || !frames) return;
            if (next_ == INT64_MIN) next_ = ring_start_ = start;
            const auto end = start + static_cast<std::int64_t>(frames);
            if (end <= next_) return;
            if (start < next_) {
                const auto skip = static_cast<std::size_t>(next_ - start);
                if (stereo) stereo += skip * 2;
                frames -= skip;
                start = next_;
            }
            // A gap becomes silence so the AAC timeline stays continuous.
            write(nullptr, static_cast<std::size_t>(std::min<std::int64_t>(start - next_, kAudioFrames)));
            next_ = start;
            write(stereo, frames);
        }
        wake_.notify_one();
    }

    std::string stop() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
        servers_.close(0);
        release();
        std::string error;
        // Kept, so counters() can still read its frame count.
        if (movie_ && !movie_finished_) {
            movie_finished_ = true;
            error = movie_->finish();
        }
        return error;
    }

    LiveCounters counters() const {
        LiveCounters c;
        const bool broken = broken_.load();
        c.hardware = hardware_;
        std::uint64_t skipped = 0;
        c.server_count = static_cast<std::uint32_t>(std::min(servers_.size(), kMaxStreams));
        for (std::uint32_t i = 0; i < c.server_count; ++i) {
            const auto s = servers_.counters(i);
            auto& out = c.servers[i];
            out = {s.live && !broken, s.reconnecting && !broken, s.reconnects, s.frames, s.dropped, s.bytes, s.queued_ms};
            c.live = c.live || out.live;
            c.reconnecting = c.reconnecting || out.reconnecting;
            c.reconnects += out.reconnects;
            c.frames = std::max(c.frames, out.frames);
            skipped = std::max(skipped, out.dropped);
            c.bytes += out.bytes;
            c.queued_ms = std::max(c.queued_ms, out.queued_ms);
        }
        c.dropped = dropped_.load() + skipped;
        c.replay_seconds = replay_ ? replay_->seconds() : 0;
        c.movie_frames = movie_ ? movie_->frames() : 0;
        return c;
    }

    Result save_replay(const std::string& path, double& seconds) const {
        seconds = 0;
        if (!replay_) return {"the replay buffer is off"};
        return {replay_->save(path, seconds)};
    }

    std::string failure() const {
        {
            std::lock_guard lock(mutex_);
            if (!error_.empty()) return error_;
        }
        return servers_.failure();
    }

private:
    Result create_video() {
        const MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12}, out{MFMediaType_Video, MFVideoFormat_H264};
        if (settings_.manager) {
            auto list = encoders(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE, in, out);
            wchar_t wanted[16];
            std::swprintf(wanted, 16, L"VEN_%04X", settings_.vendor);
            std::stable_partition(list.begin(), list.end(), [&](const ComPtr<IMFActivate>& a) {
                LPWSTR vendor = nullptr;
                UINT32 length = 0;
                if (FAILED(a->GetAllocatedString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, &vendor, &length))) return false;
                const bool match = _wcsicmp(vendor, wanted) == 0;
                CoTaskMemFree(vendor);
                return match;
            });
            for (auto& activate : list)
                if (configure_video(activate, true)) { hardware_ = true; return {}; }
        }
        for (auto& activate : encoders(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT, in, out))
            if (configure_video(activate, false)) return {};
        return {"no H.264 encoder accepts the stream settings"};
    }

    bool configure_video(const ComPtr<IMFActivate>& activate, bool hardware) {
        Transform t;
        t.activate = activate;
        if (FAILED(activate->ActivateObject(IID_PPV_ARGS(&t.mft)))) return false;
        const auto give_up = [&] { t.release(); return false; };
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(t.mft->GetAttributes(&attributes)) && MFGetAttributeUINT32(attributes.Get(), MF_TRANSFORM_ASYNC, FALSE)) {
            attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            t.async = true;
            if (FAILED(t.mft.As(&t.events))) return give_up();
        }
        if (hardware && FAILED(t.mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                     reinterpret_cast<ULONG_PTR>(settings_.manager))))
            return give_up();
        // Streaming services expect constant bitrate, a keyframe every 2 s, and no B-frames.
        ComPtr<ICodecAPI> api;
        const auto tune = [&] {
            if (!api) return;
            set_value(api.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
            set_value(api.Get(), CODECAPI_AVEncCommonMeanBitRate, settings_.video_kbps * 1000);
            set_value(api.Get(), CODECAPI_AVEncMPVGOPSize, settings_.fps * 2);
            set_value(api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
            set_flag(api.Get(), CODECAPI_AVLowLatencyMode, true);
        };
        t.mft.As(&api);
        tune();
        auto output = video_type(MFVideoFormat_H264, settings_.width, settings_.height, settings_.fps);
        output->SetUINT32(MF_MT_AVG_BITRATE, settings_.video_kbps * 1000);
        output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        if (FAILED(t.mft->SetOutputType(0, output.Get(), 0))) return give_up();
        auto input = video_type(MFVideoFormat_NV12, settings_.width, settings_.height, settings_.fps);
        if (FAILED(t.mft->SetInputType(0, input.Get(), 0))) {
            // Some encoders only take their own NV12 type, with the frame size filled in.
            bool accepted = false;
            for (DWORD i = 0; !accepted; ++i) {
                ComPtr<IMFMediaType> offered;
                if (FAILED(t.mft->GetInputAvailableType(0, i, &offered))) break;
                GUID subtype{};
                offered->GetGUID(MF_MT_SUBTYPE, &subtype);
                if (subtype != MFVideoFormat_NV12) continue;
                MFSetAttributeSize(offered.Get(), MF_MT_FRAME_SIZE, settings_.width, settings_.height);
                MFSetAttributeRatio(offered.Get(), MF_MT_FRAME_RATE, settings_.fps, 1);
                accepted = SUCCEEDED(t.mft->SetInputType(0, offered.Get(), 0));
            }
            if (!accepted) return give_up();
        }
        tune();
        video_ = t;
        return true;
    }

    Result create_audio() {
        const MFT_REGISTER_TYPE_INFO in{MFMediaType_Audio, MFAudioFormat_PCM}, out{MFMediaType_Audio, MFAudioFormat_AAC};
        for (auto& activate : encoders(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT, in, out)) {
            Transform t;
            t.activate = activate;
            if (FAILED(activate->ActivateObject(IID_PPV_ARGS(&t.mft)))) continue;
            const auto pcm = audio_type(false, 0), aac = audio_type(true, settings_.audio_kbps);
            bool ok = SUCCEEDED(t.mft->SetInputType(0, pcm.Get(), 0)) && SUCCEEDED(t.mft->SetOutputType(0, aac.Get(), 0));
            if (!ok) ok = SUCCEEDED(t.mft->SetOutputType(0, aac.Get(), 0)) && SUCCEEDED(t.mft->SetInputType(0, pcm.Get(), 0));
            if (ok) { audio_ = t; return {}; }
            t.release();
        }
        return {"no AAC encoder is available for streaming"};
    }

    void release() {
        video_.release();
        audio_.release();
        std::lock_guard lock(mutex_);
        for (auto& frame : frames_) frame.Reset();
        frame_count_ = 0;
    }

    void write(const float* stereo, std::size_t frames) {
        const auto clip = [](float v) {
            return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 32767.0f / 32768.0f) * 32768.0f));
        };
        for (std::size_t i = 0; i < frames; ++i) {
            if (ring_count_ == kAudioFrames) {
                ring_head_ = (ring_head_ + 1) % kAudioFrames;
                --ring_count_;
                ++ring_start_;
            }
            const auto at = (ring_head_ + ring_count_) % kAudioFrames * 2;
            ring_[at] = stereo ? clip(stereo[i * 2]) : 0;
            ring_[at + 1] = stereo ? clip(stereo[i * 2 + 1]) : 0;
            ++ring_count_;
        }
        next_ += static_cast<std::int64_t>(frames);
    }

    void fail(std::string text) {
        {
            std::lock_guard lock(mutex_);
            if (error_.empty()) error_ = std::move(text);
        }
        broken_.store(true);
    }

    void run() {
        ComScope com;
        std::int64_t stop_deadline = 0;
        const auto now_ms = [] {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        };
        for (;;) {
            bool done = false;
            {
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(video_.async ? 2 : 20),
                               [this] { return stopping_ || frame_count_ || ring_count_ >= 1024; });
                if (stopping_ && !stop_deadline) stop_deadline = now_ms() + 2000;
                done = stopping_ && ((!frame_count_ && !ring_count_) || now_ms() > stop_deadline);
            }
            if (video_.async) pump_video();
            for (;;) {
                ComPtr<IMFSample> frame;
                {
                    std::lock_guard lock(mutex_);
                    if (!frame_count_ || !video_.ready()) break;
                    frame.Swap(frames_[frame_head_]);
                    frame_head_ = (frame_head_ + 1) % kVideoQueue;
                    --frame_count_;
                }
                if (!broken_.load()) encode_video(frame.Get());
            }
            encode_audio();
            if (done) break;
        }
        if (!broken_.load()) drain();
        servers_.close(3000);
    }

    void encode_video(IMFSample* frame) {
        HRESULT hr = video_.input(frame);
        if (hr == MF_E_NOTACCEPTING && !video_.async) {
            collect_video();
            hr = video_.input(frame);
        }
        if (FAILED(hr)) { fail(failed("stream video encoding failed", hr)); return; }
        if (!video_.async) collect_video();
    }

    void collect_video() {
        for (;;) {
            ComPtr<IMFSample> out;
            const HRESULT hr = video_.output(out, settings_.width * settings_.height * 3 / 2);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if (FAILED(hr)) { fail(failed("stream video encoding failed", hr)); return; }
            send_video(out.Get());
        }
    }

    void pump_video() {
        for (;;) {
            ComPtr<IMFMediaEvent> event;
            if (FAILED(video_.events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event))) return;
            MediaEventType type = MEUnknown;
            event->GetType(&type);
            if (type == METransformNeedInput) {
                ++video_.needs;
            } else if (type == METransformHaveOutput) {
                ComPtr<IMFSample> out;
                const HRESULT hr = video_.output(out, settings_.width * settings_.height * 3 / 2);
                if (SUCCEEDED(hr)) send_video(out.Get());
                else if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT) fail(failed("stream video encoding failed", hr));
            } else if (type == METransformDrainComplete) {
                video_.drained = true;
            } else if (type == MEError) {
                HRESULT status = S_OK;
                event->GetStatus(&status);
                fail(failed("the stream video encoder stopped", status));
            }
        }
    }

    void send_video(IMFSample* sample) {
        LONGLONG time = 0;
        sample->GetSampleTime(&time);
        ComPtr<IMFMediaBuffer> buffer;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&data, nullptr, &length))) return;
        const bool parsed = core::flv::avc_frame(data, length, unit_);
        buffer->Unlock();
        if (!parsed) return;
        const auto ms = static_cast<std::uint32_t>(std::max<LONGLONG>(0, (time + 5000) / 10000));
        if (unit_.sps.empty() && sps_.empty()) {
            // Encoders that keep parameter sets out of band publish them on the output type.
            ComPtr<IMFMediaType> type;
            UINT32 size = 0;
            if (SUCCEEDED(video_.mft->GetOutputCurrentType(0, &type)) &&
                SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size) {
                std::vector<UINT8> header(size);
                if (SUCCEEDED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(), size, nullptr)))
                    core::flv::parameter_sets(header.data(), size, unit_.sps, unit_.pps);
            }
        }
        if (unit_.sps.size() >= 4 && !unit_.pps.empty() && (unit_.sps != sps_ || unit_.pps != pps_)) {
            sps_ = unit_.sps;
            pps_ = unit_.pps;
            send_video(ms, true, true, core::flv::avc_sequence_header(sps_, pps_));
        }
        if (sps_.empty() || (!started_ && !unit_.key)) { dropped_.fetch_add(1); return; }
        started_ = true;
        send_video(ms, unit_.key, false, std::move(unit_.body));
    }

    void send_video(std::uint32_t ms, bool key, bool header, core::Bytes body) {
        if (replay_) replay_->video(ms, key, header, body);
        if (movie_) movie_->video(ms, key, header, body);
        servers_.video(ms, key, header, std::move(body));
    }

    void send_audio(std::uint32_t ms, bool header, core::Bytes body) {
        if (replay_) replay_->audio(ms, header, body);
        if (movie_) movie_->audio(ms, header, body);
        servers_.audio(ms, header, std::move(body));
    }

    void encode_audio() {
        std::int64_t start = 0;
        std::size_t frames = 0;
        {
            std::lock_guard lock(mutex_);
            frames = ring_count_;
            start = ring_start_;
            pcm_.resize(frames * 2);
            for (std::size_t i = 0; i < frames; ++i) {
                const auto at = (ring_head_ + i) % kAudioFrames * 2;
                pcm_[i * 2] = ring_[at];
                pcm_[i * 2 + 1] = ring_[at + 1];
            }
            ring_head_ = (ring_head_ + frames) % kAudioFrames;
            ring_count_ = 0;
            ring_start_ += static_cast<std::int64_t>(frames);
        }
        if (!frames || broken_.load()) return;
        const DWORD bytes = static_cast<DWORD>(frames * 4);
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        BYTE* data = nullptr;
        if (FAILED(MFCreateMemoryBuffer(bytes, &buffer)) || FAILED(buffer->Lock(&data, nullptr, nullptr))) return;
        std::memcpy(data, pcm_.data(), bytes);
        buffer->Unlock();
        buffer->SetCurrentLength(bytes);
        if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer.Get()))) return;
        sample->SetSampleTime(start * kTicks / kRate);
        sample->SetSampleDuration(static_cast<LONGLONG>(frames) * kTicks / kRate);
        HRESULT hr = audio_.input(sample.Get());
        if (hr == MF_E_NOTACCEPTING) {
            collect_audio();
            hr = audio_.input(sample.Get());
        }
        if (FAILED(hr)) { fail(failed("stream audio encoding failed", hr)); return; }
        collect_audio();
    }

    void collect_audio() {
        for (;;) {
            ComPtr<IMFSample> out;
            const HRESULT hr = audio_.output(out, 8192);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if (FAILED(hr)) { fail(failed("stream audio encoding failed", hr)); return; }
            LONGLONG time = 0;
            out->GetSampleTime(&time);
            const auto ms = static_cast<std::uint32_t>(std::max<LONGLONG>(0, (time + 5000) / 10000));
            ComPtr<IMFMediaBuffer> buffer;
            BYTE* data = nullptr;
            DWORD length = 0;
            if (FAILED(out->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&data, nullptr, &length))) continue;
            if (!audio_header_) {
                send_audio(ms, true, core::flv::aac_sequence_header(kRate, 2));
                audio_header_ = true;
            }
            if (length) send_audio(ms, false, core::flv::aac_frame(data, length));
            buffer->Unlock();
        }
    }

    void drain() {
        video_.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        video_.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (video_.async) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!video_.drained && !broken_.load() && std::chrono::steady_clock::now() < deadline) {
                pump_video();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } else {
            collect_video();
        }
        audio_.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        audio_.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        collect_audio();
    }

    LiveSettings settings_;
    core::Broadcast servers_;
    std::unique_ptr<core::ReplayBuffer> replay_;
    std::unique_ptr<core::MovieWriter> movie_;  // Set before the thread starts.
    bool movie_finished_ = false;
    Transform video_, audio_;
    bool hardware_ = false;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stopping_ = false;
    std::string error_;
    std::array<ComPtr<IMFSample>, kVideoQueue> frames_;
    std::size_t frame_head_ = 0, frame_count_ = 0;
    std::vector<std::int16_t> ring_;  // Interleaved stereo, kAudioFrames long.
    std::size_t ring_head_ = 0, ring_count_ = 0;
    std::int64_t ring_start_ = 0;     // Timeline sample of the ring's first frame.
    std::int64_t next_ = INT64_MIN;   // Timeline sample after the last queued frame.

    std::vector<std::int16_t> pcm_;
    core::flv::AccessUnit unit_;
    core::Bytes sps_, pps_;
    bool started_ = false, audio_header_ = false;
    std::atomic_bool broken_{false};
    std::atomic<std::uint64_t> dropped_{0};
};

LiveStream::LiveStream() : impl_(std::make_unique<Impl>()) {}
LiveStream::~LiveStream() = default;
Result LiveStream::start(const LiveSettings& settings) { return impl_->start(settings); }
void LiveStream::video(IMFSample* frame) { impl_->video(frame); }
void LiveStream::audio(std::int64_t start, const float* stereo, std::size_t frames) { impl_->audio(start, stereo, frames); }
std::string LiveStream::stop() { return impl_->stop(); }
LiveCounters LiveStream::counters() const { return impl_->counters(); }
std::string LiveStream::failure() const { return impl_->failure(); }
Result LiveStream::save_replay(const std::string& path, double& seconds) const { return impl_->save_replay(path, seconds); }

} // namespace rec::win
