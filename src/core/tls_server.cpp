#include "core/tls_server.hpp"

#include "core/dtls.hpp"
#include "core/gcm.hpp"

#include <algorithm>
#include <cstring>

namespace rec::core {
namespace {

using Bytes = crypto::Bytes;

constexpr std::uint8_t kChangeCipherSpec = 20, kAlert = 21, kHandshake = 22, kApplicationData = 23;
constexpr std::uint8_t kClientHello = 1, kServerHello = 2, kCertificate = 11, kServerKeyExchange = 12,
                       kServerHelloDone = 14, kClientKeyExchange = 16, kFinished = 20;
constexpr std::uint16_t kEcdheEcdsaAes128Gcm = 0xC02B, kEcdsaSha256 = 0x0403, kSecp256r1 = 0x0017;
constexpr std::size_t kMaxRecord = 16384;

void put8(Bytes& b, std::uint64_t v) { b.push_back(static_cast<std::uint8_t>(v)); }
void put16(Bytes& b, std::uint64_t v) { put8(b, v >> 8); put8(b, v); }
void put24(Bytes& b, std::uint64_t v) { put8(b, v >> 16); put16(b, v); }
void put64(Bytes& b, std::uint64_t v) { put16(b, v >> 48); put16(b, v >> 32); put16(b, v >> 16); put16(b, v); }
void append(Bytes& b, const std::uint8_t* data, std::size_t size) { b.insert(b.end(), data, data + size); }
void append(Bytes& b, const Bytes& more) { b.insert(b.end(), more.begin(), more.end()); }

struct Reader {
    const std::uint8_t* data;
    std::size_t size, at = 0;
    bool ok = true;
    Reader(const std::uint8_t* d, std::size_t n) : data(d), size(n) {}
    explicit Reader(const Bytes& b) : data(b.data()), size(b.size()) {}
    std::size_t left() const { return ok ? size - at : 0; }
    std::uint32_t number(int bytes) {
        if (!ok || size - at < static_cast<std::size_t>(bytes)) { ok = false; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < bytes; ++i) v = v << 8 | data[at++];
        return v;
    }
    const std::uint8_t* take(std::size_t n) {
        if (!ok || size - at < n) { ok = false; return nullptr; }
        const auto* p = data + at;
        at += n;
        return p;
    }
};

bool read_exact(Transport& t, std::uint8_t* out, std::size_t size) {
    while (size) {
        const std::size_t got = t.receive(out, size);
        if (!got) return false;
        out += got;
        size -= got;
    }
    return true;
}

// Both directions' AES-GCM state once ChangeCipherSpec has passed.
struct Cipher {
    std::unique_ptr<crypto::Aes128> aes;
    std::uint8_t salt[4]{};
    std::uint64_t seq = 0;

    Bytes seal(std::uint8_t type, const std::uint8_t* plain, std::size_t size) {
        Bytes explicit_nonce;
        put64(explicit_nonce, seq);
        std::uint8_t iv[12];
        std::memcpy(iv, salt, 4);
        std::memcpy(iv + 4, explicit_nonce.data(), 8);
        Bytes aad;
        put64(aad, seq++);
        put8(aad, type);
        put16(aad, 0x0303);
        put16(aad, size);
        Bytes out = explicit_nonce;
        out.resize(8 + size + 16);
        gcm::seal(*aes, iv, aad, plain, out.data() + 8, size);
        return out;
    }
    bool open(std::uint8_t type, const Bytes& record, Bytes& plain) {
        if (record.size() < 24) return false;
        const std::size_t size = record.size() - 24;
        std::uint8_t iv[12];
        std::memcpy(iv, salt, 4);
        std::memcpy(iv + 4, record.data(), 8);
        Bytes aad;
        put64(aad, seq++);
        put8(aad, type);
        put16(aad, 0x0303);
        put16(aad, size);
        plain.resize(size);
        return gcm::open(*aes, iv, aad, record.data() + 8, plain.data(), size);
    }
};

class TlsTransport final : public Transport {
public:
    TlsTransport(std::unique_ptr<Transport> tcp, Cipher read, Cipher write)
        : tcp_(std::move(tcp)), read_(std::move(read)), write_(std::move(write)) {}

    bool send(const std::uint8_t* data, std::size_t size) override {
        while (size) {
            const std::size_t n = std::min(size, kMaxRecord);
            if (!send_record(kApplicationData, data, n)) return false;
            data += n;
            size -= n;
        }
        return true;
    }

    std::size_t receive(std::uint8_t* data, std::size_t size) override {
        while (at_ >= plain_.size()) {
            if (closed_) return 0;
            std::uint8_t type = 0;
            Bytes record;
            if (!read_record(type, record)) { closed_ = true; return 0; }
            plain_.clear();
            at_ = 0;
            if (!read_.open(type, record, plain_)) { closed_ = true; return 0; }
            if (type == kAlert) { closed_ = true; return 0; }
            if (type != kApplicationData) plain_.clear();  // A renegotiation request is ignored.
        }
        const std::size_t n = std::min(size, plain_.size() - at_);
        std::memcpy(data, plain_.data() + at_, n);
        at_ += n;
        return n;
    }

    bool readable() override { return at_ < plain_.size() || closed_ || tcp_->readable(); }
    void interrupt() override { tcp_->interrupt(); }
    void finish() override {
        const std::uint8_t close_notify[2] = {1, 0};
        send_record(kAlert, close_notify, 2);
        tcp_->finish();
    }

private:
    bool send_record(std::uint8_t type, const std::uint8_t* data, std::size_t size) {
        const Bytes sealed = write_.seal(type, data, size);
        Bytes out;
        put8(out, type);
        put16(out, 0x0303);
        put16(out, sealed.size());
        append(out, sealed);
        return tcp_->send(out.data(), out.size());
    }
    bool read_record(std::uint8_t& type, Bytes& body) {
        std::uint8_t header[5];
        if (!read_exact(*tcp_, header, 5)) return false;
        type = header[0];
        const std::size_t length = static_cast<std::size_t>(header[3]) << 8 | header[4];
        if (length > kMaxRecord + 2048) return false;
        body.resize(length);
        return read_exact(*tcp_, body.data(), length);
    }

    std::unique_ptr<Transport> tcp_;
    Cipher read_, write_;
    Bytes plain_;
    std::size_t at_ = 0;
    bool closed_ = false;
};

// The handshake, one message at a time from records that may split or join them.
class Handshake {
public:
    Handshake(Transport& tcp, const crypto::P256& key, const Bytes& certificate)
        : tcp_(tcp), key_(key), certificate_(certificate) {}

    std::string run(Cipher& read, Cipher& write) {
        std::uint8_t type = 0;
        Bytes body;
        if (auto e = next(type, body); !e.empty()) return e;
        if (type != kClientHello) return "expected a TLS ClientHello";
        if (auto e = client_hello(body); !e.empty()) return alert(40, e);
        crypto::random(server_random_, 32);
        auto ephemeral = crypto::P256::generate(crypto::P256::Use::agree);
        if (!ephemeral) return "could not make a TLS key";

        Bytes flight;
        {
            Bytes hello;
            put16(hello, 0x0303);
            append(hello, server_random_, 32);
            put8(hello, 0);  // No session to resume.
            put16(hello, kEcdheEcdsaAes128Gcm);
            put8(hello, 0);
            Bytes extensions;
            auto extension = [&](std::uint16_t t, const Bytes& data) {
                put16(extensions, t);
                put16(extensions, data.size());
                append(extensions, data);
            };
            extension(0xFF01, {0x00});  // renegotiation_info: none so far.
            extension(0x000B, {0x01, 0x00});
            if (extended_master_) extension(0x0017, {});
            put16(hello, extensions.size());
            append(hello, extensions);
            add(flight, kServerHello, hello);
        }
        {
            Bytes list;
            put24(list, certificate_.size() + 3);
            put24(list, certificate_.size());
            append(list, certificate_);
            add(flight, kCertificate, list);
        }
        {
            Bytes params{3};
            put16(params, kSecp256r1);
            put8(params, ephemeral->public_point().size());
            append(params, ephemeral->public_point());
            Bytes signed_data(client_random_, client_random_ + 32);
            append(signed_data, server_random_, 32);
            append(signed_data, params);
            const Bytes signature = key_.sign(signed_data.data(), signed_data.size());
            if (signature.empty()) return "could not sign the TLS key exchange";
            Bytes exchange = params;
            put16(exchange, kEcdsaSha256);
            put16(exchange, signature.size());
            append(exchange, signature);
            add(flight, kServerKeyExchange, exchange);
        }
        add(flight, kServerHelloDone, {});
        if (!send_plain(kHandshake, flight)) return "the browser closed the connection";

        if (auto e = next(type, body); !e.empty()) return e;
        if (type != kClientKeyExchange) return alert(10, "expected a TLS ClientKeyExchange");
        Reader r(body);
        const std::size_t point_size = r.number(1);
        const auto* point = r.take(point_size);
        if (!point) return alert(50, "bad TLS ClientKeyExchange");
        const Bytes premaster = ephemeral->agree(Bytes(point, point + point_size));
        if (premaster.size() != 32) return alert(47, "the TLS key exchange failed");
        Bytes master;
        if (extended_master_) {
            const auto session = crypto::sha256(transcript_.data(), transcript_.size());
            master = tls_prf(premaster.data(), premaster.size(), "extended master secret", Bytes(session.begin(), session.end()), 48);
        } else {
            Bytes randoms(client_random_, client_random_ + 32);
            append(randoms, server_random_, 32);
            master = tls_prf(premaster.data(), premaster.size(), "master secret", randoms, 48);
        }
        Bytes server_first(server_random_, server_random_ + 32);
        append(server_first, client_random_, 32);
        const Bytes block = tls_prf(master.data(), master.size(), "key expansion", server_first, 40);
        read.aes = std::make_unique<crypto::Aes128>(block.data());
        write.aes = std::make_unique<crypto::Aes128>(block.data() + 16);
        std::memcpy(read.salt, block.data() + 32, 4);
        std::memcpy(write.salt, block.data() + 36, 4);
        if (!read.aes->valid() || !write.aes->valid()) return "could not set up TLS encryption";

        // ChangeCipherSpec, then the client's Finished under the new keys.
        std::uint8_t record_type = 0;
        Bytes record;
        if (!read_record(record_type, record) || record_type != kChangeCipherSpec) return alert(10, "expected a TLS ChangeCipherSpec");
        if (!read_record(record_type, record) || record_type != kHandshake) return alert(10, "expected the TLS Finished message");
        Bytes plain;
        if (!read.open(kHandshake, record, plain)) return alert(20, "the TLS Finished message did not decrypt");
        const auto before = crypto::sha256(transcript_.data(), transcript_.size());
        const Bytes expected = tls_prf(master.data(), master.size(), "client finished", Bytes(before.begin(), before.end()), 12);
        Reader finished(plain);
        if (finished.number(1) != kFinished || finished.number(3) != 12 || finished.left() != 12 ||
            std::memcmp(plain.data() + 4, expected.data(), 12) != 0)
            return "the browser's TLS Finished message is wrong";
        append(transcript_, plain);

        const auto hash = crypto::sha256(transcript_.data(), transcript_.size());
        const Bytes verify = tls_prf(master.data(), master.size(), "server finished", Bytes(hash.begin(), hash.end()), 12);
        Bytes message{kFinished};
        put24(message, 12);
        append(message, verify);
        if (!send_plain(kChangeCipherSpec, Bytes{1})) return "the browser closed the connection";
        const Bytes sealed = write.seal(kHandshake, message.data(), message.size());
        if (!send_plain(kHandshake, sealed)) return "the browser closed the connection";
        return {};
    }

private:
    std::string alert(std::uint8_t description, std::string text) {
        send_plain(kAlert, Bytes{2, description});
        return text;
    }

    std::string client_hello(const Bytes& body) {
        Reader r(body);
        if (r.number(2) < 0x0303) return "the browser does not offer TLS 1.2";
        const auto* random = r.take(32);
        r.take(r.number(1));
        const std::size_t suites_size = r.number(2);
        const auto* suites = r.take(suites_size);
        r.take(r.number(1));
        if (!r.ok || !random) return "bad TLS ClientHello";
        std::memcpy(client_random_, random, 32);
        bool suite = false;
        for (std::size_t i = 0; i + 1 < suites_size; i += 2)
            if ((suites[i] << 8 | suites[i + 1]) == kEcdheEcdsaAes128Gcm) suite = true;
        if (!suite) return "the browser does not offer ECDHE-ECDSA with AES-128-GCM";
        bool curve = true, signature = true;
        if (r.left() >= 2) {
            const std::size_t total = r.number(2);
            Reader extensions(r.take(total), total);
            while (extensions.left() >= 4) {
                const auto type = extensions.number(2);
                const std::size_t length = extensions.number(2);
                Reader data(extensions.take(length), length);
                if (!extensions.ok) break;
                if (type == 0x000A) {
                    curve = false;
                    const std::size_t n = data.number(2);
                    for (std::size_t i = 0; i + 1 < n; i += 2) curve |= data.number(2) == kSecp256r1;
                } else if (type == 0x000D) {
                    signature = false;
                    const std::size_t n = data.number(2);
                    for (std::size_t i = 0; i + 1 < n; i += 2) signature |= data.number(2) == kEcdsaSha256;
                } else if (type == 0x0017) {
                    extended_master_ = true;
                }
            }
        }
        if (!curve) return "the browser does not offer the P-256 curve";
        if (!signature) return "the browser does not accept ECDSA with SHA-256";
        return {};
    }

    void add(Bytes& flight, std::uint8_t type, const Bytes& body) {
        Bytes message{type};
        put24(message, body.size());
        append(message, body);
        append(transcript_, message);
        append(flight, message);
    }

    bool send_plain(std::uint8_t type, const Bytes& body) {
        for (std::size_t at = 0; at < body.size() || (at == 0 && body.empty());) {
            const std::size_t n = std::min(body.size() - at, kMaxRecord);
            Bytes out;
            put8(out, type);
            put16(out, 0x0303);
            put16(out, n);
            append(out, body.data() + at, n);
            if (!tcp_.send(out.data(), out.size())) return false;
            at += n;
            if (body.empty()) break;
        }
        return true;
    }

    bool read_record(std::uint8_t& type, Bytes& body) {
        std::uint8_t header[5];
        if (!read_exact(tcp_, header, 5) || header[1] != 3) return false;
        type = header[0];
        const std::size_t length = static_cast<std::size_t>(header[3]) << 8 | header[4];
        if (length > kMaxRecord + 2048) return false;
        body.resize(length);
        return read_exact(tcp_, body.data(), length);
    }

    // The next plaintext handshake message, hashed into the transcript.
    std::string next(std::uint8_t& type, Bytes& body) {
        while (pending_.size() < 4 || pending_.size() < 4 + (static_cast<std::size_t>(pending_[1]) << 16 |
                                                              static_cast<std::size_t>(pending_[2]) << 8 | pending_[3])) {
            std::uint8_t record_type = 0;
            Bytes record;
            if (!read_record(record_type, record)) return "the browser closed the connection during the TLS handshake";
            if (record_type == kAlert) return "the browser refused the certificate (TLS alert)";
            if (record_type != kHandshake) return "unexpected TLS record";
            append(pending_, record);
            if (pending_.size() > 65536) return "TLS handshake message too large";
        }
        const std::size_t length = static_cast<std::size_t>(pending_[1]) << 16 | static_cast<std::size_t>(pending_[2]) << 8 | pending_[3];
        type = pending_[0];
        body.assign(pending_.begin() + 4, pending_.begin() + static_cast<std::ptrdiff_t>(4 + length));
        append(transcript_, pending_.data(), 4 + length);
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(4 + length));
        return {};
    }

    Transport& tcp_;
    const crypto::P256& key_;
    const Bytes& certificate_;
    Bytes pending_, transcript_;
    std::uint8_t client_random_[32]{}, server_random_[32]{};
    bool extended_master_ = false;
};
} // namespace

std::unique_ptr<Transport> tls_accept(std::unique_ptr<Transport> tcp, const crypto::P256& key, const Bytes& certificate,
                                      std::string& error) {
    Cipher read, write;
    Handshake handshake(*tcp, key, certificate);
    error = handshake.run(read, write);
    if (!error.empty()) return nullptr;
    return std::make_unique<TlsTransport>(std::move(tcp), std::move(read), std::move(write));
}

} // namespace rec::core
