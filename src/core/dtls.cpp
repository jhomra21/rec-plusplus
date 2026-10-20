#include "core/dtls.hpp"

#include "core/gcm.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace rec::core {
namespace {

using Bytes = crypto::Bytes;

constexpr std::uint8_t kChangeCipherSpec = 20, kAlert = 21, kHandshake = 22;
constexpr std::uint8_t kClientHello = 1, kServerHello = 2, kHelloVerifyRequest = 3, kCertificate = 11,
                       kServerKeyExchange = 12, kCertificateRequest = 13, kServerHelloDone = 14,
                       kCertificateVerify = 15, kClientKeyExchange = 16, kFinished = 20;
constexpr std::uint16_t kEcdheEcdsaAes128Gcm = 0xC02B, kEcdheRsaAes128Gcm = 0xC02F;
constexpr std::uint16_t kEcdsaSha256 = 0x0403, kRsaSha256 = 0x0401;
constexpr std::size_t kMaxDatagram = 1200;
constexpr auto kMaxResendWait = std::chrono::milliseconds(8000);

void put8(Bytes& b, std::uint64_t v) { b.push_back(static_cast<std::uint8_t>(v)); }
void put16(Bytes& b, std::uint64_t v) { put8(b, v >> 8); put8(b, v); }
void put24(Bytes& b, std::uint64_t v) { put8(b, v >> 16); put16(b, v); }
void put48(Bytes& b, std::uint64_t v) { put16(b, v >> 32); put16(b, v >> 16); put16(b, v); }
void append(Bytes& b, const std::uint8_t* data, std::size_t size) { b.insert(b.end(), data, data + size); }
void append(Bytes& b, const Bytes& more) { b.insert(b.end(), more.begin(), more.end()); }

struct Reader {
    const std::uint8_t* data;
    std::size_t size, at = 0;
    bool ok = true;
    Reader(const std::uint8_t* d, std::size_t n) : data(d), size(n) {}
    explicit Reader(const Bytes& b) : data(b.data()), size(b.size()) {}
    bool has(std::size_t n) const { return ok && size - at >= n; }
    std::size_t left() const { return ok ? size - at : 0; }
    std::uint32_t number(int bytes) {
        if (!has(static_cast<std::size_t>(bytes))) { ok = false; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < bytes; ++i) v = v << 8 | data[at++];
        return v;
    }
    std::uint32_t u8() { return number(1); }
    std::uint32_t u16() { return number(2); }
    std::uint32_t u24() { return number(3); }
    const std::uint8_t* take(std::size_t n) {
        if (!has(n)) { ok = false; return nullptr; }
        const auto* p = data + at;
        at += n;
        return p;
    }
    Bytes bytes(std::size_t n) {
        const auto* p = take(n);
        return p ? Bytes(p, p + n) : Bytes{};
    }
};

// DER, enough to write a self-signed certificate and find a certificate's public key.
Bytes der(std::uint8_t tag, const Bytes& body) {
    Bytes out{tag};
    const auto n = body.size();
    if (n < 0x80) put8(out, n);
    else if (n < 0x100) { put8(out, 0x81); put8(out, n); }
    else { put8(out, 0x82); put16(out, n); }
    append(out, body);
    return out;
}
Bytes der_seq(std::initializer_list<Bytes> parts) {
    Bytes body;
    for (const auto& p : parts) append(body, p);
    return der(0x30, body);
}
const Bytes kOidEcdsaSha256{0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02};
const Bytes kOidEcPublicKey{0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
const Bytes kOidPrime256v1{0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
const Bytes kOidRsa{0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01};
const Bytes kOidCommonName{0x55, 0x04, 0x03};

bool der_next(Reader& r, std::uint8_t& tag, Reader& body) {
    tag = static_cast<std::uint8_t>(r.u8());
    std::size_t length = r.u8();
    if (length & 0x80) {
        const int count = static_cast<int>(length & 0x7F);
        if (count < 1 || count > 3) return false;
        length = r.number(count);
    }
    const auto* p = r.take(length);
    if (!r.ok) return false;
    body = Reader(p, length);
    return true;
}

Bytes utc_time(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char text[16];
    std::snprintf(text, sizeof text, "%02d%02d%02d%02d%02d%02dZ", tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    return der(0x17, Bytes(text, text + 13));
}


} // namespace

std::string hex_colons(const std::uint8_t* data, std::size_t size) {
    std::string out;
    char part[4];
    for (std::size_t i = 0; i < size; ++i) {
        std::snprintf(part, sizeof part, i ? ":%02X" : "%02X", data[i]);
        out += part;
    }
    return out;
}

Bytes tls_prf(const std::uint8_t* secret, std::size_t secret_size, const std::string& label, const Bytes& seed,
              std::size_t size) {
    Bytes labelled(label.begin(), label.end());
    append(labelled, seed);
    Bytes out;
    auto a = crypto::hmac_sha256(secret, secret_size, labelled.data(), labelled.size());
    while (out.size() < size) {
        Bytes input(a.begin(), a.end());
        append(input, labelled);
        const auto chunk = crypto::hmac_sha256(secret, secret_size, input.data(), input.size());
        append(out, chunk.data(), chunk.size());
        a = crypto::hmac_sha256(secret, secret_size, a.data(), a.size());
    }
    out.resize(size);
    return out;
}

DtlsClient::DtlsClient() = default;
DtlsClient::~DtlsClient() = default;

Bytes self_signed_certificate(const crypto::P256& key) {
    Bytes serial(8);
    crypto::random(serial.data(), serial.size());
    serial[0] = (serial[0] & 0x7F) | 0x40;
    const Bytes algorithm = der_seq({der(0x06, kOidEcdsaSha256)});
    const Bytes name = der_seq({der(0x31, der_seq({der(0x06, kOidCommonName), der(0x0C, Bytes{'r', 'e', 'c'})}))});
    const std::time_t now = std::time(nullptr);
    Bytes point{0};
    append(point, key.public_point());
    const Bytes tbs = der_seq({
        der(0xA0, der(0x02, Bytes{2})),
        der(0x02, serial),
        algorithm,
        name,
        der_seq({utc_time(now - 86400), utc_time(now + 30 * 86400)}),
        name,
        der_seq({der_seq({der(0x06, kOidEcPublicKey), der(0x06, kOidPrime256v1)}), der(0x03, point)}),
    });
    Bytes signature{0};
    const Bytes signed_tbs = key.sign(tbs.data(), tbs.size());
    if (signed_tbs.empty()) return {};
    append(signature, signed_tbs);
    return der_seq({tbs, algorithm, der(0x03, signature)});
}

std::string DtlsClient::init() {
    identity_ = crypto::P256::generate(crypto::P256::Use::sign);
    if (!identity_) return "could not make a DTLS key";
    certificate_ = self_signed_certificate(*identity_);
    if (certificate_.empty()) return "could not sign the DTLS certificate";
    fingerprint_ = crypto::sha256(certificate_.data(), certificate_.size());
    if (!crypto::random(client_random_.data(), client_random_.size())) return "no random numbers";
    return {};
}

std::string DtlsClient::fingerprint() const { return hex_colons(fingerprint_.data(), fingerprint_.size()); }

bool DtlsClient::expect_fingerprint(const std::string& text) {
    std::size_t count = 0;
    int nibble = -1;
    for (const char c : text) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c == ':') continue;
        else return false;
        if (nibble < 0) nibble = v;
        else {
            if (count == expected_.size()) return false;
            expected_[count++] = static_cast<std::uint8_t>(nibble << 4 | v);
            nibble = -1;
        }
    }
    have_expected_ = count == expected_.size() && nibble < 0;
    return have_expected_;
}

void DtlsClient::fail(std::string text) {
    if (failure_.empty()) failure_ = std::move(text);
    flight_.clear();
}

void DtlsClient::start(Clock::time_point now) {
    now_ = now;
    send_client_hello();
}

Bytes DtlsClient::handshake(std::uint8_t type, const Bytes& body) {
    Bytes message;
    put8(message, type);
    put24(message, body.size());
    put16(message, send_seq_++);
    put24(message, 0);
    put24(message, body.size());
    append(message, body);
    return message;
}

void DtlsClient::send_client_hello() {
    Bytes body{0xFE, 0xFD};
    append(body, client_random_.data(), client_random_.size());
    put8(body, 0);  // No session to resume.
    put8(body, cookie_.size());
    append(body, cookie_);
    put16(body, 4);
    put16(body, kEcdheEcdsaAes128Gcm);
    put16(body, kEcdheRsaAes128Gcm);
    put8(body, 1);
    put8(body, 0);
    Bytes extensions;
    auto extension = [&](std::uint16_t type, const Bytes& data) {
        put16(extensions, type);
        put16(extensions, data.size());
        append(extensions, data);
    };
    extension(0x000A, {0x00, 0x02, 0x00, 0x17});                   // supported_groups: secp256r1
    extension(0x000B, {0x01, 0x00});                               // ec_point_formats: uncompressed
    extension(0x000D, {0x00, 0x04, 0x04, 0x03, 0x04, 0x01});       // signature_algorithms
    extension(0x000E, {0x00, 0x02, 0x00, 0x01, 0x00});             // use_srtp: AES128_CM_HMAC_SHA1_80
    extension(0x0017, {});                                         // extended_master_secret
    extension(0xFF01, {0x00});                                     // renegotiation_info
    put16(body, extensions.size());
    append(body, extensions);
    hello_ = handshake(kClientHello, body);
    flight_ = {{kHandshake, 0, hello_}};
    resend_wait_ = std::chrono::milliseconds(1000);
    send_flight();
}

Bytes DtlsClient::record(std::uint8_t type, std::uint16_t epoch, const Bytes& body) {
    const std::uint64_t seq = record_seq_[epoch]++;
    const Bytes payload = epoch ? seal(type, seq, body) : body;
    Bytes out;
    put8(out, type);
    put16(out, 0xFEFD);
    put16(out, epoch);
    put48(out, seq);
    put16(out, payload.size());
    append(out, payload);
    return out;
}

void DtlsClient::send_flight() {
    Bytes datagram;
    for (const auto& item : flight_) {
        Bytes r = record(item.type, item.epoch, item.body);
        if (!datagram.empty() && datagram.size() + r.size() > kMaxDatagram) {
            outgoing_.push_back(std::move(datagram));
            datagram.clear();
        }
        append(datagram, r);
    }
    if (!datagram.empty()) outgoing_.push_back(std::move(datagram));
    resend_at_ = now_ + resend_wait_;
}

void DtlsClient::poll(Clock::time_point now) {
    now_ = now;
    if (connected_ || flight_.empty() || now < resend_at_) return;
    resend_wait_ = std::min(resend_wait_ * 2, std::chrono::duration_cast<std::chrono::milliseconds>(kMaxResendWait));
    send_flight();
}

std::vector<DtlsClient::Bytes> DtlsClient::take_outgoing() { return std::exchange(outgoing_, {}); }

Bytes DtlsClient::close_notify() {
    if (!connected_) return {};
    return record(kAlert, 1, Bytes{1, 0});
}

void DtlsClient::receive(const std::uint8_t* data, std::size_t size, Clock::time_point now) {
    now_ = now;
    Reader r(data, size);
    while (r.left() >= 13 && failure_.empty()) {
        const auto type = static_cast<std::uint8_t>(r.u8());
        r.u16();
        const auto epoch = static_cast<std::uint16_t>(r.u16());
        const std::uint64_t high = r.u16();
        const std::uint64_t seq = high << 32 | r.number(4);
        const std::size_t length = r.u16();
        const auto* body = r.take(length);
        if (!body) return;
        handle_record(type, epoch, seq, body, length);
    }
}

void DtlsClient::handle_record(std::uint8_t type, std::uint16_t epoch, std::uint64_t seq, const std::uint8_t* data,
                               std::size_t size) {
    Bytes plain;
    if (epoch == 1) {
        if (!read_ || !open(type, epoch, seq, data, size, plain)) return;  // Not ours, or damaged: dropped.
        data = plain.data();
        size = plain.size();
    } else if (epoch != 0) {
        return;
    }
    if (type == kAlert && size >= 2) {
        if (data[1] == 0) closed_ = true;
        else if (data[0] == 2) {
            char text[80];
            std::snprintf(text, sizeof text, "the server ended the DTLS handshake (alert %u)", data[1]);
            fail(text);
        }
        return;
    }
    if (type == kHandshake) handle_fragments(data, size);
}

void DtlsClient::handle_fragments(const std::uint8_t* data, std::size_t size) {
    Reader r(data, size);
    while (r.left() >= 12 && failure_.empty()) {
        const auto type = static_cast<std::uint8_t>(r.u8());
        const std::size_t length = r.u24();
        const auto seq = static_cast<std::uint16_t>(r.u16());
        const std::size_t offset = r.u24(), count = r.u24();
        const auto* body = r.take(count);
        if (!body || offset + count > length || length > 65536) return;
        if (seq < receive_seq_) {
            // The server repeats its flight when ours was lost: answer its last message again.
            if (sent_finished_ && !connected_ && type == kServerHelloDone) send_flight();
            continue;
        }
        if (seq > receive_seq_ + 16) continue;
        auto& partial = partial_[seq];
        if (partial.have.empty()) {
            partial.type = type;
            partial.body.assign(length, 0);
            partial.have.assign(length, false);
            partial.missing = length;
        }
        if (partial.body.size() != length) continue;
        for (std::size_t i = 0; i < count; ++i) {
            if (partial.have[offset + i]) continue;
            partial.have[offset + i] = true;
            partial.body[offset + i] = body[i];
            --partial.missing;
        }
        while (failure_.empty()) {
            auto next = partial_.find(receive_seq_);
            if (next == partial_.end() || next->second.missing) break;
            const Partial done = std::move(next->second);
            partial_.erase(next);
            handle_message(done.type, receive_seq_++, done.body);
        }
    }
}

void DtlsClient::handle_message(std::uint8_t type, std::uint16_t seq, const Bytes& body) {
    auto hashed = [&] {
        Bytes message;
        put8(message, type);
        put24(message, body.size());
        put16(message, seq);
        put24(message, 0);
        put24(message, body.size());
        append(message, body);
        append(transcript_, message);
    };
    switch (type) {
    case kHelloVerifyRequest: {
        Reader r(body);
        r.u16();
        const std::size_t length = r.u8();
        cookie_ = r.bytes(length);
        if (!r.ok || cookie_.empty()) return fail("the DTLS server sent a bad cookie");
        send_client_hello();
        return;
    }
    case kServerHello:
        transcript_ = hello_;
        hashed();
        if (!server_hello(body)) return;
        flight_.clear();  // The ClientHello arrived.
        return;
    case kCertificate:
        hashed();
        certificate(body);
        return;
    case kServerKeyExchange:
        hashed();
        key_exchange(body);
        return;
    case kCertificateRequest:
        hashed();
        certificate_requested_ = true;
        return;
    case kServerHelloDone:
        hashed();
        finish_client();
        return;
    case kFinished:
        if (!sent_finished_) return fail("the DTLS server finished early");
        server_finished(body);
        return;
    default:
        return fail("unexpected DTLS handshake message " + std::to_string(type));
    }
}

bool DtlsClient::server_hello(const Bytes& body) {
    Reader r(body);
    r.u16();
    const auto* random = r.take(32);
    const std::size_t session = r.u8();
    r.take(session);
    cipher_ = static_cast<std::uint16_t>(r.u16());
    const auto compression = r.u8();
    if (!r.ok || !random) { fail("bad DTLS ServerHello"); return false; }
    std::memcpy(server_random_.data(), random, 32);
    if (cipher_ != kEcdheEcdsaAes128Gcm && cipher_ != kEcdheRsaAes128Gcm) { fail("the DTLS server chose an unsupported cipher"); return false; }
    if (compression != 0) { fail("the DTLS server chose compression"); return false; }
    if (r.left() >= 2) {
        const std::size_t total = r.u16();
        Reader extensions(r.take(total), total);
        while (extensions.left() >= 4) {
            const auto type = extensions.u16();
            const std::size_t length = extensions.u16();
            Reader data(extensions.take(length), length);
            if (!extensions.ok) break;
            if (type == 0x000E) {
                data.u16();
                srtp_ = data.u16() == 1;
            } else if (type == 0x0017) {
                extended_master_ = true;
            }
        }
    }
    return true;
}

bool DtlsClient::certificate(const Bytes& body) {
    Reader r(body);
    r.u24();
    const std::size_t length = r.u24();
    const auto* der_data = r.take(length);
    if (!der_data) { fail("the DTLS server sent no certificate"); return false; }
    const auto digest = crypto::sha256(der_data, length);
    if (!have_expected_ || digest != expected_) { fail("the DTLS certificate does not match the server's fingerprint"); return false; }
    // Certificate > TBSCertificate > [version], serial, signature, issuer, validity, subject, key.
    Reader top(der_data, length), cert(nullptr, 0), tbs(nullptr, 0), item(nullptr, 0);
    std::uint8_t tag = 0;
    if (!der_next(top, tag, cert) || !der_next(cert, tag, tbs)) { fail("bad DTLS certificate"); return false; }
    int skip = 6;
    while (skip-- > 0) {
        if (!der_next(tbs, tag, item)) { fail("bad DTLS certificate"); return false; }
        if (tag == 0xA0) ++skip;
    }
    Reader algorithm(nullptr, 0), oid(nullptr, 0), key(nullptr, 0);
    if (!der_next(item, tag, algorithm) || !der_next(algorithm, tag, oid) || !der_next(item, tag, key) || tag != 0x03 ||
        key.u8() != 0) {
        fail("bad DTLS certificate key");
        return false;
    }
    const Bytes id(oid.data, oid.data + oid.size);
    if (id == kOidEcPublicKey) {
        Reader curve(nullptr, 0);
        if (!der_next(algorithm, tag, curve) || Bytes(curve.data, curve.data + curve.size) != kOidPrime256v1) {
            fail("the DTLS certificate uses an unsupported curve");
            return false;
        }
        server_point_ = key.bytes(key.left());
    } else if (id == kOidRsa) {
        Reader sequence(nullptr, 0), n(nullptr, 0), e(nullptr, 0);
        if (!der_next(key, tag, sequence) || !der_next(sequence, tag, n) || !der_next(sequence, tag, e)) {
            fail("bad DTLS RSA key");
            return false;
        }
        server_modulus_ = n.bytes(n.left());
        server_exponent_ = e.bytes(e.left());
    } else {
        fail("the DTLS certificate has an unsupported key");
        return false;
    }
    return true;
}

bool DtlsClient::key_exchange(const Bytes& body) {
    Reader r(body);
    if (r.u8() != 3 || r.u16() != 0x0017) { fail("the DTLS server chose an unsupported curve"); return false; }
    const std::size_t length = r.u8();
    server_ecdhe_ = r.bytes(length);
    const std::size_t params = 4 + length;
    const auto algorithm = r.u16();
    const std::size_t sig_length = r.u16();
    const Bytes signature = r.bytes(sig_length);
    if (!r.ok) { fail("bad DTLS ServerKeyExchange"); return false; }
    Bytes signed_data(client_random_.begin(), client_random_.end());
    append(signed_data, server_random_.data(), server_random_.size());
    append(signed_data, body.data(), params);
    bool good = false;
    if (algorithm == kEcdsaSha256 && !server_point_.empty())
        good = crypto::verify_p256(server_point_, signed_data.data(), signed_data.size(), signature);
    else if (algorithm == kRsaSha256 && !server_modulus_.empty())
        good = crypto::verify_rsa(server_modulus_, server_exponent_, signed_data.data(), signed_data.size(), signature);
    if (!good) { fail("the DTLS server's key exchange signature is not valid"); return false; }
    return true;
}

void DtlsClient::finish_client() {
    if (server_ecdhe_.empty()) return fail("the DTLS server sent no key exchange");
    if (!srtp_) return fail("the DTLS server did not agree to SRTP");
    ephemeral_ = crypto::P256::generate(crypto::P256::Use::agree);
    if (!ephemeral_) return fail("could not make a DTLS key");
    const Bytes premaster = ephemeral_->agree(server_ecdhe_);
    if (premaster.size() != 32) return fail("the DTLS key exchange failed");

    std::vector<Item> items;
    auto add = [&](std::uint8_t type, const Bytes& message_body) {
        const Bytes message = handshake(type, message_body);
        append(transcript_, message);
        items.push_back({kHandshake, 0, message});
    };
    if (certificate_requested_) {
        Bytes list;
        put24(list, certificate_.size() + 3);
        put24(list, certificate_.size());
        append(list, certificate_);
        add(kCertificate, list);
    }
    Bytes exchange;
    put8(exchange, ephemeral_->public_point().size());
    append(exchange, ephemeral_->public_point());
    add(kClientKeyExchange, exchange);

    Bytes randoms(client_random_.begin(), client_random_.end());
    append(randoms, server_random_.data(), server_random_.size());
    Bytes master;
    if (extended_master_) {
        const auto session = crypto::sha256(transcript_.data(), transcript_.size());
        master = tls_prf(premaster.data(), premaster.size(), "extended master secret", Bytes(session.begin(), session.end()), 48);
    } else {
        master = tls_prf(premaster.data(), premaster.size(), "master secret", randoms, 48);
    }
    std::memcpy(master_.data(), master.data(), 48);

    if (certificate_requested_) {
        const Bytes signature = identity_->sign(transcript_.data(), transcript_.size());
        if (signature.empty()) return fail("could not sign the DTLS handshake");
        Bytes verify;
        put16(verify, kEcdsaSha256);
        put16(verify, signature.size());
        append(verify, signature);
        add(kCertificateVerify, verify);
    }

    Bytes server_first(server_random_.begin(), server_random_.end());
    append(server_first, client_random_.data(), client_random_.size());
    const Bytes block = tls_prf(master_.data(), master_.size(), "key expansion", server_first, 40);
    write_ = std::make_unique<crypto::Aes128>(block.data());
    read_ = std::make_unique<crypto::Aes128>(block.data() + 16);
    std::memcpy(write_iv_.data(), block.data() + 32, 4);
    std::memcpy(read_iv_.data(), block.data() + 36, 4);
    if (!write_->valid() || !read_->valid()) return fail("could not set up DTLS encryption");

    items.push_back({kChangeCipherSpec, 0, Bytes{1}});
    const auto hash = crypto::sha256(transcript_.data(), transcript_.size());
    const Bytes verify = tls_prf(master_.data(), master_.size(), "client finished", Bytes(hash.begin(), hash.end()), 12);
    const Bytes finished = handshake(kFinished, verify);
    append(transcript_, finished);
    items.push_back({kHandshake, 1, finished});
    flight_ = std::move(items);
    sent_finished_ = true;
    resend_wait_ = std::chrono::milliseconds(1000);
    send_flight();
}

bool DtlsClient::server_finished(const Bytes& body) {
    const auto hash = crypto::sha256(transcript_.data(), transcript_.size());
    const Bytes expected = tls_prf(master_.data(), master_.size(), "server finished", Bytes(hash.begin(), hash.end()), 12);
    if (body != expected) { fail("the DTLS server's Finished message is wrong"); return false; }
    Bytes randoms(client_random_.begin(), client_random_.end());
    append(randoms, server_random_.data(), server_random_.size());
    const Bytes material = tls_prf(master_.data(), master_.size(), "EXTRACTOR-dtls_srtp", randoms, 60);
    std::memcpy(keys_.local_key.data(), material.data(), 16);
    std::memcpy(keys_.remote_key.data(), material.data() + 16, 16);
    std::memcpy(keys_.local_salt.data(), material.data() + 32, 14);
    std::memcpy(keys_.remote_salt.data(), material.data() + 46, 14);
    flight_.clear();
    connected_ = true;
    return true;
}

Bytes DtlsClient::seal(std::uint8_t type, std::uint64_t seq, const Bytes& plain) {
    Bytes explicit_nonce;
    put16(explicit_nonce, 1);
    put48(explicit_nonce, seq);
    std::uint8_t iv[12];
    std::memcpy(iv, write_iv_.data(), 4);
    std::memcpy(iv + 4, explicit_nonce.data(), 8);
    Bytes aad = explicit_nonce;
    put8(aad, type);
    put16(aad, 0xFEFD);
    put16(aad, plain.size());
    Bytes out = explicit_nonce;
    out.resize(8 + plain.size() + 16);
    gcm::seal(*write_, iv, aad, plain.data(), out.data() + 8, plain.size());
    return out;
}

bool DtlsClient::open(std::uint8_t type, std::uint16_t epoch, std::uint64_t seq, const std::uint8_t* data,
                      std::size_t size, Bytes& plain) {
    if (size < 24) return false;
    const std::size_t length = size - 24;
    std::uint8_t iv[12];
    std::memcpy(iv, read_iv_.data(), 4);
    std::memcpy(iv + 4, data, 8);
    Bytes aad;
    put16(aad, epoch);
    put48(aad, seq);
    put8(aad, type);
    put16(aad, 0xFEFD);
    put16(aad, length);
    plain.resize(length);
    return gcm::open(*read_, iv, aad, data + 8, plain.data(), length);
}

} // namespace rec::core
