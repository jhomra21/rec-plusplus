#pragma once

#include "core/crypto.hpp"

#include <cstdint>
#include <map>
#include <memory>

namespace rec::core {

// SRTP and SRTCP with AES_CM_128_HMAC_SHA1_80 (RFC 3711), one direction: a context made
// from the local keys protects what is sent, one from the remote keys unprotects.
class Srtp {
public:
    Srtp(const std::uint8_t* master_key, const std::uint8_t* master_salt);
    bool valid() const noexcept;
    // Encrypts an RTP packet in place and appends its 10-byte tag.
    bool protect_rtp(crypto::Bytes& packet);
    // Encrypts a compound RTCP packet in place and appends the SRTCP index and tag.
    bool protect_rtcp(crypto::Bytes& packet);
    // Checks and decrypts an SRTCP packet in place, removing the index and tag.
    bool unprotect_rtcp(crypto::Bytes& packet);
    // Checks and decrypts an SRTP packet in place, removing the tag. The rollover counter is
    // guessed from the highest sequence number seen (RFC 3711 appendix A).
    bool unprotect_rtp(crypto::Bytes& packet);

private:
    struct Keys {
        std::unique_ptr<crypto::Aes128> cipher;
        std::uint8_t auth[20]{};
        std::uint8_t salt[14]{};
    };
    void derive(const crypto::Aes128& master, const std::uint8_t* master_salt, std::uint8_t label, std::uint8_t* out,
                std::size_t size);
    void make(Keys& keys, const crypto::Aes128& master, const std::uint8_t* master_salt, std::uint8_t first_label);
    // XORs the AES-CM keystream for (ssrc, index) over data.
    void crypt(const Keys& keys, std::uint32_t ssrc, std::uint64_t index, std::uint8_t* data, std::size_t size);

    struct Sender {
        std::uint32_t roc = 0;
        std::uint16_t last = 0;
        bool started = false;
    };
    Keys rtp_, rtcp_;
    std::map<std::uint32_t, Sender> senders_, receivers_;
    std::uint32_t rtcp_index_ = 0;
    crypto::Bytes stream_;  // Counter blocks, then keystream.
    bool valid_ = false;
};

} // namespace rec::core
