#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <VideoToolbox/VideoToolbox.h>

#include "rec/rec.hpp"
#include "core/mixer.hpp"
#include "core/overlay.hpp"
#include "core/pause.hpp"
#include "core/preview.hpp"
#include "core/sync.hpp"
#include "platform/macos/overlay.hpp"
#include "platform/macos/screen.h"
#include "platform/macos/stream.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <unistd.h>

namespace rec {
void receive_frame(void* owner, CMSampleBufferRef sample);
void receive_audio(void* owner, CMSampleBufferRef sample, bool microphone);
void receive_failure(void* owner, NSError* error);
}

@interface RecCaptureOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) void* owner;
@end

@implementation RecCaptureOutput
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)buffer
       ofType:(SCStreamOutputType)type {
    (void)stream;
    if (!self.owner) return;
    if (type == SCStreamOutputTypeScreen) {
        rec::receive_frame(self.owner, buffer);
    } else if (type == SCStreamOutputTypeAudio) {
        rec::receive_audio(self.owner, buffer, false);
    } else if (@available(macOS 15.0, *)) {
        if (type == SCStreamOutputTypeMicrophone) rec::receive_audio(self.owner, buffer, true);
    }
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    (void)stream;
    if (self.owner) rec::receive_failure(self.owner, error);
}
@end

namespace rec {
namespace mac {
std::string message(NSError* error, const char* fallback) {
    // ScreenCaptureKit reports a missing permission as "The user declined TCCs ...".
    if ([error.domain isEqualToString:SCStreamErrorDomain] && error.code == SCStreamErrorUserDeclined)
        return "Screen Recording permission is off. Turn it on for this app in System Settings > Privacy & "
               "Security > Screen & System Audio Recording, then quit and reopen the app.";
    return error ? std::string(error.localizedDescription.UTF8String ?: fallback) : std::string(fallback);
}

SCShareableContent* shareable(std::string& error) {
    __block SCShareableContent* content = nil;
    __block NSError* reported = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* found, NSError* e) {
        content = found;
        reported = e;
        dispatch_semaphore_signal(done);
    }];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC))) {
        error = "ScreenCaptureKit did not return shareable content in 10 seconds";
        return nil;
    }
    if (!content) error = message(reported, "ScreenCaptureKit could not list displays. Check Screen Recording permission.");
    return content;
}

PixelSize native_size(SCDisplay* display) {
    auto mode = CGDisplayCopyDisplayMode(display.displayID);
    const std::uint32_t w = mode ? static_cast<std::uint32_t>(CGDisplayModeGetPixelWidth(mode)) : display.width;
    const std::uint32_t h = mode ? static_cast<std::uint32_t>(CGDisplayModeGetPixelHeight(mode)) : display.height;
    if (mode) CGDisplayModeRelease(mode);
    return {w, h};
}

double pixels_per_point(SCShareableContent* content, CGRect frame) {
    double best = 0, scale = 1;
    for (SCDisplay* display in content.displays) {
        const CGRect overlap = CGRectIntersection(display.frame, frame);
        const double area = CGRectIsNull(overlap) ? 0 : overlap.size.width * overlap.size.height;
        if (area > best && display.width > 0) {
            best = area;
            scale = static_cast<double>(native_size(display).width) / display.width;
        }
    }
    return scale;
}

Result wait_capture(SCStream* stream, bool start) {
    __block NSError* reported = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    void (^completion)(NSError*) = ^(NSError* error) {
        reported = error;
        dispatch_semaphore_signal(done);
    };
    if (start) [stream startCaptureWithCompletionHandler:completion];
    else [stream stopCaptureWithCompletionHandler:completion];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)))
        return {start ? "screen capture start timed out" : "screen capture stop timed out"};
    return reported ? Result{message(reported, "screen capture failed")} : Result{};
}
} // namespace mac

namespace {
using core::kMixRate;
using mac::message;
using mac::native_size;
using mac::pixels_per_point;
using mac::shareable;
using mac::wait_capture;
// Video is emitted this far behind real time, so the frame that was on screen at each
// slot instant has already been delivered. ScreenCaptureKit delivers frames a few
// milliseconds after their presentation timestamps.
constexpr auto kEmitDelay = std::chrono::milliseconds(40);
// A stalled source holds the mix back at most this long; it is then filled with silence.
constexpr std::int64_t kMixWaitFrames = kMixRate * 3 / 10;

constexpr std::int32_t kNanos = 1000000000;

// Host-clock instants as integer nanoseconds, the unit of the pause timeline.
std::int64_t nanos(CMTime time) { return CMTimeConvertScale(time, kNanos, kCMTimeRoundingMethod_RoundHalfAwayFromZero).value; }
std::int64_t now_nanos() { return nanos(CMClockGetTime(CMClockGetHostTimeClock())); }

struct Size { std::uint32_t width, height; };

// Normal app windows (layer 0) that are on screen, not rec's own.
bool listable(SCWindow* window) {
    return window.windowLayer == 0 && window.onScreen && window.frame.size.width >= 2 && window.frame.size.height >= 2 &&
           window.owningApplication && window.owningApplication.processID != getpid();
}

Size output_size(const Options& options, std::uint32_t native_w, std::uint32_t native_h) {
    if (!options.width || !options.height) return {native_w & ~1u, native_h & ~1u};
    const double factor = std::min({1.0, static_cast<double>(options.width) / native_w,
                                    static_cast<double>(options.height) / native_h});
    return {std::max(2u, static_cast<std::uint32_t>(native_w * factor) & ~1u),
            std::max(2u, static_cast<std::uint32_t>(native_h * factor) & ~1u)};
}

Size output_size(const Options& options, SCDisplay* display) {
    const auto native = native_size(display);
    return output_size(options, native.width, native.height);
}

bool window_gone(CGWindowID window) {
    CFArrayRef found = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, window);
    const bool gone = !found || CFArrayGetCount(found) == 0;
    if (found) CFRelease(found);
    return gone;
}

template <class T>
bool audio_property(AudioObjectID object, AudioObjectPropertySelector selector,
                    AudioObjectPropertyScope scope, T& value) {
    AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) == noErr;
}

// ScreenCaptureKit captures system audio when it is mixed, one output latency before it is heard.
double output_latency_seconds() {
    AudioDeviceID device = kAudioObjectUnknown;
    if (!audio_property(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice,
                        kAudioObjectPropertyScopeGlobal, device) || device == kAudioObjectUnknown)
        return 0;
    Float64 rate = 0;
    if (!audio_property(device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, rate) ||
        rate <= 0)
        return 0;
    UInt32 latency = 0, safety = 0, buffer = 0, stream_latency = 0;
    audio_property(device, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, latency);
    audio_property(device, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, safety);
    audio_property(device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, buffer);
    AudioObjectPropertyAddress streams{kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &streams, 0, nullptr, &size) == noErr &&
        size >= sizeof(AudioStreamID)) {
        std::vector<AudioStreamID> ids(size / sizeof(AudioStreamID));
        if (AudioObjectGetPropertyData(device, &streams, 0, nullptr, &size, ids.data()) == noErr && !ids.empty())
            audio_property(ids[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, stream_latency);
    }
    const double seconds = (static_cast<double>(latency) + safety + buffer + stream_latency) / rate;
    return std::clamp(seconds, 0.0, 0.5);
}

NSArray<AVCaptureDevice*>* microphones() {
    if (@available(macOS 14.0, *)) {
        return [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeMicrophone]
                                  mediaType:AVMediaTypeAudio
                                   position:AVCaptureDevicePositionUnspecified].devices;
    }
    return @[];
}

AVCaptureDevice* find_microphone(const std::string& query, std::string& error) {
    if (query.empty() || query == "default") {
        AVCaptureDevice* device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
        if (!device) error = "no microphone found";
        return device;
    }
    NSString* needle = [NSString stringWithUTF8String:query.c_str()];
    if (!needle) { error = "invalid microphone name"; return nil; }
    NSArray<AVCaptureDevice*>* devices = microphones();
    for (AVCaptureDevice* device in devices)
        if ([device.uniqueID isEqualToString:needle]) return device;
    for (AVCaptureDevice* device in devices)
        if ([device.localizedName rangeOfString:needle options:NSCaseInsensitiveSearch].location != NSNotFound)
            return device;
    error = "microphone not found: " + query + " (run `rec devices`)";
    return nil;
}

Result microphone_access() {
    const auto status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (status == AVAuthorizationStatusAuthorized) return {};
    if (status == AVAuthorizationStatusNotDetermined) {
        __block BOOL granted = NO;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL allowed) {
            granted = allowed;
            dispatch_semaphore_signal(done);
        }];
        if (!dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC)) && granted)
            return {};
    }
    return {"microphone access denied. Allow it in System Settings > Privacy & Security > Microphone, then retry."};
}

CMAudioFormatDescriptionRef pcm_format(std::uint32_t channels, bool is_float) {
    AudioStreamBasicDescription format{};
    format.mSampleRate = kMixRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = (is_float ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) |
                          kAudioFormatFlagIsPacked;
    format.mBitsPerChannel = is_float ? 32 : 16;
    format.mChannelsPerFrame = channels;
    format.mBytesPerFrame = channels * format.mBitsPerChannel / 8;
    format.mFramesPerPacket = 1;
    format.mBytesPerPacket = format.mBytesPerFrame;
    CMAudioFormatDescriptionRef description = nullptr;
    if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &format, 0, nullptr, 0, nullptr, nullptr,
                                       &description) != noErr)
        return nullptr;
    return description;
}

// Interleaved PCM in format at a 48 kHz timestamp. The caller releases the result.
CMSampleBufferRef pcm_buffer(CMAudioFormatDescriptionRef format, const void* data, std::size_t bytes,
                             std::size_t frames, CMTime pts) {
    if (!format || !frames) return nullptr;
    CMBlockBufferRef block = nullptr;
    if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, bytes, kCFAllocatorDefault,
                                           nullptr, 0, bytes, 0, &block) != kCMBlockBufferNoErr)
        return nullptr;
    if (CMBlockBufferReplaceDataBytes(data, block, 0, bytes) != kCMBlockBufferNoErr) {
        CFRelease(block);
        return nullptr;
    }
    CMSampleTimingInfo timing{CMTimeMake(1, kMixRate), pts, kCMTimeInvalid};
    CMSampleBufferRef sample = nullptr;
    const auto status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, format,
                                                  static_cast<CMItemCount>(frames), 1, &timing, 0,
                                                  nullptr, &sample);
    CFRelease(block);
    return status == noErr ? sample : nullptr;
}

// A copy of a 16/32-bit integer or 32-bit float PCM buffer with its samples scaled along
// ramp, in the same format and at the same time. Null for other formats. The caller
// releases the result.
CMSampleBufferRef scaled_pcm(CMSampleBufferRef sample, const core::Fader::Ramp& ramp, std::vector<std::byte>& scratch) {
    CMAudioFormatDescriptionRef description = CMSampleBufferGetFormatDescription(sample);
    const auto* format = CMAudioFormatDescriptionGetStreamBasicDescription(description);
    if (!format || format->mFormatID != kAudioFormatLinearPCM || format->mChannelsPerFrame == 0 ||
        (format->mFormatFlags & kAudioFormatFlagIsBigEndian))
        return nullptr;
    const bool is_float = format->mFormatFlags & kAudioFormatFlagIsFloat;
    const auto bits = format->mBitsPerChannel;
    if (!(is_float ? bits == 32 : (bits == 16 || bits == 32))) return nullptr;
    const auto frames = static_cast<std::size_t>(CMSampleBufferGetNumSamples(sample));
    if (!frames) return nullptr;
    const bool planar = format->mFormatFlags & kAudioFormatFlagIsNonInterleaved;
    const std::size_t per_frame = planar ? 1 : format->mChannelsPerFrame;
    const std::size_t planes = planar ? format->mChannelsPerFrame : 1;
    const std::size_t plane_bytes = frames * per_frame * (bits / 8);

    const auto flags = kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment;
    std::size_t list_size = 0;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, &list_size, nullptr, 0, nullptr, nullptr,
                                                                flags, nullptr) != noErr)
        return nullptr;
    scratch.resize(list_size);
    auto* list = reinterpret_cast<AudioBufferList*>(scratch.data());
    CMBlockBufferRef source = nullptr;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, nullptr, list, list_size, nullptr, nullptr,
                                                                flags, &source) != noErr)
        return nullptr;
    bool readable = list->mNumberBuffers == planes;
    for (UInt32 i = 0; readable && i < list->mNumberBuffers; ++i)
        readable = list->mBuffers[i].mData && list->mBuffers[i].mDataByteSize >= plane_bytes;
    CMBlockBufferRef block = nullptr;
    char* data = nullptr;
    // Planes follow one another in the new buffer, as CMSampleBufferCreate expects.
    if (!readable ||
        CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, plane_bytes * planes, kCFAllocatorDefault,
                                           nullptr, 0, plane_bytes * planes, kCMBlockBufferAssureMemoryNowFlag,
                                           &block) != kCMBlockBufferNoErr ||
        CMBlockBufferGetDataPointer(block, 0, nullptr, nullptr, &data) != kCMBlockBufferNoErr) {
        if (block) CFRelease(block);
        if (source) CFRelease(source);
        return nullptr;
    }
    const float step = (ramp.to - ramp.from) / static_cast<float>(frames);
    for (std::size_t p = 0; p < planes; ++p) {
        std::memcpy(data + p * plane_bytes, list->mBuffers[p].mData, plane_bytes);
        for (std::size_t i = 0; i < frames * per_frame; ++i) {
            const float g = ramp.from + step * static_cast<float>(i / per_frame + 1);
            const std::size_t at = p * frames * per_frame + i;
            if (is_float) {
                reinterpret_cast<float*>(data)[at] *= g;
            } else if (bits == 16) {
                auto& v = reinterpret_cast<std::int16_t*>(data)[at];
                v = static_cast<std::int16_t>(std::lround(std::clamp(v * g, -32768.0f, 32767.0f)));
            } else {
                auto& v = reinterpret_cast<std::int32_t*>(data)[at];
                v = static_cast<std::int32_t>(std::clamp(static_cast<double>(v) * g, -2147483648.0, 2147483647.0));
            }
        }
    }
    if (source) CFRelease(source);
    CMSampleBufferRef out = nullptr;
    const auto status = CMAudioSampleBufferCreateReadyWithPacketDescriptions(
        kCFAllocatorDefault, block, description, static_cast<CMItemCount>(frames),
        CMSampleBufferGetPresentationTimeStamp(sample), nullptr, &out);
    CFRelease(block);
    return status == noErr ? out : nullptr;
}

// Reads 16/32-bit integer or 32-bit float PCM, interleaved or planar, as interleaved
// stereo float at 48 kHz. Mono is duplicated; channels after the second are ignored.
bool stereo_pcm(CMSampleBufferRef sample, std::vector<float>& out, std::vector<std::byte>& scratch) {
    out.clear();
    const auto* format = CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(sample));
    if (!format || format->mFormatID != kAudioFormatLinearPCM || format->mSampleRate <= 0 ||
        format->mChannelsPerFrame == 0 || (format->mFormatFlags & kAudioFormatFlagIsBigEndian))
        return false;
    const bool is_float = format->mFormatFlags & kAudioFormatFlagIsFloat;
    const auto bits = format->mBitsPerChannel;
    if (!(is_float ? bits == 32 : (bits == 16 || bits == 32))) return false;
    const auto frames = static_cast<std::size_t>(CMSampleBufferGetNumSamples(sample));
    if (!frames) return true;

    const auto flags = kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment;
    std::size_t list_size = 0;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, &list_size, nullptr, 0, nullptr,
                                                                nullptr, flags, nullptr) != noErr)
        return false;
    scratch.resize(list_size);
    auto* list = reinterpret_cast<AudioBufferList*>(scratch.data());
    CMBlockBufferRef block = nullptr;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, nullptr, list, list_size, nullptr,
                                                                nullptr, flags, &block) != noErr)
        return false;
    const bool planar = format->mFormatFlags & kAudioFormatFlagIsNonInterleaved;
    const UInt32 channels = format->mChannelsPerFrame;
    const std::size_t needed = frames * (bits / 8) * (planar ? 1 : channels);
    bool readable = list->mNumberBuffers > 0;
    for (UInt32 i = 0; readable && i < list->mNumberBuffers; ++i)
        readable = list->mBuffers[i].mData && list->mBuffers[i].mDataByteSize >= needed;
    if (!readable) {
        if (block) CFRelease(block);
        return false;
    }
    auto read = [&](UInt32 channel, std::size_t frame) -> float {
        channel = std::min(channel, channels - 1);
        const AudioBuffer& buffer = list->mBuffers[planar ? std::min(channel, list->mNumberBuffers - 1) : 0];
        const std::size_t index = planar ? frame : frame * channels + channel;
        if (is_float) return static_cast<const float*>(buffer.mData)[index];
        if (bits == 16) return static_cast<const std::int16_t*>(buffer.mData)[index] / 32768.0f;
        return static_cast<const std::int32_t*>(buffer.mData)[index] / 2147483648.0f;
    };
    const double ratio = format->mSampleRate / kMixRate;
    if (ratio == 1.0) {
        out.resize(frames * 2);
        for (std::size_t i = 0; i < frames; ++i) {
            out[i * 2] = read(0, i);
            out[i * 2 + 1] = read(1, i);
        }
    } else {
        // Linear, per buffer. The microphone stem is converted by AVFoundation instead.
        const auto converted = static_cast<std::size_t>(std::llround(frames / ratio));
        out.resize(converted * 2);
        for (std::size_t i = 0; i < converted; ++i) {
            const double position = i * ratio;
            const auto first = std::min(static_cast<std::size_t>(position), frames - 1);
            const auto second = std::min(first + 1, frames - 1);
            const auto t = static_cast<float>(position - static_cast<double>(first));
            for (UInt32 c = 0; c < 2; ++c) {
                const float a = read(c, first);
                out[i * 2 + c] = a + (read(c, second) - a) * t;
            }
        }
    }
    if (block) CFRelease(block);
    return true;
}

CVPixelBufferRef pattern_frame(Size size, bool flash) {
    CVPixelBufferRef pixel = nullptr;
    NSDictionary* attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
    if (CVPixelBufferCreate(kCFAllocatorDefault, size.width, size.height,
                            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                            (__bridge CFDictionaryRef)attributes, &pixel) != kCVReturnSuccess)
        return nullptr;
    CVPixelBufferLockBaseAddress(pixel, 0);
    auto* y = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel, 0));
    const auto y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 0);
    for (std::size_t row = 0; row < CVPixelBufferGetHeightOfPlane(pixel, 0); ++row)
        std::memset(y + row * y_stride, flash ? 235 : (row % 24 < 12 ? 120 : 40),
                    CVPixelBufferGetWidthOfPlane(pixel, 0));
    auto* uv = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel, 1));
    const auto uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 1);
    for (std::size_t row = 0; row < CVPixelBufferGetHeightOfPlane(pixel, 1); ++row)
        std::memset(uv + row * uv_stride, 128, CVPixelBufferGetWidthOfPlane(pixel, 1) * 2);
    CVPixelBufferUnlockBaseAddress(pixel, 0);
    // Tagged like a capture, so overlays convert with the same matrix.
    CVBufferSetAttachment(pixel, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixel, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixel, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    return pixel;
}

// Fixed-capacity FIFO of retained sample buffers. Overflow releases the oldest.
class PacketRing {
public:
    ~PacketRing() { clear(); }
    // Takes ownership. Returns false when the oldest packet was dropped to make room.
    bool push(CMSampleBufferRef sample) {
        bool kept = true;
        if (count_ == items_.size()) {
            CFRelease(pop());
            kept = false;
        }
        items_[(head_ + count_) % items_.size()] = sample;
        ++count_;
        return kept;
    }
    CMSampleBufferRef pop() {
        if (!count_) return nullptr;
        auto* sample = items_[head_];
        items_[head_] = nullptr;
        head_ = (head_ + 1) % items_.size();
        --count_;
        return sample;
    }
    void clear() {
        while (auto* sample = pop()) CFRelease(sample);
    }

private:
    std::array<CMSampleBufferRef, 16> items_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

struct Frame {
    CVPixelBufferRef pixel = nullptr;
    CMTime pts = kCMTimeInvalid;
};

// Frames delivered but not yet due. Overflow releases the oldest, which a newer frame supersedes.
class FrameRing {
public:
    ~FrameRing() { clear(); }
    void push(Frame frame) {
        if (count_ == items_.size()) CVPixelBufferRelease(pop().pixel);
        items_[(head_ + count_) % items_.size()] = frame;
        ++count_;
    }
    bool empty() const { return count_ == 0; }
    const Frame& front() const { return items_[head_]; }
    Frame pop() {
        Frame frame = items_[head_];
        items_[head_] = {};
        head_ = (head_ + 1) % items_.size();
        --count_;
        return frame;
    }
    void clear() {
        while (count_) CVPixelBufferRelease(pop().pixel);
    }

private:
    std::array<Frame, 3> items_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

enum Source : std::size_t { kSystem = 0, kMic = 1 };

struct AudioSource {
    bool enabled = false;
    PacketRing queue;                    // Filled by the capture callback; guarded by CaptureState::mutex_.
    CMSampleBufferRef held = nullptr;    // Dequeued, waiting for its writer input to accept it.
    AVAssetWriterInput* __strong stem = nil;
    CMTime offset = kCMTimeZero;
    core::Fader fader;                   // Volume and mute of the program audio.
    std::int64_t next_mix = INT64_MIN;   // Keeps consecutive buffers contiguous in the mix.
    std::atomic<std::uint64_t> written{0}, dropped{0};
    mutable std::atomic<float> peak{0};  // Level meter; stats() takes it.
};
} // namespace

std::vector<Display> list_displays(std::string& error) {
    @autoreleasepool {
        error.clear();
        SCShareableContent* content = shareable(error);
        if (!content) return {};
        std::vector<Display> result;
        for (SCDisplay* display in content.displays) {
            auto size = output_size(Options{.width = 0, .height = 0}, display);
            result.push_back({display.displayID, size.width, size.height,
                              "Display " + std::to_string(display.displayID)});
        }
        return result;
    }
}

std::vector<Window> list_windows(std::string& error) {
    @autoreleasepool {
        error.clear();
        SCShareableContent* content = shareable(error);
        if (!content) return {};
        std::vector<Window> result;
        for (SCWindow* window in content.windows) {
            if (!listable(window)) continue;
            const double scale = pixels_per_point(content, window.frame);
            result.push_back({window.windowID, static_cast<std::uint32_t>(std::lround(window.frame.size.width * scale)),
                              static_cast<std::uint32_t>(std::lround(window.frame.size.height * scale)),
                              window.title ? std::string(window.title.UTF8String ?: "") : std::string(),
                              std::string(window.owningApplication.applicationName.UTF8String ?: "")});
        }
        return result;
    }
}

std::vector<AudioDevice> list_microphones(std::string& error) {
    @autoreleasepool {
        error.clear();
        std::vector<AudioDevice> result;
        for (AVCaptureDevice* device in microphones())
            result.push_back({device.uniqueID.UTF8String ?: "", device.localizedName.UTF8String ?: ""});
        return result;
    }
}

std::vector<Camera> list_cameras(std::string& error) {
    error.clear();
    return mac::cameras();
}

double default_output_latency_ms() noexcept { return output_latency_seconds() * 1000.0; }

// Capture callbacks only retain and enqueue. One recording thread encodes video on a
// constant-frame-rate grid, routes audio to stems and the mix, and never blocks a callback.
class CaptureState {
public:
    explicit CaptureState(Options options) : options_(std::move(options)) {}
    ~CaptureState() { stop(); }

    Result start() {
        @autoreleasepool {
            if (active_) return {"already recording"};
            window_closed_.store(false);
            if (options_.preview_only) {
                if (!options_.preview_width) return {"a preview needs a preview width"};
                options_.system_audio = options_.microphone = options_.test_tone = options_.test_mic = false;
                options_.stream_urls.clear();
                options_.canvas2.stream_urls.clear();
                options_.canvas2.output.clear();
                options_.replay_seconds = 0;
                options_.duration_seconds = 0;
            }
            if ((!options_.stream_urls.empty() || options_.replay_seconds) && (options_.stream_kbps < 500 || options_.stream_kbps > 50000))
                return {"stream bitrate must be between 500 and 50000 kb/s"};
            if (options_.replay_seconds && (options_.replay_seconds < 5 || options_.replay_seconds > 300))
                return {"replay must be between 5 and 300 seconds"};
            if (options_.stream_urls.size() > kMaxStreams) return {"at most " + std::to_string(kMaxStreams) + " streams"};
            if (auto error = core::check_canvas(options_); !error.empty()) return {error};
            if (!options_.preview_only && !options_.canvas2.output.empty()) {
                NSString* second = [NSString stringWithUTF8String:options_.canvas2.output.c_str()];
                if (!second) return {"invalid output path for the second canvas"};
                if ([[NSFileManager defaultManager] fileExistsAtPath:second])
                    return {"output file already exists: " + options_.canvas2.output};
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
                NSString* path = [NSString stringWithUTF8String:options_.output.c_str()];
                if (!path) return {"invalid output path"};
                if ([[NSFileManager defaultManager] fileExistsAtPath:path])
                    return {"output file already exists: " + options_.output};
                if (options_.codec == Codec::prores && [path.pathExtension.lowercaseString isEqualToString:@"mp4"])
                    return {"ProRes needs a .mov output"};
            }

            audio_[kSystem].enabled = options_.system_audio || options_.test_tone;
            audio_[kMic].enabled = options_.microphone || options_.test_mic;
            system_offset_ms_ = options_.system_audio_offset_ms.value_or(
                options_.test_pattern ? 0.0 : output_latency_seconds() * 1000.0);
            audio_[kSystem].offset = CMTimeMakeWithSeconds(system_offset_ms_ / 1000.0, kMixRate);
            audio_[kMic].offset = CMTimeMakeWithSeconds(options_.mic_offset_ms / 1000.0, kMixRate);
            audio_[kSystem].fader.reset(core::gain_of(options_.system_gain_db, options_.system_muted));
            audio_[kMic].fader.reset(core::gain_of(options_.mic_gain_db, options_.mic_muted));

            if (!options_.test_pattern) {
                if (auto started = start_capture(); !started) return started;
            } else {
                // Exercise the real VideoToolbox writer without TCC, display or compositor.
                size_ = {(options_.width ? options_.width : 1920u) & ~1u,
                         (options_.height ? options_.height : 1080u) & ~1u};
                if (size_.width < 2 || size_.height < 2) return {"invalid synthetic frame dimensions"};
                pattern_[0] = pattern_frame(size_, false);
                pattern_[1] = pattern_frame(size_, true);
                if (!pattern_[0] || !pattern_[1]) { stop(); return {"synthetic frame allocation failed"}; }
                captured_.store(1);
                active_.store(true);
                // Marks land as if display options_.display were captured.
                CGDirectDisplayID displays[16];
                std::uint32_t found = 0;
                if (CGGetActiveDisplayList(16, displays, &found) == kCGErrorSuccess && options_.display >= 1 && options_.display <= found)
                    click_source_.display = options_.display == 1 ? CGMainDisplayID() : displays[options_.display - 1];
            }
            // The compositor always runs, so the screen can move and layers can show while
            // recording. Without anything to draw it passes frames through.
            overlays_ = std::make_unique<mac::Overlays>();
            if (auto started = overlays_->start(size_.width, size_.height, options_, click_source_); !started) {
                overlays_.reset();
                // Frames captured larger than the output cannot be encoded as they are.
                if (layered() || capture_size_.width != size_.width || capture_size_.height != size_.height) {
                    stop();
                    return started;
                }
            }
            if (options_.preview_width) {
                const auto size = core::preview_size(size_.width, size_.height, options_.preview_width);
                // A preview the system cannot scale still records.
                if (VTPixelTransferSessionCreate(kCFAllocatorDefault, &transfer_) == noErr &&
                    CVPixelBufferCreate(kCFAllocatorDefault, size.width, size.height, kCVPixelFormatType_32BGRA,
                                        nullptr, &preview_pixels_) == kCVReturnSuccess)
                    preview_.reset(size);
                else if (options_.preview_only) {
                    stop();
                    return {"could not make the preview"};
                }
                if (const auto& c = options_.canvas2; c.width && preview_pixels_) {
                    const auto second = core::preview_size(c.width, c.height, options_.preview_width);
                    if (CVPixelBufferCreate(kCFAllocatorDefault, second.width, second.height, kCVPixelFormatType_32BGRA, nullptr,
                                            &preview2_pixels_) == kCVReturnSuccess)
                        preview2_.reset(second);
                }
            }
            if (!options_.stream_urls.empty() || options_.replay_seconds) {
                mac::LiveSettings live;
                live.urls = options_.stream_urls;
                live.replay_seconds = options_.replay_seconds;
                live.width = size_.width;
                live.height = size_.height;
                live.fps = options_.fps;
                live.video_kbps = options_.stream_kbps;
                live_ = std::make_unique<mac::LiveStream>();
                if (auto started = live_->start(live); !started) {
                    stop();
                    return {(options_.stream_urls.empty() ? "replay: " : "stream: ") + started.error};
                }
            }
            if (options_.canvas2.width && !options_.preview_only)
                if (auto started = start_second(); !started) { stop(); return started; }
            if (!options_.preview_only) {
                auto ready = create_writer();
                if (!ready) { stop(); return ready; }
                activity_ = [[NSProcessInfo processInfo]
                    beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                    reason:@"Screen recording"];
            }
            finished_.store(false);
            stop_requested_.store(false);
            host_origin_ = CMClockGetTime(CMClockGetHostTimeClock());
            origin_ = nanos(host_origin_);
            timeline_.reset(origin_);
            begin_ = std::chrono::steady_clock::now();
            // Audio has been queueing since capture started. Drain it before drops count:
            // buffers before the origin are skipped, and any an output-latency offset moves
            // past it are kept. Otherwise the full ring evicts pre-roll until the first slot.
            pump_audio(false);
            recording_.store(true);
            pacer_ = std::thread([this] { run(); });
            return {};
        }
    }

    Result stop() {
        @autoreleasepool {
            if (pacer_.joinable()) { stop_requested_.store(true); pacer_.join(); }
            Result result;
            if (stream_) {
                // ScreenCaptureKit refuses to stop a stream that it already stopped itself.
                if (active_ && !stream_stopped_.load()) result = wait_capture(stream_, false);
                // Drain callbacks before clearing their unowned C++ pointer.
                if (queue_) dispatch_sync(queue_, ^{});
                if (audio_queue_) dispatch_sync(audio_queue_, ^{});
                cleanup_capture();
            }
            if (writer_) {
                if (writer_.status == AVAssetWriterStatusWriting) {
                    // Append audio delivered just before capture stopped, then close the mix.
                    pump_audio(true);
                    [input_ markAsFinished];
                    if (mix_input_) [mix_input_ markAsFinished];
                    for (auto& source : audio_)
                        if (source.stem) [source.stem markAsFinished];
                    dispatch_semaphore_t done = dispatch_semaphore_create(0);
                    [writer_ finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
                    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)))
                        result = {"timed out finalizing the recording"};
                }
                if (writer_.status == AVAssetWriterStatusFailed)
                    result = {message(writer_.error, "recording writer failed")};
                adaptor_ = nil;
                mix_input_ = nil;
                for (auto& source : audio_) source.stem = nil;
                input_ = nil;
                writer_ = nil;
            }
            if (live_) live_->stop();
            if (live2_)
                if (auto error = live2_->stop(); !error.empty() && result) result = {"second canvas: " + error};
            if (overlays_) {
                camera_frames_ = overlays_->camera_frames();
                overlays_->stop();
                overlays_.reset();
            }
            if (transfer_) { VTPixelTransferSessionInvalidate(transfer_); CFRelease(transfer_); transfer_ = nullptr; }
            if (preview_pixels_) { CVPixelBufferRelease(preview_pixels_); preview_pixels_ = nullptr; }
            if (preview2_pixels_) { CVPixelBufferRelease(preview2_pixels_); preview2_pixels_ = nullptr; }
            if (activity_) { [[NSProcessInfo processInfo] endActivity:activity_]; activity_ = nil; }
            recording_.store(false);
            {
                std::lock_guard lock(mutex_);
                pending_.clear();
                for (auto& source : audio_) source.queue.clear();
            }
            for (auto& source : audio_)
                if (source.held) { CFRelease(source.held); source.held = nullptr; }
            if (current_.pixel) { CVPixelBufferRelease(current_.pixel); current_ = {}; }
            for (auto& pixel : pattern_)
                if (pixel) { CVPixelBufferRelease(pixel); pixel = nullptr; }
            for (auto* format : {&mix_format_, &tone_format_, &mic_tone_format_})
                if (*format) { CFRelease(*format); *format = nullptr; }
            active_ = false;
            return result;
        }
    }

    void on_frame(CMSampleBufferRef sample) {
        if (!CMSampleBufferIsValid(sample)) return;
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        if (attachments && CFArrayGetCount(attachments) > 0) {
            NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
            NSNumber* status = info[SCStreamFrameInfoStatus];
            if (status && status.integerValue != SCFrameStatusComplete) return;
        }
        CVPixelBufferRef pixel = (CVPixelBufferRef)CMSampleBufferGetImageBuffer(sample);
        if (!pixel) return;
        CVPixelBufferRetain(pixel);
        {
            std::lock_guard lock(mutex_);
            pending_.push({pixel, CMSampleBufferGetPresentationTimeStamp(sample)});
            captured_.fetch_add(1, std::memory_order_relaxed);
        }
        frames_ready_.notify_one();
    }

    void on_audio(CMSampleBufferRef sample, bool microphone) {
        auto& source = audio_[microphone ? kMic : kSystem];
        if (!source.enabled || !CMSampleBufferIsValid(sample) || !CMSampleBufferDataIsReady(sample)) return;
        CFRetain(sample);
        std::lock_guard lock(mutex_);
        // Before the origin, overflow only discards pre-roll that would never be written.
        if (!source.queue.push(sample) && recording_.load(std::memory_order_relaxed))
            source.dropped.fetch_add(1, std::memory_order_relaxed);
    }

    void on_failure(NSError* error) {
        stream_stopped_.store(true);
        // The recorded window closed: end the recording like a reached duration, so it is saved.
        if (options_.window && window_gone(static_cast<CGWindowID>(options_.window))) {
            {
                std::lock_guard lock(mutex_);
                window_closed_.store(true);
            }
            finished_.store(true);
            frames_ready_.notify_one();
            return;
        }
        fail(message(error, "screen capture stopped unexpectedly"));
        frames_ready_.notify_one();
    }

    bool running() const noexcept { return active_ && !finished_.load(); }
    bool finished() const noexcept { return finished_.load(); }
    Stats stats() const noexcept {
        Stats stats;
        stats.captured = captured_.load();
        stats.written = written_.load();
        stats.dropped = dropped_.load();
        stats.audio_written = audio_[kSystem].written.load();
        stats.audio_dropped = audio_[kSystem].dropped.load();
        stats.mic_written = audio_[kMic].written.load();
        stats.mic_dropped = audio_[kMic].dropped.load();
        stats.mix_written = mix_written_.load();
        stats.system_peak = audio_[kSystem].peak.exchange(0);
        stats.mic_peak = audio_[kMic].peak.exchange(0);
        stats.system_audio_offset_ms = audio_[kSystem].enabled ? system_offset_ms_ : 0;
        stats.paused = timeline_.paused();
        stats.paused_seconds = static_cast<double>(timeline_.paused_total(now_nanos())) / kNanos;
        stats.window_closed = window_closed_.load();
        // The second canvas's servers follow the main canvas's.
        for (const auto* stream : {live_.get(), live2_.get()}) {
            if (!stream) continue;
            const auto live = stream->counters();
            stats.streaming = stats.streaming || live.live;
            stats.stream_reconnecting = stats.stream_reconnecting || live.reconnecting;
            stats.stream_reconnects += live.reconnects;
            stats.stream_hardware = stats.stream_hardware || live.hardware;
            stats.stream_frames = std::max(stats.stream_frames, live.frames);
            stats.stream_dropped += live.dropped;
            stats.stream_bytes += live.bytes;
            stats.stream_queued_ms = std::max(stats.stream_queued_ms, live.queued_ms);
            for (std::uint32_t i = 0; i < live.server_count && stats.stream_count < kMaxStreams; ++i)
                stats.streams[stats.stream_count++] = live.servers[i];
            if (stream == live_.get()) stats.replay_seconds = live.replay_seconds;
        }
        stats.canvas2_frames = canvas2_frames_.load();
        stats.canvas2_dropped = canvas2_dropped_.load();
        stats.camera_frames = overlays_ ? overlays_->camera_frames() : camera_frames_;
        return stats;
    }
    std::string failure() const { std::lock_guard lock(mutex_); return failure_; }
    std::string stream_failure() const {
        std::string main = live_ ? live_->failure() : std::string(), second = live2_ ? live2_->failure() : std::string();
        if (!second.empty()) second = "second canvas: " + second;
        return main.empty() ? second : second.empty() ? main : main + "; " + second;
    }
    Result save_replay(const std::string& path, double& seconds) const {
        seconds = 0;
        if (!live_ || !options_.replay_seconds) return {"the replay buffer is off"};
        return live_->save_replay(path, seconds);
    }
    Result set_overlay(std::size_t index, bool visible, const std::optional<Area>& area) {
        if (!overlays_) return {"no overlay " + std::to_string(index + 1)};
        return overlays_->set(index, visible, area);
    }
    Result set_screen(bool visible, const std::optional<Area>& area) {
        if (!overlays_) return {"this recording draws the capture as is; start it with a screen area or an overlay to change it"};
        return overlays_->set_screen(visible, area);
    }
    Result set_layout(std::size_t canvas, std::size_t layer, const Layout& layout) {
        if (!overlays_) return {"this recording draws the capture as is; start it with a screen area or an overlay to change it"};
        return overlays_->set_layout(canvas, layer, layout);
    }
    Result set_volume(AudioInput input, double gain_db, bool muted) {
        if (!(gain_db >= -60 && gain_db <= 24)) return {"volume must be between -60 and 24 dB"};
        auto& source = audio_[input == AudioInput::system ? kSystem : kMic];
        if (!recording_.load() || !source.enabled)
            return {input == AudioInput::system ? "system audio is not being recorded" : "the microphone is not being recorded"};
        source.fader.set(core::gain_of(gain_db, muted));
        return {};
    }

    bool preview(PreviewFrame& frame, std::size_t canvas) const {
        return canvas == kSecondCanvas ? preview2_.take(frame) : preview_.take(frame);
    }
    void show_preview(bool on) {
        preview_.show(on);
        preview2_.show(on);
    }
    Result pause() {
        if (!recording_.load() || finished_.load()) return {"not recording"};
        if (!timeline_.pause(now_nanos())) return {"already paused"};
        return {};
    }
    Result resume() {
        if (!recording_.load() || finished_.load()) return {"not recording"};
        if (!timeline_.resume(now_nanos())) return {"not paused"};
        return {};
    }

private:
    // Frames need composing: overlays, or the main capture moved, cropped or hidden, or a
    // second canvas.
    bool layered() const {
        const auto& a = options_.screen_area;
        const auto& c = options_.screen_crop;
        return !options_.overlays.empty() || options_.clicks || !options_.screen_visible || a.x > 0 || a.y > 0 || a.width < 1 ||
               a.height < 1 || c.x > 0 || c.y > 0 || c.width < 1 || c.height < 1 || options_.canvas2.width;
    }

    // The second canvas's encoder: its servers and its movie.
    Result start_second() {
        const auto& c = options_.canvas2;
        if (c.stream_urls.empty() && c.output.empty()) return {};
        if (options_.stream_kbps < 500 || options_.stream_kbps > 50000) return {"stream bitrate must be between 500 and 50000 kb/s"};
        mac::LiveSettings live;
        live.urls = c.stream_urls;
        live.movie = c.output;
        live.width = c.width;
        live.height = c.height;
        live.fps = options_.fps;
        live.video_kbps = options_.stream_kbps;
        live2_ = std::make_unique<mac::LiveStream>();
        if (auto started = live2_->start(live); !started) return {"second canvas: " + started.error};
        return {};
    }

    // Composes the second canvas from the captured frame and hands it to its encoder. It
    // never holds back the main canvas: without a free frame it skips this one.
    void write_second(CVPixelBufferRef base, std::uint64_t slot) {
        const bool preview = preview2_pixels_ && preview2_.due(slot, options_.fps);
        if (!overlays_ || (!live2_ && !preview)) return;
        CVPixelBufferRef drawn = overlays_->compose(kSecondCanvas, base);
        if (!drawn) {
            canvas2_dropped_.fetch_add(1);
            return;
        }
        if (preview) render_preview(drawn, preview2_pixels_, preview2_);
        if (live2_) {
            live2_->video(drawn, slot);
            if (!audio_[kSystem].enabled && !audio_[kMic].enabled)
                live2_->audio(0, nullptr, static_cast<std::size_t>((slot + 1) * kMixRate / options_.fps));
            canvas2_frames_.fetch_add(1);
        }
        CVPixelBufferRelease(drawn);
    }
    void fail(std::string text) {
        {
            std::lock_guard lock(mutex_);
            if (failure_.empty()) failure_ = std::move(text);
        }
        finished_.store(true);
    }

    Result start_capture() {
        std::string error;
        NSString* microphone_id = nil;
        if (options_.microphone) {
            if (@available(macOS 15.0, *)) {
            } else {
                return {"microphone capture requires macOS 15 or newer"};
            }
            AVCaptureDevice* device = find_microphone(options_.microphone_device, error);
            if (!device) return {error};
            if (auto access = microphone_access(); !access) return access;
            microphone_id = device.uniqueID;
        }
        SCShareableContent* content = shareable(error);
        if (!content) return {error};
        SCContentFilter* filter = nil;
        CGRect source = CGRectNull;
        Size native{};
        if (options_.window) {
            SCWindow* window = nil;
            for (SCWindow* candidate in content.windows)
                if (candidate.windowID == options_.window) { window = candidate; break; }
            if (!window) return {"window not found; run `rec windows`"};
            if (!window.onScreen) return {"the window is not on screen; show it (not minimized, on this Space), then retry"};
            const double scale = pixels_per_point(content, window.frame);
            native = {static_cast<std::uint32_t>(std::lround(window.frame.size.width * scale)),
                      static_cast<std::uint32_t>(std::lround(window.frame.size.height * scale))};
            size_ = output_size(options_, native.width, native.height);
            filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
            click_source_.window = window.windowID;
        } else {
            if (options_.display == 0 || options_.display > content.displays.count)
                return {"display index not found; run `rec devices`"};
            SCDisplay* display = content.displays[options_.display - 1];
            size_ = output_size(options_, display);
            native = {native_size(display).width, native_size(display).height};
            if (options_.region) {
                const auto& r = *options_.region;
                if (r.x < 0 || r.y < 0 || r.width < 2 || r.height < 2 || r.x + r.width > native.width ||
                    r.y + r.height > native.height)
                    return {"region " + std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.width) +
                            "," + std::to_string(r.height) + " does not fit display " + std::to_string(options_.display) +
                            " (" + std::to_string(native.width) + "x" + std::to_string(native.height) + ")"};
                // The source rectangle is in points; the region is in pixels.
                const double scale = display.width > 0 ? static_cast<double>(native.width) / display.width : 1.0;
                source = CGRectMake(r.x / scale, r.y / scale, r.width / scale, r.height / scale);
                size_ = output_size(options_, r.width, r.height);
                native = {r.width, r.height};
            }
            filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
            click_source_ = {display.displayID, 0, options_.region};
        }
        if (size_.width < 2 || size_.height < 2) return {"the capture has no usable pixel dimensions"};
        // With Lanczos, frames arrive at the source's native size and the compositor shrinks them.
        capture_size_ = size_;
        if (options_.scaler == Scaler::lanczos && (native.width > size_.width + 1 || native.height > size_.height + 1))
            capture_size_ = {native.width & ~1u, native.height & ~1u};
        SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
        if (!CGRectIsNull(source)) config.sourceRect = source;
        config.width = capture_size_.width;
        config.height = capture_size_.height;
        config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        config.minimumFrameInterval = CMTimeMake(9, static_cast<int32_t>(options_.fps * 10));
        // Up to three pending frames plus the current one and the encoder's in-flight surfaces;
        // a live stream's queue and encoder hold a few more.
        config.queueDepth = options_.stream_urls.empty() && !options_.replay_seconds && !options_.canvas2.width ? 6 : 8;
        config.showsCursor = options_.cursor;
        config.capturesAudio = options_.system_audio;
        config.sampleRate = kMixRate;
        config.channelCount = 2;
        // rec plays no sound of its own. Excluding this process also made ScreenCaptureKit
        // deliver all-zero system audio for the whole recording whenever another app was
        // already playing when capture started (macOS 27 on an M3 Pro; sounds that began
        // after the start were captured).
        config.excludesCurrentProcessAudio = NO;
        config.scalesToFit = YES;
        config.preservesAspectRatio = YES;
        if (options_.microphone) {
            if (@available(macOS 15.0, *)) {
                config.captureMicrophone = YES;
                config.microphoneCaptureDeviceID = microphone_id;
            }
        }
        receiver_ = [[RecCaptureOutput alloc] init];
        receiver_.owner = this;
        queue_ = dispatch_queue_create("rec.capture.frames", DISPATCH_QUEUE_SERIAL);
        if (options_.system_audio || options_.microphone)
            audio_queue_ = dispatch_queue_create("rec.capture.audio", DISPATCH_QUEUE_SERIAL);
        stream_ = [[SCStream alloc] initWithFilter:filter configuration:config delegate:receiver_];
        NSError* stream_error = nil;
        if (![stream_ addStreamOutput:receiver_ type:SCStreamOutputTypeScreen
                  sampleHandlerQueue:queue_ error:&stream_error]) {
            cleanup_capture();
            return {message(stream_error, "could not subscribe to screen frames")};
        }
        if (options_.system_audio &&
            ![stream_ addStreamOutput:receiver_ type:SCStreamOutputTypeAudio
                  sampleHandlerQueue:audio_queue_ error:&stream_error]) {
            cleanup_capture();
            return {message(stream_error, "could not subscribe to system audio")};
        }
        if (options_.microphone) {
            if (@available(macOS 15.0, *)) {
                if (![stream_ addStreamOutput:receiver_ type:SCStreamOutputTypeMicrophone
                          sampleHandlerQueue:audio_queue_ error:&stream_error]) {
                    cleanup_capture();
                    return {message(stream_error, "could not subscribe to the microphone")};
                }
            }
        }
        stream_stopped_.store(false);
        auto started = wait_capture(stream_, true);
        if (!started) { cleanup_capture(); return started; }
        active_ = true;
        {
            std::unique_lock lock(mutex_);
            if (!frames_ready_.wait_for(lock, std::chrono::seconds(8), [this] {
                return !pending_.empty() || !failure_.empty();
            })) {
                error = "no screen frames arrived within 8 seconds (check Screen Recording permission)";
            } else if (!failure_.empty()) {
                error = failure_;
            }
        }
        if (!error.empty()) { stop(); return {error}; }
        return {};
    }

    AVAssetWriterInput* add_audio_input(std::uint32_t channels, std::uint32_t bitrate) {
        NSDictionary* settings = @{
            AVFormatIDKey: @(kAudioFormatMPEG4AAC),
            AVSampleRateKey: @(kMixRate),
            AVNumberOfChannelsKey: @(channels),
            AVEncoderBitRateKey: @(bitrate)
        };
        AVAssetWriterInput* input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                                       outputSettings:settings];
        input.expectsMediaDataInRealTime = YES;
        if (![writer_ canAddInput:input]) return nil;
        [writer_ addInput:input];
        return input;
    }

    Result create_writer() {
        NSString* path = [NSString stringWithUTF8String:options_.output.c_str()];
        NSURL* url = [NSURL fileURLWithPath:path];
        AVFileType type = [path.pathExtension.lowercaseString isEqualToString:@"mp4"]
            ? AVFileTypeMPEG4 : AVFileTypeQuickTimeMovie;
        NSError* error = nil;
        writer_ = [[AVAssetWriter alloc] initWithURL:url fileType:type error:&error];
        if (!writer_) return {message(error, "could not create output movie")};
        bool hevc = options_.codec == Codec::hevc ||
            (options_.codec == Codec::automatic && size_.height > 1080);
        const std::uint64_t bitrate = static_cast<std::uint64_t>(options_.bitrate_mbps ?
            options_.bitrate_mbps : (size_.height > 1440 ? 50 : size_.height > 1080 ? 30 : 20)) * 1000000;
        // ProRes is intra-only at a rate set by its profile, so it takes no rate or keyframe keys.
        NSDictionary* settings = options_.codec == Codec::prores ? @{
            AVVideoCodecKey: AVVideoCodecTypeAppleProRes422,
            AVVideoWidthKey: @(size_.width), AVVideoHeightKey: @(size_.height)
        } : @{
            AVVideoCodecKey: hevc ? AVVideoCodecTypeHEVC : AVVideoCodecTypeH264,
            AVVideoWidthKey: @(size_.width), AVVideoHeightKey: @(size_.height),
            AVVideoCompressionPropertiesKey: @{
                AVVideoAverageBitRateKey: @(bitrate),
                AVVideoExpectedSourceFrameRateKey: @(options_.fps),
                AVVideoMaxKeyFrameIntervalKey: @(options_.fps * 2),
                AVVideoAllowFrameReorderingKey: @NO
            }
        };
        input_ = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:settings];
        input_.expectsMediaDataInRealTime = YES;
        if (![writer_ canAddInput:input_]) return {"video encoder cannot accept the selected settings"};
        [writer_ addInput:input_];

        // Track order is the order inputs are added: the mix first, so it is the default track.
        // A live stream always carries the mix, even when the file keeps only stems.
        const bool system = audio_[kSystem].enabled, mic = audio_[kMic].enabled;
        const bool file_mix = system && mic && options_.audio_tracks != AudioTracks::stems;
        mixing_ = file_mix || (system && mic && (live_ || live2_));
        const bool stems = !file_mix || options_.audio_tracks == AudioTracks::both;
        NSMutableArray<AVAssetWriterInput*>* audio_inputs = [NSMutableArray array];
        if (mixing_) mixer_.reset(static_cast<std::size_t>(kMixRate) * 2);
        if (file_mix) {
            mix_input_ = add_audio_input(2, 256000);
            if (!mix_input_) return {"AAC writer cannot accept the audio mix"};
            [audio_inputs addObject:mix_input_];
            mix_format_ = pcm_format(2, true);
            if (!mix_format_) return {"could not describe the audio mix format"};
        }
        if (system && stems) {
            audio_[kSystem].stem = add_audio_input(2, 256000);
            if (!audio_[kSystem].stem) return {"AAC writer cannot accept system audio"};
            [audio_inputs addObject:audio_[kSystem].stem];
        }
        if (mic && stems) {
            audio_[kMic].stem = add_audio_input(1, 128000);
            if (!audio_[kMic].stem) return {"AAC writer cannot accept the microphone"};
            [audio_inputs addObject:audio_[kMic].stem];
        }
        if (audio_inputs.count > 1) {
            // One alternate group: players use the default (mix), editors see every track.
            AVAssetWriterInputGroup* group = [AVAssetWriterInputGroup
                assetWriterInputGroupWithInputs:audio_inputs defaultInput:audio_inputs.firstObject];
            if ([writer_ canAddInputGroup:group]) [writer_ addInputGroup:group];
        }
        if (options_.test_tone) tone_format_ = pcm_format(2, false);
        if (options_.test_mic) mic_tone_format_ = pcm_format(1, false);

        // No adaptor pool: SCK's IOSurface-backed pixel buffer goes to VideoToolbox unchanged.
        adaptor_ = [AVAssetWriterInputPixelBufferAdaptor
            assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input_ sourcePixelBufferAttributes:nil];
        // Write a movie fragment every second: a crash or power loss leaves every completed
        // fragment playable. finishWriting consolidates them into a regular movie.
        writer_.movieFragmentInterval = CMTimeMake(1, 1);
        if (![writer_ startWriting]) return {message(writer_.error, "video writer could not start")};
        [writer_ startSessionAtSourceTime:kCMTimeZero];
        return {};
    }

    // The frame on screen at the slot's instant: the newest delivered frame whose
    // timestamp is not after it. Runs on the recording thread, which owns current_.
    CVPixelBufferRef select_frame(std::uint64_t slot, std::int64_t due_nanos) {
        if (options_.test_pattern) return pattern_[core::test_flash(slot, options_.fps) ? 1 : 0];
        const CMTime due = CMTimeMake(due_nanos, kNanos);
        std::lock_guard lock(mutex_);
        while (!pending_.empty() &&
               (!current_.pixel || CMTimeCompare(pending_.front().pts, due) <= 0)) {
            if (current_.pixel) CVPixelBufferRelease(current_.pixel);
            current_ = pending_.pop();
        }
        return current_.pixel;
    }

    bool append_retimed(AVAssetWriterInput* input, CMSampleBufferRef sample, CMTime shift) {
        CMItemCount entries = 0;
        auto status = CMSampleBufferGetSampleTimingInfoArray(sample, 0, nullptr, &entries);
        std::array<CMSampleTimingInfo, 8> timing{};
        if (status != noErr || entries < 1 || entries > static_cast<CMItemCount>(timing.size())) return false;
        status = CMSampleBufferGetSampleTimingInfoArray(sample, entries, timing.data(), &entries);
        if (status != noErr) return false;
        for (CMItemCount i = 0; i < entries; ++i) {
            if (CMTIME_IS_VALID(timing[i].presentationTimeStamp))
                timing[i].presentationTimeStamp = CMTimeAdd(timing[i].presentationTimeStamp, shift);
            if (CMTIME_IS_VALID(timing[i].decodeTimeStamp))
                timing[i].decodeTimeStamp = CMTimeAdd(timing[i].decodeTimeStamp, shift);
        }
        CMSampleBufferRef adjusted = nullptr;
        status = CMSampleBufferCreateCopyWithNewTiming(kCFAllocatorDefault, sample, entries, timing.data(), &adjusted);
        if (status != noErr || !adjusted) return false;
        const bool appended = [input appendSampleBuffer:adjusted];
        CFRelease(adjusted);
        return appended;
    }

    // Routes one buffer to its stem and the mix. Returns false, without side effects,
    // when the stem input cannot accept data yet.
    // Synthetic buffers are already on the recording timeline; captured ones pass through
    // the pause timeline, which drops audio heard while paused.
    bool process_audio(Source kind, CMSampleBufferRef sample, bool synthetic = false) {
        auto& source = audio_[kind];
        if (source.stem && !source.stem.readyForMoreMediaData) return false;
        const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
        if (!CMTIME_IS_NUMERIC(pts)) {
            source.dropped.fetch_add(1);
            return true;
        }
        // Host clock to recording time, plus the source's sync offset, minus paused time.
        CMTime shift = CMTimeSubtract(source.offset, host_origin_);
        CMTime start = CMTimeAdd(pts, shift);
        if (!synthetic) {
            const auto point = timeline_.to_output(nanos(CMTimeAdd(pts, source.offset)));
            if (point.inside) return true;
            start = CMTimeMake(point.output, kNanos);
            shift = CMTimeSubtract(start, pts);
        }
        // With one source its track is the program and takes the volume; with two, the stems
        // stay as captured.
        const auto ramp = source.fader.next();
        const bool alone = !(audio_[kSystem].enabled && audio_[kMic].enabled);
        bool used = false;
        if (source.stem && CMTimeCompare(start, kCMTimeZero) >= 0) {
            CMSampleBufferRef scaled = alone && !ramp.unity() ? scaled_pcm(sample, ramp, scale_scratch_) : nullptr;
            const bool appended = append_retimed(source.stem, scaled ? scaled : sample, shift);
            if (scaled) CFRelease(scaled);
            if (appended) {
                used = true;
            } else {
                source.dropped.fetch_add(1);
                if (writer_.status == AVAssetWriterStatusFailed)
                    fail(message(writer_.error, "audio encoding failed"));
            }
        }
        const bool pcm = stereo_pcm(sample, pcm_, scratch_);
        if (pcm) {
            // Meters show the source before its volume, so a muted microphone still shows it works.
            core::raise_peak(source.peak, pcm_.data(), pcm_.size());
            if (!ramp.unity()) ramp.apply(pcm_.data(), pcm_.size() / 2);
        }
        // A single source goes to the live stream as it is; two go through the mix.
        if (mixing_ || live_ || live2_) {
            if (pcm) {
                auto first = static_cast<std::int64_t>(std::llround(CMTimeGetSeconds(start) * kMixRate));
                // Timestamps jitter by a sample or two; keep a continuous stream continuous.
                if (source.next_mix != INT64_MIN && std::llabs(first - source.next_mix) <= kMixRate / 1000)
                    first = source.next_mix;
                const auto frames = pcm_.size() / 2;
                source.next_mix = first + static_cast<std::int64_t>(frames);
                if (mixing_) {
                    mixer_.add(kind, first, pcm_.data(), frames);
                } else {
                    if (live_) live_->audio(first, pcm_.data(), frames);
                    if (live2_) live2_->audio(first, pcm_.data(), frames);
                }
                used = true;
            } else if (!source.stem) {
                source.dropped.fetch_add(1);
            }
        }
        if (used) source.written.fetch_add(1);
        return true;
    }

    void emit_mix(bool flush) {
        if (!mixing_) return;
        const auto system_end = mixer_.end(kSystem), mic_end = mixer_.end(kMic);
        std::int64_t target = flush ? std::max(system_end, mic_end) : std::min(system_end, mic_end);
        if (!flush && !options_.test_pattern) {
            const auto now = timeline_.to_output(now_nanos()).output;
            target = std::max(target, now * kMixRate / kNanos - kMixWaitFrames);
        }
        target = std::min(target, mixer_.emitted() + mixer_.capacity());
        while (mixer_.emitted() < target) {
            if (mix_input_ && !mix_input_.readyForMoreMediaData) {
                if (!flush) return;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                while (!mix_input_.readyForMoreMediaData && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                if (!mix_input_.readyForMoreMediaData) return;
            }
            const auto first = mixer_.emitted();
            const auto frames = static_cast<std::size_t>(std::min<std::int64_t>(target - first, 4096));
            mixer_.take(frames, mixed_);
            if (live_) live_->audio(first, mixed_.data(), frames);
            if (live2_) live2_->audio(first, mixed_.data(), frames);
            if (!mix_input_) continue;
            CMSampleBufferRef sample = pcm_buffer(mix_format_, mixed_.data(), mixed_.size() * sizeof(float),
                                                  frames, CMTimeMake(first, kMixRate));
            if (!sample) { fail("could not create an audio mix buffer"); return; }
            if ([mix_input_ appendSampleBuffer:sample]) {
                mix_written_.fetch_add(1);
            } else if (writer_.status == AVAssetWriterStatusFailed) {
                fail(message(writer_.error, "audio mix encoding failed"));
            }
            CFRelease(sample);
        }
    }

    // Drains captured audio on the recording thread. Never encode or block in a callback.
    void pump_audio(bool flush) {
        for (auto kind : {kSystem, kMic}) {
            auto& source = audio_[kind];
            if (!source.enabled) continue;
            for (int packets = 0; packets < 64; ++packets) {
                if (!source.held) {
                    std::lock_guard lock(mutex_);
                    source.held = source.queue.pop();
                }
                if (!source.held) break;
                bool done = process_audio(kind, source.held);
                if (!done && flush) {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                    while (!done && std::chrono::steady_clock::now() < deadline) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        done = process_audio(kind, source.held);
                    }
                    if (!done) source.dropped.fetch_add(1);
                } else if (!done) {
                    break;
                }
                CFRelease(source.held);
                source.held = nullptr;
            }
        }
        emit_mix(flush);
    }

    // Takes ownership of sample. Synthetic audio must not be lost, so wait briefly for the input.
    bool submit_synthetic(Source kind, CMSampleBufferRef sample) {
        if (!sample) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        bool done = process_audio(kind, sample, true);
        while (!done && !stop_requested_.load() && std::chrono::steady_clock::now() < deadline) {
            emit_mix(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            done = process_audio(kind, sample, true);
        }
        CFRelease(sample);
        return done;
    }

    // Synthetic sources follow the written video, so CI gets a deterministic A/V timeline:
    // a 100 ms 440 Hz beep starting with every one-second flash, and a quiet 1 kHz "mic".
    bool write_synthetic_audio(std::uint64_t until) {
        std::array<std::int16_t, 2048> tone{};
        std::array<std::int16_t, 1024> mic{};
        while (synthetic_samples_ < until && !stop_requested_.load() && !finished_.load()) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(1024, until - synthetic_samples_));
            const CMTime pts = CMTimeAdd(host_origin_, CMTimeMake(static_cast<int64_t>(synthetic_samples_), kMixRate));
            if (options_.test_tone) {
                for (std::size_t i = 0; i < count; ++i)
                    tone[i * 2] = tone[i * 2 + 1] =
                        static_cast<std::int16_t>(core::test_tone(synthetic_samples_ + i) * 32768.0f);
                if (!submit_synthetic(kSystem, pcm_buffer(tone_format_, tone.data(), count * 4, count, pts)))
                    return false;
            }
            if (options_.test_mic) {
                for (std::size_t i = 0; i < count; ++i)
                    mic[i] = static_cast<std::int16_t>(core::test_mic(synthetic_samples_ + i) * 32768.0f);
                if (!submit_synthetic(kMic, pcm_buffer(mic_tone_format_, mic.data(), count * 2, count, pts)))
                    return false;
            }
            synthetic_samples_ += count;
        }
        return true;
    }

    void run() {
        using namespace std::chrono;
        const auto delay = options_.test_pattern ? nanoseconds(0) : duration_cast<nanoseconds>(kEmitDelay);
        const auto limit = options_.duration_seconds > 0
            ? static_cast<std::uint64_t>(std::llround(options_.duration_seconds * options_.fps)) : UINT64_MAX;
        const CMTimeScale scale = static_cast<CMTimeScale>(options_.fps);
        for (std::uint64_t slot = 0; slot < limit && !stop_requested_.load() && !finished_; ++slot) {
            const auto output = static_cast<std::int64_t>(slot) * kNanos / options_.fps;
            std::int64_t due = 0;
            steady_clock::time_point tick;
            // Wait for the slot's host instant. A pause can begin while waiting; then the
            // slot waits for resume() and lands after it, so the movie has no gap.
            for (;;) {
                if (stop_requested_.load() || finished_.load()) break;
                const auto host = timeline_.to_host(output);
                if (!host) {
                    @autoreleasepool { pump_audio(false); }
                    std::this_thread::sleep_for(milliseconds(5));
                    continue;
                }
                due = *host;
                tick = begin_ + nanoseconds(due - origin_) + delay;
                const auto now = steady_clock::now();
                if (now >= tick) break;
                std::this_thread::sleep_until(std::min(tick, now + milliseconds(10)));
            }
            if (stop_requested_.load() || finished_) break;
            @autoreleasepool {
                CVPixelBufferRef frame = select_frame(slot, due);
                if (options_.preview_only) {
                    if (frame && preview_.due(slot, options_.fps)) {
                        CVPixelBufferRef drawn = overlays_ ? overlays_->compose(kMainCanvas, frame) : nullptr;
                        if (overlays_ && !drawn) fail("could not draw the overlays");
                        else render_preview(drawn ? drawn : frame, preview_pixels_, preview_);
                        if (drawn) CVPixelBufferRelease(drawn);
                    }
                    if (frame) write_second(frame, slot);
                    continue;
                }
                CVPixelBufferRef composed = nullptr;
                CVPixelBufferRef captured = frame;
                if (frame && overlays_) {
                    composed = overlays_->compose(kMainCanvas, frame);
                    if (!composed) fail("could not draw the overlays");
                    frame = composed;
                }
                if (frame && preview_.due(slot, options_.fps)) render_preview(frame, preview_pixels_, preview_);
                // Retry the same timestamp for up to 500 ms on transient backpressure.
                // Never hold a stream callback or create an unbounded frame queue.
                bool appended = false;
                const auto deadline = tick + milliseconds(500);
                while (frame && !stop_requested_.load() && !finished_.load() &&
                       steady_clock::now() < deadline && writer_.status != AVAssetWriterStatusFailed) {
                    if (input_.readyForMoreMediaData &&
                        [adaptor_ appendPixelBuffer:frame
                               withPresentationTime:CMTimeMake(static_cast<int64_t>(slot), scale)]) {
                        appended = true;
                        break;
                    }
                    // AVAssetWriter interleaves inputs: keep audio moving so video can become ready.
                    pump_audio(false);
                    std::this_thread::sleep_for(milliseconds(2));
                }
                if (appended) {
                    written_.fetch_add(1);
                    if (live_) {
                        live_->video(frame, slot);
                        // A stream without audio sources still carries a silent AAC track; services expect one.
                        if (!audio_[kSystem].enabled && !audio_[kMic].enabled)
                            live_->audio(0, nullptr, static_cast<std::size_t>((slot + 1) * kMixRate / options_.fps));
                    }
                    write_second(captured, slot);
                    if ((options_.test_tone || options_.test_mic) &&
                        !write_synthetic_audio(((slot + 1) * static_cast<std::uint64_t>(kMixRate)) / options_.fps) &&
                        !stop_requested_.load())
                        fail(message(writer_.error, "synthetic audio append failed"));
                } else if (!stop_requested_.load()) {
                    dropped_.fetch_add(1);
                }
                if (composed) CVPixelBufferRelease(composed);
                if (writer_.status == AVAssetWriterStatusFailed)
                    fail(message(writer_.error, "video encoding failed"));
                pump_audio(false);
            }
        }
        finished_.store(true);
    }

    // Scales and converts a canvas's frame as encoded into BGRA for its preview.
    void render_preview(CVPixelBufferRef frame, CVPixelBufferRef pixels, core::PreviewBuffer& preview) {
        if (VTPixelTransferSessionTransferImage(transfer_, frame, pixels) != noErr) return;
        if (CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
        preview.publish(static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(pixels)), CVPixelBufferGetBytesPerRow(pixels));
        CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
    }

    void cleanup_capture() {
        if (receiver_) receiver_.owner = nullptr;
        receiver_ = nil;
        stream_ = nil;
        queue_ = nil;
        audio_queue_ = nil;
    }

    Options options_;
    Size size_{};
    Size capture_size_{};  // What ScreenCaptureKit delivers: size_, or the native size to scale from.
    mutable std::mutex mutex_;
    std::condition_variable frames_ready_;
    FrameRing pending_;                 // Guarded by mutex_.
    Frame current_;                     // Recording thread only.
    std::array<CVPixelBufferRef, 2> pattern_{};
    std::string failure_;
    SCStream* __strong stream_ = nil;
    RecCaptureOutput* __strong receiver_ = nil;
    dispatch_queue_t queue_ = nullptr;
    dispatch_queue_t audio_queue_ = nullptr;
    std::array<AudioSource, 2> audio_;
    double system_offset_ms_ = 0;
    bool mixing_ = false;
    core::Mixer mixer_;
    AVAssetWriterInput* __strong mix_input_ = nil;
    std::vector<float> pcm_, mixed_;
    std::vector<std::byte> scratch_, scale_scratch_;
    CMAudioFormatDescriptionRef mix_format_ = nullptr;
    CMAudioFormatDescriptionRef tone_format_ = nullptr;
    CMAudioFormatDescriptionRef mic_tone_format_ = nullptr;
    std::uint64_t synthetic_samples_ = 0;
    CMTime host_origin_ = kCMTimeInvalid;
    std::int64_t origin_ = 0;           // host_origin_ in nanoseconds.
    core::PauseTimeline timeline_;
    std::chrono::steady_clock::time_point begin_{};
    AVAssetWriter* __strong writer_ = nil;
    AVAssetWriterInput* __strong input_ = nil;
    AVAssetWriterInputPixelBufferAdaptor* __strong adaptor_ = nil;
    id __strong activity_ = nil;
    std::thread pacer_;
    std::atomic_bool stop_requested_{false};
    std::atomic_bool active_{false};
    std::atomic_bool recording_{false};
    std::atomic_bool finished_{false};
    std::atomic_bool stream_stopped_{false};  // ScreenCaptureKit ended the stream itself.
    std::atomic_bool window_closed_{false};
    std::atomic<std::uint64_t> captured_{0}, written_{0}, dropped_{0}, mix_written_{0};
    std::unique_ptr<mac::LiveStream> live_;  // Kept after stop() for its counters.
    std::unique_ptr<mac::Overlays> overlays_;
    mac::ClickSource click_source_;
    std::uint64_t camera_frames_ = 0;  // The count when the overlays were released.
    VTPixelTransferSessionRef transfer_ = nullptr;  // Preview scaling, on the recording thread.
    CVPixelBufferRef preview_pixels_ = nullptr;
    core::PreviewBuffer preview_;
    // The second canvas.
    std::unique_ptr<mac::LiveStream> live2_;  // With servers or a movie; kept after stop() for its counters.
    CVPixelBufferRef preview2_pixels_ = nullptr;
    core::PreviewBuffer preview2_;
    std::atomic<std::uint64_t> canvas2_frames_{0}, canvas2_dropped_{0};
};

namespace {
double brightness(CVPixelBufferRef pixel) {
    if (!pixel || CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return 0;
    const auto* base = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(pixel));
    const auto stride = CVPixelBufferGetBytesPerRow(pixel);
    const auto width = CVPixelBufferGetWidth(pixel), height = CVPixelBufferGetHeight(pixel);
    std::uint64_t sum = 0, count = 0;
    if (base) {
        const auto step_x = std::max<std::size_t>(1, width / 64), step_y = std::max<std::size_t>(1, height / 36);
        for (std::size_t y = step_y / 2; y < height; y += step_y) {
            for (std::size_t x = step_x / 2; x < width; x += step_x) {
                const auto* bgra = base + y * stride + x * 4;
                sum += bgra[0] + bgra[1] + bgra[2];
                count += 3;
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
    return count ? static_cast<double>(sum) / (255.0 * static_cast<double>(count)) : 0;
}

Probe::Color mean_color(CVPixelBufferRef pixel, const Region& r) {
    Probe::Color color;
    if (!pixel || CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return color;
    const auto* base = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(pixel));
    const auto stride = CVPixelBufferGetBytesPerRow(pixel);
    const auto width = CVPixelBufferGetWidth(pixel), height = CVPixelBufferGetHeight(pixel);
    double sum[3]{}, luma_sum = 0, luma_squares = 0;
    std::uint64_t count = 0;
    for (std::size_t y = static_cast<std::size_t>(r.y); base && y < height && y < r.y + r.height; ++y)
        for (std::size_t x = static_cast<std::size_t>(r.x); x < width && x < r.x + r.width; ++x, ++count) {
            const std::uint8_t* p = base + y * stride + x * 4;
            for (int c = 0; c < 3; ++c) sum[c] += p[c];
            const double luma = 0.0722 * p[0] + 0.7152 * p[1] + 0.2126 * p[2];
            luma_sum += luma;
            luma_squares += luma * luma;
        }
    CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
    if (count) {
        const double mean = luma_sum / count;
        color.spread = std::sqrt(std::max(0.0, luma_squares / count - mean * mean));
        color.b = static_cast<std::uint8_t>(std::lround(sum[0] / count));
        color.g = static_cast<std::uint8_t>(std::lround(sum[1] / count));
        color.r = static_cast<std::uint8_t>(std::lround(sum[2] / count));
    }
    return color;
}
} // namespace

Result probe_file(const std::string& path, Probe& probe, std::optional<Region> sample_rect) {
    @autoreleasepool {
        probe = {};
        NSString* name = [NSString stringWithUTF8String:path.c_str()];
        if (!name) return {"invalid recording path"};
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:name] options:nil];
        AVAssetTrack* track = [[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
        if (!track) return {"no video track"};
        const CGSize size = track.naturalSize;
        probe.width = static_cast<std::uint32_t>(std::abs(size.width));
        probe.height = static_cast<std::uint32_t>(std::abs(size.height));
        CMFormatDescriptionRef format = (__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
        const auto codec = format ? CMFormatDescriptionGetMediaSubType(format) : 0;
        probe.codec = codec == kCMVideoCodecType_H264 ? "h264" :
                      codec == kCMVideoCodecType_HEVC ? "hevc" :
                      codec == kCMVideoCodecType_AppleProRes422 ? "prores" : "other";
        if (sample_rect) {
            const auto& r = *sample_rect;
            if (r.x < 0 || r.y < 0 || r.width < 2 || r.height < 2 || r.x + r.width > probe.width || r.y + r.height > probe.height)
                return {"the sample rectangle does not fit the " + std::to_string(probe.width) + "x" + std::to_string(probe.height) + " video"};
        }
        double next_sample = 0;
        NSError* error = nil;
        AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
        if (!reader) return {message(error, "could not open movie")};
        AVAssetReaderTrackOutput* output = [[AVAssetReaderTrackOutput alloc] initWithTrack:track
            outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)}];
        if (![reader canAddOutput:output]) return {"could not decode video"};
        [reader addOutput:output];
        if (![reader startReading]) return {message(reader.error, "video decode start failed")};
        CMTime previous = kCMTimeInvalid;
        core::FlashDetector flashes;
        while (CMSampleBufferRef sample = [output copyNextSampleBuffer]) {
            const CMTime timestamp = CMSampleBufferGetPresentationTimeStamp(sample);
            if (CMTIME_IS_VALID(previous) && CMTimeCompare(timestamp, previous) <= 0) {
                CFRelease(sample);
                [reader cancelReading];
                return {"video timestamps are not strictly increasing"};
            }
            previous = timestamp;
            ++probe.frames;
            flashes.feed(CMTimeGetSeconds(timestamp), brightness(CMSampleBufferGetImageBuffer(sample)));
            if (sample_rect && CMTimeGetSeconds(timestamp) >= next_sample) {
                probe.samples.push_back(mean_color(CMSampleBufferGetImageBuffer(sample), *sample_rect));
                next_sample = std::floor(CMTimeGetSeconds(timestamp)) + 1;
            }
            CFRelease(sample);
        }
        if (reader.status != AVAssetReaderStatusCompleted)
            return {message(reader.error, "video decode failed")};
        if (!probe.frames) return {"no decoded frames"};
        probe.duration_seconds = CMTimeGetSeconds(track.timeRange.duration);

        NSArray<AVAssetTrack*>* audio_tracks = [asset tracksWithMediaType:AVMediaTypeAudio];
        probe.audio_tracks = static_cast<std::uint32_t>(audio_tracks.count);
        AVAssetTrack* audio = audio_tracks.firstObject;
        core::OnsetDetector beeps;
        if (audio) {
            CMFormatDescriptionRef audio_format =
                (__bridge CMFormatDescriptionRef)audio.formatDescriptions.firstObject;
            const auto subtype = audio_format ? CMFormatDescriptionGetMediaSubType(audio_format) : 0;
            probe.audio_codec = subtype == kAudioFormatMPEG4AAC ? "aac" : "other";
            probe.audio_duration_seconds = CMTimeGetSeconds(audio.timeRange.duration);
            NSError* audio_error = nil;
            AVAssetReader* audio_reader = [[AVAssetReader alloc] initWithAsset:asset error:&audio_error];
            if (!audio_reader) return {message(audio_error, "could not open audio track")};
            AVAssetReaderTrackOutput* audio_output = [[AVAssetReaderTrackOutput alloc]
                initWithTrack:audio outputSettings:@{
                    AVFormatIDKey: @(kAudioFormatLinearPCM),
                    AVLinearPCMBitDepthKey: @32,
                    AVLinearPCMIsFloatKey: @YES,
                    AVLinearPCMIsNonInterleaved: @NO,
                    AVLinearPCMIsBigEndianKey: @NO
                }];
            if (![audio_reader canAddOutput:audio_output]) return {"could not decode audio"};
            [audio_reader addOutput:audio_output];
            if (![audio_reader startReading]) return {message(audio_reader.error, "audio decode start failed")};
            std::vector<float> samples;
            while (CMSampleBufferRef buffer = [audio_output copyNextSampleBuffer]) {
                ++probe.audio_buffers;
                const auto* description =
                    CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(buffer));
                CMBlockBufferRef block = CMSampleBufferGetDataBuffer(buffer);
                if (description && block && description->mChannelsPerFrame && description->mSampleRate > 0 &&
                    (description->mFormatFlags & kAudioFormatFlagIsFloat) && description->mBitsPerChannel == 32) {
                    samples.resize(CMBlockBufferGetDataLength(block) / sizeof(float));
                    if (CMBlockBufferCopyDataBytes(block, 0, samples.size() * sizeof(float), samples.data()) ==
                        kCMBlockBufferNoErr) {
                        const double start = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(buffer));
                        const auto channels = description->mChannelsPerFrame;
                        for (std::size_t frame = 0; frame < samples.size() / channels; ++frame) {
                            float peak = 0;
                            for (UInt32 c = 0; c < channels; ++c)
                                peak = std::max(peak, std::fabs(samples[frame * channels + c]));
                            beeps.feed(start + static_cast<double>(frame) / description->mSampleRate, peak);
                        }
                    }
                }
                CFRelease(buffer);
            }
            if (audio_reader.status != AVAssetReaderStatusCompleted)
                return {message(audio_reader.error, "audio decode failed")};
            if (!probe.audio_buffers) return {"audio track has no decoded samples"};
        }

        core::measure_sync(flashes.onsets, beeps.onsets, probe);
        return {};
    }
}

void receive_frame(void* owner, CMSampleBufferRef sample) {
    static_cast<CaptureState*>(owner)->on_frame(sample);
}
void receive_audio(void* owner, CMSampleBufferRef sample, bool microphone) {
    static_cast<CaptureState*>(owner)->on_audio(sample, microphone);
}
void receive_failure(void* owner, NSError* error) {
    static_cast<CaptureState*>(owner)->on_failure(error);
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
Result Recorder::set_screen(bool visible, std::optional<Area> area) { return impl_->capture.set_screen(visible, area); }
Result Recorder::set_volume(AudioInput input, double gain_db, bool muted) {
    return impl_->capture.set_volume(input, gain_db, muted);
}
Result Recorder::set_layout(std::size_t canvas, std::size_t layer, const Layout& layout) {
    return impl_->capture.set_layout(canvas, layer, layout);
}
bool Recorder::preview(PreviewFrame& frame) const { return impl_->capture.preview(frame, kMainCanvas); }
bool Recorder::preview(PreviewFrame& frame, std::size_t canvas) const { return impl_->capture.preview(frame, canvas); }
void Recorder::show_preview(bool on) noexcept { impl_->capture.show_preview(on); }

} // namespace rec
