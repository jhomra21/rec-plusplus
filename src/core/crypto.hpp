#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// The few primitives WebRTC needs (DTLS 1.2, SRTP, STUN), on the platform's crypto library:
// CNG (bcrypt) on Windows, CommonCrypto and Security on macOS.
namespace rec::core::crypto {

using Bytes = std::vector<std::uint8_t>;

bool random(std::uint8_t* out, std::size_t size);
std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t size);
std::array<std::uint8_t, 20> hmac_sha1(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* data,
                                       std::size_t size);
std::array<std::uint8_t, 32> hmac_sha256(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* data,
                                         std::size_t size);

// AES-128 single blocks (ECB); counter mode and GCM are built on it.
class Aes128 {
public:
    explicit Aes128(const std::uint8_t* key);
    ~Aes128();
    Aes128(const Aes128&) = delete;
    Aes128& operator=(const Aes128&) = delete;
    bool valid() const noexcept;
    // size is a multiple of 16; in and out may be the same.
    void encrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t size) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A P-256 key pair: a signing key for the DTLS certificate, or an ephemeral ECDH key.
class P256 {
public:
    enum class Use { sign, agree };
    static std::unique_ptr<P256> generate(Use use);
    ~P256();
    P256(const P256&) = delete;
    P256& operator=(const P256&) = delete;
    // A signing key saved with private_key(); null if the bytes are not one.
    static std::unique_ptr<P256> import_private(const Bytes& key);
    const Bytes& public_point() const noexcept;  // 65 bytes: 0x04, X, Y.
    Bytes private_key() const;                   // 97 bytes: 0x04, X, Y, then the secret. Empty on failure.
    Bytes sign(const std::uint8_t* data, std::size_t size) const;  // ECDSA with SHA-256, DER. Empty on failure.
    Bytes agree(const Bytes& peer_point) const;                    // The shared X coordinate. Empty on failure.

    struct Impl;
    explicit P256(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> impl_;
};

// ECDSA P-256 with SHA-256; signature in DER.
bool verify_p256(const Bytes& point, const std::uint8_t* data, std::size_t size, const Bytes& signature);
// RSASSA-PKCS1-v1_5 with SHA-256; modulus and exponent big-endian.
bool verify_rsa(const Bytes& modulus, const Bytes& exponent, const std::uint8_t* data, std::size_t size,
                const Bytes& signature);

} // namespace rec::core::crypto
