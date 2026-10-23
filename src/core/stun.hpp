#pragma once

#include "core/crypto.hpp"
#include "core/net.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// STUN messages for ICE (RFC 8489, attributes from RFC 8445): building requests and answers
// signed with MESSAGE-INTEGRITY and FINGERPRINT.
namespace rec::core::stun {

inline constexpr std::uint32_t kMagic = 0x2112A442;
inline constexpr std::uint16_t kBindingRequest = 0x0001, kBindingSuccess = 0x0101;
inline constexpr std::uint16_t kUsername = 0x0006, kMessageIntegrity = 0x0008, kXorMappedAddress = 0x0020,
                               kPriority = 0x0024, kUseCandidate = 0x0025, kFingerprint = 0x8028,
                               kIceControlled = 0x8029, kIceControlling = 0x802A;

inline std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Whether a datagram is a STUN message.
inline bool is_stun(const std::uint8_t* data, std::size_t size) {
    return size >= 20 && data[0] < 4 &&
           (static_cast<std::uint32_t>(data[4]) << 24 | static_cast<std::uint32_t>(data[5]) << 16 |
            static_cast<std::uint32_t>(data[6]) << 8 | data[7]) == kMagic;
}
inline std::uint16_t type_of(const std::uint8_t* data) { return static_cast<std::uint16_t>(data[0] << 8 | data[1]); }

inline bool has_attribute(const std::uint8_t* data, std::size_t size, std::uint16_t wanted) {
    for (std::size_t at = 20; at + 4 <= size;) {
        const std::uint16_t type = static_cast<std::uint16_t>(data[at] << 8 | data[at + 1]);
        const std::size_t length = static_cast<std::size_t>(data[at + 2]) << 8 | data[at + 3];
        if (at + 4 + length > size) break;
        if (type == wanted) return true;
        at += 4 + (length + 3) / 4 * 4;
    }
    return false;
}

// The text of an attribute, empty when absent.
inline std::string text_attribute(const std::uint8_t* data, std::size_t size, std::uint16_t wanted) {
    for (std::size_t at = 20; at + 4 <= size;) {
        const std::uint16_t type = static_cast<std::uint16_t>(data[at] << 8 | data[at + 1]);
        const std::size_t length = static_cast<std::size_t>(data[at + 2]) << 8 | data[at + 3];
        if (at + 4 + length > size) break;
        if (type == wanted) return std::string(reinterpret_cast<const char*>(data + at + 4), length);
        at += 4 + (length + 3) / 4 * 4;
    }
    return {};
}

class Message {
public:
    Message(std::uint16_t type, const std::uint8_t* txid) {
        put16(type);
        put16(0);
        put16(kMagic >> 16);
        put16(kMagic & 0xFFFF);
        m_.insert(m_.end(), txid, txid + 12);
    }
    void attribute(std::uint16_t type, const std::uint8_t* data, std::size_t size) {
        put16(type);
        put16(static_cast<std::uint32_t>(size));
        if (size) m_.insert(m_.end(), data, data + size);
        while (m_.size() % 4) m_.push_back(0);
    }
    void attribute(std::uint16_t type, const std::string& text) {
        attribute(type, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    }
    // XOR-MAPPED-ADDRESS: where the request came from.
    void mapped_address(const Endpoint& from) {
        crypto::Bytes value{0, static_cast<std::uint8_t>(from.v6 ? 2 : 1)};
        const std::uint16_t port = static_cast<std::uint16_t>(from.port ^ (kMagic >> 16));
        value.push_back(static_cast<std::uint8_t>(port >> 8));
        value.push_back(static_cast<std::uint8_t>(port));
        std::uint8_t mask[16];
        for (int i = 0; i < 4; ++i) mask[i] = static_cast<std::uint8_t>(kMagic >> (24 - 8 * i));
        std::memcpy(mask + 4, m_.data() + 8, 12);
        for (int i = 0; i < (from.v6 ? 16 : 4); ++i) value.push_back(static_cast<std::uint8_t>(from.address[i] ^ mask[i]));
        attribute(kXorMappedAddress, value.data(), value.size());
    }
    // MESSAGE-INTEGRITY then FINGERPRINT, each over the message with its length so far.
    crypto::Bytes finish(const std::string& password) {
        set_length(m_.size() - 20 + 24);
        const auto mac = crypto::hmac_sha1(reinterpret_cast<const std::uint8_t*>(password.data()), password.size(),
                                           m_.data(), m_.size());
        attribute(kMessageIntegrity, mac.data(), mac.size());
        set_length(m_.size() - 20 + 8);
        const std::uint32_t fingerprint = crc32(m_.data(), m_.size()) ^ 0x5354554Eu;
        std::uint8_t value[4] = {static_cast<std::uint8_t>(fingerprint >> 24), static_cast<std::uint8_t>(fingerprint >> 16),
                                 static_cast<std::uint8_t>(fingerprint >> 8), static_cast<std::uint8_t>(fingerprint)};
        attribute(kFingerprint, value, 4);
        return m_;
    }

private:
    void put16(std::uint32_t v) {
        m_.push_back(static_cast<std::uint8_t>(v >> 8));
        m_.push_back(static_cast<std::uint8_t>(v));
    }
    void set_length(std::size_t length) {
        m_[2] = static_cast<std::uint8_t>(length >> 8);
        m_[3] = static_cast<std::uint8_t>(length);
    }
    crypto::Bytes m_;
};

} // namespace rec::core::stun
