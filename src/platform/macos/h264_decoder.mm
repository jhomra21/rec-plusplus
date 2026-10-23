#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include "platform/macos/h264_decoder.hpp"

#include <algorithm>
#include <vector>

namespace rec::mac {
namespace {
// Calls f(nal, size) for each NAL unit between Annex B start codes.
template <class F> void each_nal(const std::uint8_t* p, std::size_t n, F&& f) {
    std::size_t i = 0, start = SIZE_MAX;
    const auto emit = [&](std::size_t end) {
        // A four-byte start code leaves its first zero at the end of the unit before it.
        while (end > start && p[end - 1] == 0) --end;
        if (end > start) f(p + start, end - start);
    };
    while (i + 3 <= n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if (start != SIZE_MAX) emit(i);
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != SIZE_MAX) emit(n);
}
} // namespace

class H264Decoder::Impl {
public:
    explicit Impl(Picture picture) : picture_(std::move(picture)) {}
    ~Impl() { reset(); }

    bool decode(const std::uint8_t* annexb, std::size_t size) {
        sample_.clear();
        bool changed = false;
        each_nal(annexb, size, [&](const std::uint8_t* nal, std::size_t length) {
            const int type = nal[0] & 0x1f;
            if (type == 7 || type == 8) {
                auto& set = type == 7 ? sps_ : pps_;
                if (set.size() != length || !std::equal(set.begin(), set.end(), nal)) {
                    set.assign(nal, nal + length);
                    changed = true;
                }
                return;
            }
            if (type < 1 || type > 5) return;  // Only slices go to the decoder; SEI and delimiters do not.
            const auto n = static_cast<std::uint32_t>(length);
            const std::uint8_t prefix[4] = {std::uint8_t(n >> 24), std::uint8_t(n >> 16), std::uint8_t(n >> 8), std::uint8_t(n)};
            sample_.insert(sample_.end(), prefix, prefix + 4);
            sample_.insert(sample_.end(), nal, nal + length);
        });
        if (changed) reset();
        if (sample_.empty()) return true;
        if (!session_ && !make()) return false;

        CMBlockBufferRef block = nullptr;
        CMSampleBufferRef sample = nullptr;
        const std::size_t bytes = sample_.size();
        OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, sample_.data(), bytes, kCFAllocatorNull,
                                                             nullptr, 0, bytes, 0, &block);
        if (status == noErr)
            status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, format_, 1, 0, nullptr, 1, &bytes, &sample);
        failed_ = false;
        if (status == noErr) {
            status = VTDecompressionSessionDecodeFrame(session_, sample, 0, nullptr, nullptr);
            // The sample points into sample_, which the next unit reuses.
            VTDecompressionSessionWaitForAsynchronousFrames(session_);
        }
        if (sample) CFRelease(sample);
        if (block) CFRelease(block);
        // A session can end under the app (sleep, GPU change): the next keyframe makes a new one.
        if (status == kVTInvalidSessionErr) reset();
        return status == noErr && !failed_;
    }

private:
    bool make() {
        if (sps_.empty() || pps_.empty()) return false;
        const std::uint8_t* sets[2] = {sps_.data(), pps_.data()};
        const std::size_t sizes[2] = {sps_.size(), pps_.size()};
        if (CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, sets, sizes, 4, &format_) != noErr) {
            format_ = nullptr;
            return false;
        }
        NSDictionary* attributes = @{
            (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
            (id)kCVPixelBufferMetalCompatibilityKey: @YES
        };
        const VTDecompressionOutputCallbackRecord callback{&Impl::output, this};
        if (VTDecompressionSessionCreate(kCFAllocatorDefault, format_, nullptr, (__bridge CFDictionaryRef)attributes, &callback,
                                         &session_) != noErr) {
            session_ = nullptr;
            reset();
            return false;
        }
        VTSessionSetProperty(session_, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
        return true;
    }

    void reset() {
        if (session_) {
            VTDecompressionSessionInvalidate(session_);
            CFRelease(session_);
            session_ = nullptr;
        }
        if (format_) {
            CFRelease(format_);
            format_ = nullptr;
        }
    }

    static void output(void* self, void*, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image, CMTime, CMTime) {
        auto* impl = static_cast<Impl*>(self);
        if (status != noErr || !image || (flags & kVTDecodeInfo_FrameDropped)) {
            impl->failed_ = true;
            return;
        }
        impl->picture_(image);
    }

    Picture picture_;
    std::vector<std::uint8_t> sps_, pps_, sample_;  // sample_: the slices, each behind its 4-byte length.
    CMVideoFormatDescriptionRef format_ = nullptr;
    VTDecompressionSessionRef session_ = nullptr;
    bool failed_ = false;
};

H264Decoder::H264Decoder(Picture picture) : impl_(std::make_unique<Impl>(std::move(picture))) {}
H264Decoder::~H264Decoder() = default;
bool H264Decoder::decode(const std::uint8_t* annexb, std::size_t size) { return impl_->decode(annexb, size); }

} // namespace rec::mac
