#pragma once

#include "core/crypto.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rec::core {

// The DTLS 1.2 client side of a WebRTC connection (RFC 6347, 5764): ECDHE with AES-128-GCM,
// a self-signed P-256 certificate, the server's certificate checked against the SHA-256
// fingerprint from its SDP answer, and SRTP keys exported for AES-CM with HMAC-SHA1-80.
// It only handshakes: media goes over SRTP, so no application data is ever sent.
// Not thread-safe; the caller feeds it datagrams and the time.
class DtlsClient {
public:
    using Clock = std::chrono::steady_clock;
    using Bytes = crypto::Bytes;

    struct SrtpKeys {
        std::array<std::uint8_t, 16> local_key{}, remote_key{};
        std::array<std::uint8_t, 14> local_salt{}, remote_salt{};
    };

    DtlsClient();
    ~DtlsClient();
    DtlsClient(const DtlsClient&) = delete;
    DtlsClient& operator=(const DtlsClient&) = delete;

    // Makes the key and certificate. Empty on success.
    std::string init();
    // "AB:CD:...", the SHA-256 of the certificate, for a=fingerprint:sha-256.
    std::string fingerprint() const;
    // The server's fingerprint from its answer (hex with colons, any case).
    bool expect_fingerprint(const std::string& text);

    // Sends the first ClientHello.
    void start(Clock::time_point now);
    // One datagram that starts with a DTLS record (first byte 20 to 63).
    void receive(const std::uint8_t* data, std::size_t size, Clock::time_point now);
    // Retransmits the last flight when its timer runs out.
    void poll(Clock::time_point now);
    // Datagrams to send, in order.
    std::vector<Bytes> take_outgoing();
    // An encrypted close_notify alert, once connected.
    Bytes close_notify();

    bool connected() const noexcept { return connected_; }
    bool closed() const noexcept { return closed_; }  // The server sent close_notify.
    const std::string& failure() const noexcept { return failure_; }
    const SrtpKeys& keys() const noexcept { return keys_; }

private:
    struct Partial {
        std::uint8_t type = 0;
        Bytes body;
        std::vector<bool> have;
        std::size_t missing = 0;
    };
    // A handshake message, alert or ChangeCipherSpec of the last flight, re-recorded with
    // fresh record numbers on each retransmission.
    struct Item {
        std::uint8_t type = 0;
        std::uint16_t epoch = 0;
        Bytes body;
    };

    void fail(std::string text);
    void send_client_hello();
    void send_flight();
    Bytes handshake(std::uint8_t type, const Bytes& body);
    Bytes record(std::uint8_t type, std::uint16_t epoch, const Bytes& body);
    void handle_record(std::uint8_t type, std::uint16_t epoch, std::uint64_t seq, const std::uint8_t* data,
                       std::size_t size);
    void handle_fragments(const std::uint8_t* data, std::size_t size);
    void handle_message(std::uint8_t type, std::uint16_t seq, const Bytes& body);
    bool server_hello(const Bytes& body);
    bool certificate(const Bytes& body);
    bool key_exchange(const Bytes& body);
    void finish_client();
    bool server_finished(const Bytes& body);
    Bytes seal(std::uint8_t type, std::uint64_t seq, const Bytes& plain);
    bool open(std::uint8_t type, std::uint16_t epoch, std::uint64_t seq, const std::uint8_t* data, std::size_t size,
              Bytes& plain);

    std::unique_ptr<crypto::P256> identity_, ephemeral_;
    Bytes certificate_;
    std::array<std::uint8_t, 32> fingerprint_{}, expected_{};
    bool have_expected_ = false;

    std::array<std::uint8_t, 32> client_random_{}, server_random_{};
    Bytes cookie_;
    Bytes transcript_;       // Handshake messages as hashed, each with its 12-byte header.
    Bytes hello_;            // The latest ClientHello with its header: hashed once the server answers.
    std::uint16_t send_seq_ = 0, receive_seq_ = 0;
    std::uint64_t record_seq_[2] = {0, 0};
    std::map<std::uint16_t, Partial> partial_;  // Messages from receive_seq_ on, by message_seq.

    std::uint16_t cipher_ = 0;
    bool extended_master_ = false, srtp_ = false, certificate_requested_ = false;
    Bytes server_point_;     // From the server's certificate: an EC point, or an RSA key.
    Bytes server_modulus_, server_exponent_;
    Bytes server_ecdhe_;
    std::array<std::uint8_t, 48> master_{};
    std::unique_ptr<crypto::Aes128> write_, read_;
    std::array<std::uint8_t, 4> write_iv_{}, read_iv_{};

    std::vector<Item> flight_;  // The last flight, for retransmission.
    std::vector<Bytes> outgoing_;
    Clock::time_point now_{}, resend_at_{};
    std::chrono::milliseconds resend_wait_{1000};
    bool sent_finished_ = false;
    bool connected_ = false, closed_ = false;
    SrtpKeys keys_;
    std::string failure_;
};

// A self-signed X.509 certificate (CN=rec, valid for 30 days) for key, in DER; empty on failure.
crypto::Bytes self_signed_certificate(const crypto::P256& key);
// "AB:CD:...", hex bytes with colons, as SDP fingerprints are written.
std::string hex_colons(const std::uint8_t* data, std::size_t size);

// TLS 1.2 PRF with SHA-256 (RFC 5246 section 5).
crypto::Bytes tls_prf(const std::uint8_t* secret, std::size_t secret_size, const std::string& label,
                      const crypto::Bytes& seed, std::size_t size);

} // namespace rec::core
