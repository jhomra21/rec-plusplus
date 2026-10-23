#include "core/opus.hpp"

#include <opus.h>

namespace rec::core {

struct OpusEncoder::Codec {
    ::OpusEncoder* encoder = nullptr;
    ~Codec() { if (encoder) opus_encoder_destroy(encoder); }
};

OpusEncoder::OpusEncoder() = default;
OpusEncoder::~OpusEncoder() = default;

std::string OpusEncoder::open(std::uint32_t kbps) {
    auto codec = std::make_unique<Codec>();
    int error = OPUS_OK;
    codec->encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK || !codec->encoder) return "could not start the Opus encoder";
    opus_encoder_ctl(codec->encoder, OPUS_SET_BITRATE(static_cast<opus_int32>(kbps * 1000)));
    codec_ = std::move(codec);
    return {};
}

std::vector<OpusEncoder::Packet> OpusEncoder::frame(const std::int16_t* pcm) {
    Packet out(1500);
    const int size = opus_encode(codec_->encoder, pcm, static_cast<int>(kFrame), out.data(), static_cast<opus_int32>(out.size()));
    if (size <= 0) return {};
    out.resize(static_cast<std::size_t>(size));
    return {std::move(out)};
}

} // namespace rec::core
