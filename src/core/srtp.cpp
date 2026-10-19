#include "core/srtp.hpp"

#include <algorithm>
#include <cstring>

namespace rec::core {
namespace {
constexpr std::size_t kTag = 10;

std::uint32_t load32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}
void store32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}
} // namespace

Srtp::Srtp(const std::uint8_t* master_key, const std::uint8_t* master_salt) {
    const crypto::Aes128 master(master_key);
    if (!master.valid()) return;
    make(rtp_, master, master_salt, 0);
    make(rtcp_, master, master_salt, 3);
    valid_ = rtp_.cipher->valid() && rtcp_.cipher->valid();
}

bool Srtp::valid() const noexcept { return valid_; }

void Srtp::derive(const crypto::Aes128& master, const std::uint8_t* master_salt, std::uint8_t label,
                  std::uint8_t* out, std::size_t size) {
    // With a key derivation rate of 0, x = label at byte 7, XOR the salt; the IV is x * 2^16.
    std::uint8_t iv[16]{};
    std::memcpy(iv, master_salt, 14);
    iv[7] ^= label;
    for (std::size_t block = 0; block * 16 < size; ++block) {
        std::uint8_t counter[16], stream[16];
        std::memcpy(counter, iv, 16);
        counter[15] = static_cast<std::uint8_t>(block);
        master.encrypt(counter, stream, 16);
        std::memcpy(out + block * 16, stream, std::min<std::size_t>(16, size - block * 16));
    }
}

void Srtp::make(Keys& keys, const crypto::Aes128& master, const std::uint8_t* master_salt, std::uint8_t first_label) {
    std::uint8_t key[16];
    derive(master, master_salt, first_label, key, 16);
    keys.cipher = std::make_unique<crypto::Aes128>(key);
    derive(master, master_salt, static_cast<std::uint8_t>(first_label + 1), keys.auth, 20);
    derive(master, master_salt, static_cast<std::uint8_t>(first_label + 2), keys.salt, 14);
}

void Srtp::crypt(const Keys& keys, std::uint32_t ssrc, std::uint64_t index, std::uint8_t* data, std::size_t size) {
    // IV = salt * 2^16 XOR ssrc * 2^64 XOR index * 2^16; the low 16 bits count blocks.
    std::uint8_t iv[16]{};
    std::memcpy(iv, keys.salt, 14);
    iv[4] ^= static_cast<std::uint8_t>(ssrc >> 24);
    iv[5] ^= static_cast<std::uint8_t>(ssrc >> 16);
    iv[6] ^= static_cast<std::uint8_t>(ssrc >> 8);
    iv[7] ^= static_cast<std::uint8_t>(ssrc);
    for (int i = 0; i < 6; ++i) iv[8 + i] ^= static_cast<std::uint8_t>(index >> (40 - 8 * i));
    const std::size_t blocks = (size + 15) / 16;
    stream_.resize(blocks * 16);
    for (std::size_t b = 0; b < blocks; ++b) {
        std::uint8_t* counter = stream_.data() + b * 16;
        std::memcpy(counter, iv, 16);
        counter[14] = static_cast<std::uint8_t>(b >> 8);
        counter[15] = static_cast<std::uint8_t>(b);
    }
    keys.cipher->encrypt(stream_.data(), stream_.data(), stream_.size());
    for (std::size_t i = 0; i < size; ++i) data[i] ^= stream_[i];
}

bool Srtp::protect_rtp(crypto::Bytes& packet) {
    if (!valid_ || packet.size() < 12) return false;
    std::size_t header = 12 + 4 * (packet[0] & 0x0F);
    if (packet[0] & 0x10) {
        if (packet.size() < header + 4) return false;
        header += 4 + 4 * (static_cast<std::size_t>(packet[header + 2]) << 8 | packet[header + 3]);
    }
    if (packet.size() < header) return false;
    const auto seq = static_cast<std::uint16_t>(packet[2] << 8 | packet[3]);
    const std::uint32_t ssrc = load32(packet.data() + 8);
    auto& sender = senders_[ssrc];
    if (sender.started && seq < sender.last && sender.last - seq > 0x8000) ++sender.roc;
    sender.last = seq;
    sender.started = true;
    const std::uint64_t index = static_cast<std::uint64_t>(sender.roc) << 16 | seq;
    crypt(rtp_, ssrc, index, packet.data() + header, packet.size() - header);
    const std::size_t size = packet.size();
    packet.resize(size + 4);
    store32(packet.data() + size, sender.roc);
    const auto tag = crypto::hmac_sha1(rtp_.auth, 20, packet.data(), packet.size());
    packet.resize(size);
    packet.insert(packet.end(), tag.begin(), tag.begin() + kTag);
    return true;
}

bool Srtp::protect_rtcp(crypto::Bytes& packet) {
    if (!valid_ || packet.size() < 8) return false;
    const std::uint32_t index = rtcp_index_++ & 0x7FFFFFFF;
    crypt(rtcp_, load32(packet.data() + 4), index, packet.data() + 8, packet.size() - 8);
    const std::size_t size = packet.size();
    packet.resize(size + 4);
    store32(packet.data() + size, 0x80000000u | index);
    const auto tag = crypto::hmac_sha1(rtcp_.auth, 20, packet.data(), packet.size());
    packet.insert(packet.end(), tag.begin(), tag.begin() + kTag);
    return true;
}

bool Srtp::unprotect_rtcp(crypto::Bytes& packet) {
    if (!valid_ || packet.size() < 8 + 4 + kTag) return false;
    const std::size_t signed_size = packet.size() - kTag;
    const auto tag = crypto::hmac_sha1(rtcp_.auth, 20, packet.data(), signed_size);
    std::uint8_t differ = 0;
    for (std::size_t i = 0; i < kTag; ++i) differ |= static_cast<std::uint8_t>(tag[i] ^ packet[signed_size + i]);
    if (differ) return false;
    const std::uint32_t e_index = load32(packet.data() + signed_size - 4);
    const std::size_t body = signed_size - 4;
    if (e_index & 0x80000000u) crypt(rtcp_, load32(packet.data() + 4), e_index & 0x7FFFFFFF, packet.data() + 8, body - 8);
    packet.resize(body);
    return true;
}

} // namespace rec::core
