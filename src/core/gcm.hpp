#pragma once

#include "core/crypto.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// AES-128-GCM (NIST SP 800-38D) for TLS and DTLS 1.2 records. GHASH is bitwise: it only
// protects handshakes and a few kilobytes of HTTP, never media.
namespace rec::core::gcm {

inline std::uint64_t load64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
    return v;
}
inline void store64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = static_cast<std::uint8_t>(v); v >>= 8; }
}
inline void gf_multiply(std::uint8_t* x, const std::uint8_t* h) {
    std::uint64_t zh = 0, zl = 0, vh = load64(h), vl = load64(h + 8);
    for (int i = 0; i < 128; ++i) {
        if (x[i / 8] & (0x80 >> (i % 8))) { zh ^= vh; zl ^= vl; }
        const bool low = vl & 1;
        vl = vl >> 1 | vh << 63;
        vh >>= 1;
        if (low) vh ^= 0xE100000000000000ULL;
    }
    store64(x, zh);
    store64(x + 8, zl);
}
inline void ghash(const std::uint8_t* h, const crypto::Bytes& aad, const std::uint8_t* text, std::size_t size,
                  std::uint8_t* out) {
    std::memset(out, 0, 16);
    auto absorb = [&](const std::uint8_t* data, std::size_t n) {
        for (std::size_t i = 0; i < n; i += 16) {
            for (std::size_t j = 0; j < 16 && i + j < n; ++j) out[j] ^= data[i + j];
            gf_multiply(out, h);
        }
    };
    absorb(aad.data(), aad.size());
    absorb(text, size);
    std::uint8_t lengths[16];
    store64(lengths, aad.size() * 8);
    store64(lengths + 8, size * 8);
    absorb(lengths, 16);
}
// Counter mode from J0 + 1; returns E(K, J0) for the tag.
inline std::array<std::uint8_t, 16> crypt(const crypto::Aes128& aes, const std::uint8_t* iv12, const std::uint8_t* in,
                                          std::uint8_t* out, std::size_t size) {
    std::uint8_t block[16], stream[16];
    std::memcpy(block, iv12, 12);
    std::array<std::uint8_t, 16> tag_mask{};
    for (std::uint32_t counter = 1;; ++counter) {
        block[12] = static_cast<std::uint8_t>(counter >> 24);
        block[13] = static_cast<std::uint8_t>(counter >> 16);
        block[14] = static_cast<std::uint8_t>(counter >> 8);
        block[15] = static_cast<std::uint8_t>(counter);
        aes.encrypt(block, stream, 16);
        if (counter == 1) { std::memcpy(tag_mask.data(), stream, 16); continue; }
        const std::size_t at = (counter - 2) * 16;
        if (at >= size) break;
        for (std::size_t i = 0; i < 16 && at + i < size; ++i) out[at + i] = in[at + i] ^ stream[i];
    }
    return tag_mask;
}
inline std::array<std::uint8_t, 16> hash_key(const crypto::Aes128& aes) {
    std::array<std::uint8_t, 16> zero{}, h{};
    aes.encrypt(zero.data(), h.data(), 16);
    return h;
}
// Encrypts size bytes of in to out and writes the 16-byte tag after them.
inline void seal(const crypto::Aes128& aes, const std::uint8_t* iv12, const crypto::Bytes& aad, const std::uint8_t* in,
                 std::uint8_t* out, std::size_t size) {
    const auto mask = crypt(aes, iv12, in, out, size);
    const auto h = hash_key(aes);
    std::uint8_t tag[16];
    ghash(h.data(), aad, out, size, tag);
    for (int i = 0; i < 16; ++i) out[size + i] = tag[i] ^ mask[i];
}
// Checks the tag after size bytes of in and decrypts them to out.
inline bool open(const crypto::Aes128& aes, const std::uint8_t* iv12, const crypto::Bytes& aad, const std::uint8_t* in,
                 std::uint8_t* out, std::size_t size) {
    const auto h = hash_key(aes);
    std::uint8_t tag[16];
    ghash(h.data(), aad, in, size, tag);
    const auto mask = crypt(aes, iv12, in, out, size);
    std::uint8_t differ = 0;
    for (int i = 0; i < 16; ++i) differ |= static_cast<std::uint8_t>((tag[i] ^ mask[i]) ^ in[size + i]);
    return differ == 0;
}

} // namespace rec::core::gcm
