#include "platform/macos/stream.hpp"
#include "core/replay.hpp"
#include "core/rtmp.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace rec::mac {
namespace {
constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kVideoQueue = 4;
constexpr std::size_t kAudioFrames = kRate * 2;
constexpr UInt32 kAacFrames = 1024;
// Returned by the converter's input callback when no more PCM is queued yet.
constexpr OSStatus kNoInput = 1;

std::string failed(const char* what, OSStatus status) {
    return std::string(what) + " (OSStatus " + std::to_string(status) + ")";
}

OSStatus set_number(VTCompressionSessionRef session, CFStringRef key, double value) {
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &value);
    const OSStatus status = VTSessionSetProperty(session, key, number);
    CFRelease(number);
    return status;
}

struct Pending {
    CVPixelBufferRef pixel = nullptr;
    std::uint64_t slot = 0;
};
} // namespace

class LiveStream::Impl {
public:
    ~Impl() { stop(); }

    Result start(const LiveSettings& settings) {
        settings_ = settings;
        if (auto made = create_video(); !made) { release(); return made; }
        if (auto made = create_audio(); !made) { release(); return made; }
        ring_.assign(kAudioFrames * 2, 0.0f);
        if (settings_.replay_seconds) {
            replay_ = std::make_unique<core::ReplayBuffer>(settings_.replay_seconds, settings_.width, settings_.height, settings_.fps);
            replay_->set_priming(static_cast<std::uint32_t>(leading_));
        }
        if (!settings_.movie.empty()) {
            movie_ = std::make_unique<core::MovieWriter>(settings_.width, settings_.height, settings_.fps);
            movie_->set_priming(static_cast<std::uint32_t>(leading_));
            if (auto error = movie_->open(settings_.movie); !error.empty()) { movie_.reset(); release(); return {error}; }
        }
        core::StreamInfo info{settings_.width, settings_.height, settings_.fps, settings_.video_kbps, settings_.audio_kbps};
        if (!settings_.urls.empty())
            if (auto error = servers_.open(settings_.urls, info); !error.empty()) { release(); return {error}; }
        thread_ = std::thread([this] { run(); });
        return {};
    }

    void video(CVPixelBufferRef frame, std::uint64_t slot) {
        if (!frame) return;
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || !thread_.joinable()) return;
            if (frame_count_ == kVideoQueue) {
                CVPixelBufferRelease(frames_[frame_head_].pixel);
                frames_[frame_head_] = {};
                frame_head_ = (frame_head_ + 1) % kVideoQueue;
                --frame_count_;
                dropped_.fetch_add(1);
            }
            CVPixelBufferRetain(frame);
            frames_[(frame_head_ + frame_count_) % kVideoQueue] = {frame, slot};
            ++frame_count_;
        }
        wake_.notify_one();
    }

    void audio(std::int64_t start, const float* stereo, std::size_t frames) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || !thread_.joinable() || !frames) return;
            // Pre-roll before the recording origin never reaches the stream.
            if (start < 0) {
                if (start + static_cast<std::int64_t>(frames) <= 0) return;
                const auto skip = static_cast<std::size_t>(-start);
                if (stereo) stereo += skip * 2;
                frames -= skip;
                start = 0;
            }
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
        c.encoder_dropped = dropped_.load();
        c.dropped = c.encoder_dropped + skipped;
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
        CFMutableDictionaryRef spec = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(spec, kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
        OSStatus status = VTCompressionSessionCreate(kCFAllocatorDefault, static_cast<int32_t>(settings_.width),
                                                     static_cast<int32_t>(settings_.height), kCMVideoCodecType_H264,
                                                     spec, nullptr, kCFAllocatorDefault, &Impl::encoded, this, &session_);
        CFRelease(spec);
        if (status != noErr || !session_) {
            session_ = nullptr;
            return {failed("could not create the stream H.264 encoder", status)};
        }
        // Streaming services expect a steady bitrate, a keyframe every 2 s, and no B-frames.
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
        set_number(session_, kVTCompressionPropertyKey_MaxKeyFrameInterval, settings_.fps * 2.0);
        set_number(session_, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, 2.0);
        set_number(session_, kVTCompressionPropertyKey_ExpectedFrameRate, settings_.fps);
        const double bits = settings_.video_kbps * 1000.0;
        bool constant = false;
        if (__builtin_available(macOS 13.0, *))
            constant = set_number(session_, kVTCompressionPropertyKey_ConstantBitRate, bits) == noErr;
        if (!constant) {
            // Encoders without constant bitrate (Intel Macs): an average, with each second
            // capped at 1.5x so the network sees no long bursts.
            status = set_number(session_, kVTCompressionPropertyKey_AverageBitRate, bits);
            if (status != noErr) return {failed("the H.264 encoder rejected the stream bitrate", status)};
            const double limit[2] = {bits / 8 * 1.5, 1.0};
            CFNumberRef bytes = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &limit[0]);
            CFNumberRef seconds = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &limit[1]);
            const void* values[2] = {bytes, seconds};
            CFArrayRef limits = CFArrayCreate(kCFAllocatorDefault, values, 2, &kCFTypeArrayCallBacks);
            VTSessionSetProperty(session_, kVTCompressionPropertyKey_DataRateLimits, limits);
            CFRelease(limits);
            CFRelease(seconds);
            CFRelease(bytes);
        }
        status = VTCompressionSessionPrepareToEncodeFrames(session_);
        if (status != noErr) return {failed("the stream H.264 encoder could not start", status)};
        CFBooleanRef hardware = nullptr;
        if (VTSessionCopyProperty(session_, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                                  kCFAllocatorDefault, &hardware) == noErr && hardware) {
            hardware_ = CFBooleanGetValue(hardware);
            CFRelease(hardware);
        }
        return {};
    }

    Result create_audio() {
        AudioStreamBasicDescription in{};
        in.mSampleRate = kRate;
        in.mFormatID = kAudioFormatLinearPCM;
        in.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        in.mBitsPerChannel = 32;
        in.mChannelsPerFrame = 2;
        in.mBytesPerFrame = 8;
        in.mFramesPerPacket = 1;
        in.mBytesPerPacket = 8;
        AudioStreamBasicDescription out{};
        out.mSampleRate = kRate;
        out.mFormatID = kAudioFormatMPEG4AAC;
        out.mFormatFlags = kMPEG4Object_AAC_LC;
        out.mChannelsPerFrame = 2;
        out.mFramesPerPacket = kAacFrames;
        OSStatus status = AudioConverterNew(&in, &out, &converter_);
        if (status != noErr || !converter_) {
            converter_ = nullptr;
            return {failed("no AAC encoder is available for streaming", status)};
        }
        UInt32 rate = settings_.audio_kbps * 1000, size = sizeof rate;
        AudioConverterSetProperty(converter_, kAudioConverterEncodeBitRate, size, &rate);
        UInt32 largest = 0;
        size = sizeof largest;
        if (AudioConverterGetProperty(converter_, kAudioConverterPropertyMaximumOutputPacketSize, &size, &largest) != noErr ||
            !largest)
            largest = 6144;
        packet_.resize(largest);
        // The encoder's output starts this many frames before its first input frame.
        AudioConverterPrimeInfo prime{};
        size = sizeof prime;
        leading_ = AudioConverterGetProperty(converter_, kAudioConverterPrimeInfo, &size, &prime) == noErr
            ? prime.leadingFrames : 2112;
        return {};
    }

    void release() {
        if (session_) {
            VTCompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        if (converter_) {
            AudioConverterDispose(converter_);
            converter_ = nullptr;
        }
        std::lock_guard lock(mutex_);
        while (frame_count_) {
            CVPixelBufferRelease(frames_[frame_head_].pixel);
            frames_[frame_head_] = {};
            frame_head_ = (frame_head_ + 1) % kVideoQueue;
            --frame_count_;
        }
    }

    void write(const float* stereo, std::size_t frames) {
        for (std::size_t i = 0; i < frames; ++i) {
            if (ring_count_ == kAudioFrames) {
                ring_head_ = (ring_head_ + 1) % kAudioFrames;
                --ring_count_;
                ++ring_start_;
            }
            const auto at = (ring_head_ + ring_count_) % kAudioFrames * 2;
            ring_[at] = stereo ? std::clamp(stereo[i * 2], -1.0f, 1.0f) : 0.0f;
            ring_[at + 1] = stereo ? std::clamp(stereo[i * 2 + 1], -1.0f, 1.0f) : 0.0f;
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
        std::int64_t stop_deadline = 0;
        const auto now_ms = [] {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        };
        for (;;) {
            bool done = false;
            {
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(20),
                               [this] { return stopping_ || frame_count_ || ring_count_ >= kAacFrames; });
                if (stopping_ && !stop_deadline) stop_deadline = now_ms() + 2000;
                done = stopping_ && ((!frame_count_ && !ring_count_) || now_ms() > stop_deadline);
            }
            for (;;) {
                Pending frame;
                {
                    std::lock_guard lock(mutex_);
                    if (!frame_count_) break;
                    frame = frames_[frame_head_];
                    frames_[frame_head_] = {};
                    frame_head_ = (frame_head_ + 1) % kVideoQueue;
                    --frame_count_;
                }
                if (!broken_.load()) encode_video(frame);
                CVPixelBufferRelease(frame.pixel);
                // A video encoder slower than real time keeps this loop full; audio must not
                // wait behind it, or its ring overflows and the stream loses its start.
                encode_audio();
            }
            encode_audio();
            if (done) break;
        }
        // Delivers every frame still in the encoder through the output callback.
        if (session_ && !broken_.load()) VTCompressionSessionCompleteFrames(session_, kCMTimeInvalid);
        servers_.close(3000);
    }

    void encode_video(const Pending& frame) {
        const auto fps = static_cast<int32_t>(settings_.fps);
        const OSStatus status = VTCompressionSessionEncodeFrame(
            session_, frame.pixel, CMTimeMake(static_cast<int64_t>(frame.slot), fps), CMTimeMake(1, fps),
            nullptr, nullptr, nullptr);
        if (status != noErr) fail(failed("stream video encoding failed", status));
    }

    // VideoToolbox calls this serially for one session, on its own thread or inside
    // CompleteFrames. The publisher only queues, so nothing here blocks the encoder.
    static void encoded(void* self, void*, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
        static_cast<Impl*>(self)->send_video(status, flags, sample);
    }

    void send_video(OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
        if (status != noErr) { fail(failed("stream video encoding failed", status)); return; }
        if (!sample || (flags & kVTEncodeInfo_FrameDropped) || !CMSampleBufferDataIsReady(sample)) {
            dropped_.fetch_add(1);
            return;
        }
        bool key = true;
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        if (attachments && CFArrayGetCount(attachments) > 0) {
            const auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
            const void* not_sync = nullptr;
            if (CFDictionaryGetValueIfPresent(info, kCMSampleAttachmentKey_NotSync, &not_sync) && not_sync &&
                CFBooleanGetValue(static_cast<CFBooleanRef>(not_sync)))
                key = false;
        }
        const double seconds = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample));
        const auto ms = static_cast<std::uint32_t>(std::max<long long>(0, std::llround(seconds * 1000.0)));
        if (CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample)) {
            const std::uint8_t *sps = nullptr, *pps = nullptr;
            std::size_t sps_size = 0, pps_size = 0, count = 0;
            int length = 0;
            if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &sps, &sps_size, &count, &length) == noErr &&
                CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 1, &pps, &pps_size, nullptr, nullptr) == noErr &&
                sps_size >= 4 && pps_size) {
                nal_length_ = length;
                if (!std::equal(sps, sps + sps_size, sps_.begin(), sps_.end()) ||
                    !std::equal(pps, pps + pps_size, pps_.begin(), pps_.end())) {
                    sps_.assign(sps, sps + sps_size);
                    pps_.assign(pps, pps + pps_size);
                    send_video(ms, true, true, core::flv::avc_sequence_header(sps_, pps_));
                }
            }
        }
        if (sps_.empty() || (!started_ && !key)) { dropped_.fetch_add(1); return; }
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
        const std::size_t size = block ? CMBlockBufferGetDataLength(block) : 0;
        data_.resize(size);
        if (!size || CMBlockBufferCopyDataBytes(block, 0, size, data_.data()) != kCMBlockBufferNoErr) {
            dropped_.fetch_add(1);
            return;
        }
        // Length-prefixed NAL units as VideoToolbox wrote them, re-prefixed with 4 bytes as
        // FLV expects; parameter sets travel in the sequence header instead.
        core::Bytes body{static_cast<std::uint8_t>(key ? 0x17 : 0x27), 0x01, 0, 0, 0};
        body.reserve(size + 16);
        const auto prefix = static_cast<std::size_t>(nal_length_ >= 1 && nal_length_ <= 4 ? nal_length_ : 4);
        for (std::size_t at = 0; at + prefix <= size;) {
            std::size_t n = 0;
            for (std::size_t i = 0; i < prefix; ++i) n = n << 8 | data_[at + i];
            at += prefix;
            if (n > size - at) break;
            const int type = data_[at] & 0x1f;
            if (n && type != 7 && type != 8 && type != 9) {
                for (int shift = 24; shift >= 0; shift -= 8) body.push_back(static_cast<std::uint8_t>(n >> shift));
                body.insert(body.end(), data_.begin() + static_cast<std::ptrdiff_t>(at),
                            data_.begin() + static_cast<std::ptrdiff_t>(at + n));
            }
            at += n;
        }
        if (body.size() == 5) { dropped_.fetch_add(1); return; }
        started_ = true;
        send_video(ms, key, false, std::move(body));
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

    static OSStatus supply(AudioConverterRef, UInt32* packets, AudioBufferList* data,
                           AudioStreamPacketDescription** descriptions, void* self) {
        auto& impl = *static_cast<Impl*>(self);
        const auto available = static_cast<UInt32>(impl.staged_.size() / 2 - impl.staged_used_);
        if (descriptions) *descriptions = nullptr;
        if (!available) {
            *packets = 0;
            data->mBuffers[0].mData = nullptr;
            data->mBuffers[0].mDataByteSize = 0;
            return kNoInput;
        }
        *packets = std::min(*packets, available);
        data->mBuffers[0].mNumberChannels = 2;
        data->mBuffers[0].mData = impl.staged_.data() + impl.staged_used_ * 2;
        data->mBuffers[0].mDataByteSize = *packets * 8;
        impl.staged_used_ += *packets;
        return noErr;
    }

    void encode_audio() {
        {
            std::lock_guard lock(mutex_);
            if (ring_count_) {
                // A ring overflow skipped audio: move the encoder's timeline past the gap.
                if (fed_ == INT64_MIN) fed_ = base_ = ring_start_;
                else if (ring_start_ > fed_) base_ += ring_start_ - fed_;
                fed_ = ring_start_ + static_cast<std::int64_t>(ring_count_);
                staged_.erase(staged_.begin(), staged_.begin() + static_cast<std::ptrdiff_t>(staged_used_ * 2));
                staged_used_ = 0;
                for (std::size_t i = 0; i < ring_count_; ++i) {
                    const auto at = (ring_head_ + i) % kAudioFrames * 2;
                    staged_.push_back(ring_[at]);
                    staged_.push_back(ring_[at + 1]);
                }
                ring_head_ = (ring_head_ + ring_count_) % kAudioFrames;
                ring_start_ += static_cast<std::int64_t>(ring_count_);
                ring_count_ = 0;
            }
        }
        while (!broken_.load() && staged_.size() / 2 - staged_used_ >= kAacFrames) {
            AudioBufferList out{};
            out.mNumberBuffers = 1;
            out.mBuffers[0].mNumberChannels = 2;
            out.mBuffers[0].mDataByteSize = static_cast<UInt32>(packet_.size());
            out.mBuffers[0].mData = packet_.data();
            UInt32 packets = 1;
            AudioStreamPacketDescription description{};
            const OSStatus status = AudioConverterFillComplexBuffer(converter_, &Impl::supply, this, &packets, &out, &description);
            if (status != noErr && status != kNoInput) { fail(failed("stream audio encoding failed", status)); return; }
            if (!packets) {
                if (status == kNoInput) break;
                continue;
            }
            // Packet k decodes to the input that began k * 1024 - leading frames after the first.
            const std::int64_t first = base_ + aac_packets_ * kAacFrames - leading_;
            ++aac_packets_;
            if (first + static_cast<std::int64_t>(kAacFrames) <= 0) continue;
            const auto ms = static_cast<std::uint32_t>(std::max<std::int64_t>(0, (first * 1000 + kRate / 2) / kRate));
            if (!audio_header_) {
                send_audio(ms, true, core::flv::aac_sequence_header(kRate, 2));
                audio_header_ = true;
            }
            if (out.mBuffers[0].mDataByteSize)
                send_audio(ms, false, core::flv::aac_frame(packet_.data(), out.mBuffers[0].mDataByteSize));
        }
    }

    LiveSettings settings_;
    core::Broadcast servers_;
    std::unique_ptr<core::ReplayBuffer> replay_;
    std::unique_ptr<core::MovieWriter> movie_;  // Set before the thread starts.
    bool movie_finished_ = false;
    VTCompressionSessionRef session_ = nullptr;
    AudioConverterRef converter_ = nullptr;
    bool hardware_ = false;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stopping_ = false;
    std::string error_;
    std::array<Pending, kVideoQueue> frames_{};
    std::size_t frame_head_ = 0, frame_count_ = 0;
    std::vector<float> ring_;         // Interleaved stereo, kAudioFrames long.
    std::size_t ring_head_ = 0, ring_count_ = 0;
    std::int64_t ring_start_ = 0;     // Timeline sample of the ring's first frame.
    std::int64_t next_ = INT64_MIN;   // Timeline sample after the last queued frame.

    // Stream thread only.
    std::vector<float> staged_;       // PCM handed to the AAC converter, interleaved stereo.
    std::size_t staged_used_ = 0;     // Frames of staged_ already consumed.
    std::int64_t fed_ = INT64_MIN;    // Timeline sample after the last staged frame.
    std::int64_t base_ = 0;           // Timeline sample of the converter's first input frame.
    std::int64_t aac_packets_ = 0;
    std::int64_t leading_ = 2112;
    std::vector<std::uint8_t> packet_;
    bool audio_header_ = false;

    // Encoder callback only.
    std::vector<std::uint8_t> data_;
    core::Bytes sps_, pps_;
    int nal_length_ = 4;
    bool started_ = false;

    std::atomic_bool broken_{false};
    std::atomic<std::uint64_t> dropped_{0};
};

LiveStream::LiveStream() : impl_(std::make_unique<Impl>()) {}
LiveStream::~LiveStream() = default;
Result LiveStream::start(const LiveSettings& settings) { return impl_->start(settings); }
void LiveStream::video(CVPixelBufferRef frame, std::uint64_t slot) { impl_->video(frame, slot); }
void LiveStream::audio(std::int64_t start, const float* stereo, std::size_t frames) { impl_->audio(start, stereo, frames); }
std::string LiveStream::stop() { return impl_->stop(); }
LiveCounters LiveStream::counters() const { return impl_->counters(); }
std::string LiveStream::failure() const { return impl_->failure(); }
Result LiveStream::save_replay(const std::string& path, double& seconds) const { return impl_->save_replay(path, seconds); }

} // namespace rec::mac
