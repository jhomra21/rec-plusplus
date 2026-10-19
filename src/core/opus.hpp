#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rec::core {

// Opus at 48 kHz stereo in 20 ms packets, for WebRTC (WHIP). libopus on Windows, the
// AudioToolbox encoder on macOS. One thread uses it.
class OpusEncoder {
public:
    static constexpr std::size_t kFrame = 960;
    using Packet = std::vector<std::uint8_t>;

    OpusEncoder();
    ~OpusEncoder();
    OpusEncoder(const OpusEncoder&) = delete;
    OpusEncoder& operator=(const OpusEncoder&) = delete;

    std::string open(std::uint32_t kbps);  // Empty on success.

    // Interleaved 16-bit stereo from a 48 kHz sample index. Calls packet(sample, bytes) for
    // each finished packet with the index of its first sample. A jump in the timeline starts
    // over there.
    template <typename Emit>
    void encode(std::int64_t start, const std::int16_t* pcm, std::size_t frames, Emit&& packet) {
        if (!codec_) return;
        if (start != pending_start_ + static_cast<std::int64_t>(pending_.size() / 2)) {
            pending_.clear();
            pending_start_ = next_out_ = start;
        }
        pending_.insert(pending_.end(), pcm, pcm + frames * 2);
        std::size_t used = 0;
        while (pending_.size() - used >= kFrame * 2) {
            for (auto& bytes : frame(pending_.data() + used)) {
                packet(next_out_, std::move(bytes));
                next_out_ += kFrame;
            }
            used += kFrame * 2;
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(used));
        pending_start_ += static_cast<std::int64_t>(used / 2);
    }

private:
    struct Codec;
    // One 20 ms frame in; the packets it finished out (an encoder may hold one back).
    std::vector<Packet> frame(const std::int16_t* pcm);

    std::unique_ptr<Codec> codec_;
    std::vector<std::int16_t> pending_;
    std::int64_t pending_start_ = 0, next_out_ = 0;
};

} // namespace rec::core
