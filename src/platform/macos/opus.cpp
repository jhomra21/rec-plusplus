#include "core/opus.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <cstring>

namespace rec::core {

struct OpusEncoder::Codec {
    AudioConverterRef converter = nullptr;
    const std::int16_t* input = nullptr;  // The frame being encoded, for supply().
    std::int16_t silence[2] = {0, 0};
    ~Codec() { if (converter) AudioConverterDispose(converter); }

    static OSStatus supply(AudioConverterRef, UInt32* packets, AudioBufferList* data, AudioStreamPacketDescription**,
                           void* context) {
        auto* self = static_cast<Codec*>(context);
        if (!self->input) {
            // No more input this call: the converter keeps what it has for the next one.
            *packets = 0;
            data->mBuffers[0].mData = self->silence;
            data->mBuffers[0].mDataByteSize = 0;
            return 1;
        }
        *packets = static_cast<UInt32>(kFrame);
        data->mBuffers[0].mData = const_cast<std::int16_t*>(self->input);
        data->mBuffers[0].mDataByteSize = static_cast<UInt32>(kFrame * 4);
        data->mBuffers[0].mNumberChannels = 2;
        self->input = nullptr;
        return noErr;
    }
};

OpusEncoder::OpusEncoder() = default;
OpusEncoder::~OpusEncoder() = default;

std::string OpusEncoder::open(std::uint32_t kbps) {
    AudioStreamBasicDescription in{};
    in.mSampleRate = 48000;
    in.mFormatID = kAudioFormatLinearPCM;
    in.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    in.mChannelsPerFrame = 2;
    in.mBitsPerChannel = 16;
    in.mBytesPerFrame = 4;
    in.mFramesPerPacket = 1;
    in.mBytesPerPacket = 4;
    AudioStreamBasicDescription out{};
    out.mSampleRate = 48000;
    out.mFormatID = kAudioFormatOpus;
    out.mChannelsPerFrame = 2;
    out.mFramesPerPacket = static_cast<UInt32>(kFrame);
    auto codec = std::make_unique<Codec>();
    if (AudioConverterNew(&in, &out, &codec->converter) != noErr) return "this Mac has no Opus encoder";
    const UInt32 bitrate = kbps * 1000;
    AudioConverterSetProperty(codec->converter, kAudioConverterEncodeBitRate, sizeof bitrate, &bitrate);
    codec_ = std::move(codec);
    return {};
}

std::vector<OpusEncoder::Packet> OpusEncoder::frame(const std::int16_t* pcm) {
    std::vector<Packet> packets;
    codec_->input = pcm;
    for (;;) {
        std::uint8_t buffer[1500];
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = 2;
        list.mBuffers[0].mDataByteSize = sizeof buffer;
        list.mBuffers[0].mData = buffer;
        UInt32 count = 1;
        AudioStreamPacketDescription description{};
        const OSStatus status =
            AudioConverterFillComplexBuffer(codec_->converter, Codec::supply, codec_.get(), &count, &list, &description);
        if (count && list.mBuffers[0].mDataByteSize) {
            const auto* start = buffer + description.mStartOffset;
            const auto size = description.mDataByteSize ? description.mDataByteSize : list.mBuffers[0].mDataByteSize;
            packets.emplace_back(start, start + size);
        }
        if (status != noErr || !count || codec_->input == nullptr) break;
    }
    codec_->input = nullptr;
    return packets;
}

} // namespace rec::core
