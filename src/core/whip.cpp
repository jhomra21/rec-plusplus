#include "core/whip.hpp"

#include "core/stun.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace rec::core {
namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr int kConnectMs = 15000;
constexpr auto kSilence = std::chrono::seconds(5);
constexpr auto kReconnectFor = std::chrono::minutes(2);
constexpr auto kConsentEvery = std::chrono::seconds(2);
constexpr auto kReportEvery = std::chrono::seconds(1);
constexpr auto kSendMargin = milliseconds(40);
constexpr std::uint32_t kQueueMs = 2000;
constexpr std::size_t kMaxQueue = 4000;
constexpr std::size_t kMaxPayload = 1200;
constexpr std::size_t kHistory = 1024;  // Sent video packets kept for retransmission on NACK.

// --- URL and HTTP ---------------------------------------------------------------------

struct Url {
    bool secure = false;
    std::string host, port, path, token;
};

std::string parse_url(const std::string& text, Url& out) {
    out = {};
    std::string rest;
    if (text.rfind("https://", 0) == 0) { out.secure = true; out.port = "443"; rest = text.substr(8); }
    else if (text.rfind("http://", 0) == 0) { out.port = "80"; rest = text.substr(7); }
    else return "WHIP URL must start with http:// or https://";
    if (const auto hash = rest.find('#'); hash != std::string::npos) {
        out.token = rest.substr(hash + 1);
        rest.resize(hash);
    }
    const auto slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (const auto at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
    if (!authority.empty() && authority[0] == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) return "bad IPv6 host in the WHIP URL";
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') out.port = authority.substr(close + 2);
    } else if (const auto colon = authority.find(':'); colon != std::string::npos) {
        out.host = authority.substr(0, colon);
        out.port = authority.substr(colon + 1);
    } else {
        out.host = authority;
    }
    if (out.host.empty()) return "the WHIP URL has no host";
    if (out.port.empty() || out.port.find_first_not_of("0123456789") != std::string::npos) return "bad port in the WHIP URL";
    return {};
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Response {
    int status = 0;
    std::map<std::string, std::string> headers;  // Lowercase names.
    std::string body;
};

std::string http(const Url& url, const std::string& method, const std::string& path, const std::string& body,
                 Response& out, int timeout_ms) {
    std::string error;
    auto transport = url.secure ? connect_tls(url.host, url.port, timeout_ms, error)
                                : connect_tcp(url.host, url.port, timeout_ms, error);
    if (!transport) return error;
    const bool ipv6 = url.host.find(':') != std::string::npos;
    std::string host = ipv6 ? "[" + url.host + "]" : url.host;
    if (url.port != (url.secure ? "443" : "80")) host += ":" + url.port;
    std::string request = method + " " + path + " HTTP/1.1\r\nHost: " + host +
                          "\r\nUser-Agent: rec\r\nConnection: close\r\n";
    if (!url.token.empty()) request += "Authorization: Bearer " + url.token + "\r\n";
    if (method == "POST") request += "Content-Type: application/sdp\r\nAccept: application/sdp\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    if (!transport->send(reinterpret_cast<const std::uint8_t*>(request.data()), request.size()))
        return "could not send the WHIP request";
    std::string response;
    std::uint8_t buffer[4096];
    std::size_t header_end = std::string::npos, length = std::string::npos;
    for (;;) {
        const std::size_t got = transport->receive(buffer, sizeof buffer);
        if (!got) break;
        response.append(reinterpret_cast<const char*>(buffer), got);
        if (header_end == std::string::npos && (header_end = response.find("\r\n\r\n")) != std::string::npos) {
            const auto headers = lower(response.substr(0, header_end));
            if (const auto at = headers.find("\r\ncontent-length:"); at != std::string::npos)
                length = std::strtoul(headers.c_str() + at + 17, nullptr, 10);
        }
        if (header_end != std::string::npos && length != std::string::npos && response.size() >= header_end + 4 + length)
            break;
        if (response.size() > (1 << 20)) return "the WHIP response is too large";
    }
    if (header_end == std::string::npos) return "the WHIP server closed the connection without answering";
    out = {};
    std::size_t line_end = response.find("\r\n");
    const std::string status_line = response.substr(0, line_end);
    if (status_line.size() < 12 || status_line.rfind("HTTP/", 0) != 0) return "the WHIP server sent no HTTP response";
    out.status = std::atoi(status_line.c_str() + status_line.find(' ') + 1);
    std::size_t at = line_end + 2;
    while (at < header_end) {
        const auto end = response.find("\r\n", at);
        const std::string line = response.substr(at, end - at);
        at = end + 2;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        out.headers[lower(line.substr(0, colon))] = value;
    }
    std::string content = response.substr(header_end + 4);
    if (lower(out.headers["transfer-encoding"]).find("chunked") != std::string::npos) {
        std::string joined;
        std::size_t p = 0;
        while (p < content.size()) {
            const auto eol = content.find("\r\n", p);
            if (eol == std::string::npos) break;
            const std::size_t size = std::strtoul(content.c_str() + p, nullptr, 16);
            if (!size || eol + 2 + size > content.size()) break;
            joined += content.substr(eol + 2, size);
            p = eol + 2 + size + 2;
        }
        content = std::move(joined);
    } else if (length != std::string::npos && content.size() > length) {
        content.resize(length);
    }
    out.body = std::move(content);
    return {};
}

// --- STUN (RFC 8489, ICE attributes from RFC 8445) -----------------------------------

void put16(Bytes& b, std::uint32_t v) { b.push_back(static_cast<std::uint8_t>(v >> 8)); b.push_back(static_cast<std::uint8_t>(v)); }
void put32(Bytes& b, std::uint32_t v) { put16(b, v >> 16); put16(b, v); }
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint32_t get32(const std::uint8_t* p) { return static_cast<std::uint32_t>(get16(p)) << 16 | get16(p + 2); }

// --- SDP ------------------------------------------------------------------------------

struct Answer {
    std::string ufrag, pwd, fingerprint, setup;
    std::vector<Endpoint> candidates;
    int video_pt = -1, audio_pt = -1;
    bool video_rejected = false, audio_rejected = false;
};

std::string random_text(std::size_t size) {
    static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::vector<std::uint8_t> bytes(size);
    crypto::random(bytes.data(), size);
    std::string out;
    for (auto b : bytes) out += kAlphabet[b % 62];
    return out;
}

std::uint32_t random32() {
    std::uint8_t b[4];
    crypto::random(b, 4);
    return get32(b);
}

std::string parse_answer(const std::string& sdp, Answer& out) {
    out = {};
    enum { kSession, kVideo, kAudio, kOther } section = kSession;
    std::map<int, bool> video_mode1;  // H.264 payload types: packetization-mode=1?
    std::vector<int> h264;
    std::size_t at = 0;
    Endpoint fallback;
    bool have_fallback = false;
    while (at < sdp.size()) {
        auto end = sdp.find('\n', at);
        if (end == std::string::npos) end = sdp.size();
        std::string line = sdp.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("m=", 0) == 0) {
            const bool video = line.rfind("m=video", 0) == 0, audio = line.rfind("m=audio", 0) == 0;
            section = video ? kVideo : audio ? kAudio : kOther;
            const auto space = line.find(' ');
            const bool rejected = space != std::string::npos && std::atoi(line.c_str() + space + 1) == 0;
            if (video) out.video_rejected = rejected;
            if (audio) out.audio_rejected = rejected;
            if (!rejected && !have_fallback && space != std::string::npos) fallback.port = static_cast<std::uint16_t>(std::atoi(line.c_str() + space + 1));
            continue;
        }
        if (line.rfind("c=IN IP", 0) == 0 && line.size() > 9) {
            Endpoint e;
            if (parse_endpoint(line.substr(9), 0, e) && e.address != std::array<std::uint8_t, 16>{}) {
                const auto port = fallback.port;
                fallback = e;
                fallback.port = port;
                have_fallback = true;
            }
            continue;
        }
        if (line.rfind("a=", 0) != 0) continue;
        const auto colon = line.find(':');
        const std::string name = line.substr(2, colon == std::string::npos ? std::string::npos : colon - 2);
        const std::string value = colon == std::string::npos ? "" : line.substr(colon + 1);
        if (name == "ice-ufrag" && out.ufrag.empty()) out.ufrag = value;
        else if (name == "ice-pwd" && out.pwd.empty()) out.pwd = value;
        else if (name == "fingerprint" && out.fingerprint.empty() && lower(value).rfind("sha-256 ", 0) == 0) out.fingerprint = value.substr(8);
        else if (name == "setup" && out.setup.empty()) out.setup = value;
        else if (name == "rtpmap") {
            const int pt = std::atoi(value.c_str());
            const std::string codec = lower(value.substr(value.find(' ') + 1));
            if (section == kVideo && codec.rfind("h264/90000", 0) == 0) h264.push_back(pt);
            if (section == kAudio && codec.rfind("opus/48000", 0) == 0 && out.audio_pt < 0) out.audio_pt = pt;
        } else if (name == "fmtp" && section == kVideo) {
            video_mode1[std::atoi(value.c_str())] = value.find("packetization-mode=1") != std::string::npos;
        } else if (name == "candidate") {
            // foundation component transport priority address port typ type ...
            char transport[16] = {}, address[64] = {};
            unsigned component = 0, port = 0;
            unsigned long priority = 0;
            if (std::sscanf(value.c_str(), "%*s %u %15s %lu %63s %u", &component, transport, &priority, address, &port) == 5 &&
                component == 1 && lower(transport) == "udp") {
                Endpoint e;
                if (parse_endpoint(address, static_cast<std::uint16_t>(port), e) &&
                    std::find(out.candidates.begin(), out.candidates.end(), e) == out.candidates.end())
                    out.candidates.push_back(e);
            }
        }
    }
    for (const int pt : h264)
        if (out.video_pt < 0 || (video_mode1[pt] && !video_mode1[out.video_pt])) out.video_pt = pt;
    if (out.candidates.empty() && have_fallback && fallback.port) out.candidates.push_back(fallback);
    if (out.video_rejected || out.video_pt < 0) return "the WHIP server does not accept H.264 video";
    if (out.audio_rejected || out.audio_pt < 0) return "the WHIP server does not accept Opus audio";
    if (out.ufrag.empty() || out.pwd.empty()) return "the WHIP answer has no ICE credentials";
    if (out.fingerprint.empty()) return "the WHIP answer has no DTLS fingerprint";
    if (out.setup == "active") return "the WHIP server answered as the DTLS client too";
    if (out.candidates.empty()) return "the WHIP answer has no UDP address to send to";
    return {};
}

Bytes avc_parameter_sets_to(const Bytes& record, Bytes& sps, Bytes& pps) {
    // AVCDecoderConfigurationRecord: version, profile, compatibility, level, length size,
    // SPS count, then each SPS and the PPS count and each PPS, all with 16-bit lengths.
    if (record.size() < 8) return {};
    std::size_t at = 5;
    const int sps_count = record[at++] & 0x1F;
    for (int i = 0; i < sps_count && at + 2 <= record.size(); ++i) {
        const std::size_t n = get16(record.data() + at);
        at += 2;
        if (at + n > record.size()) return {};
        if (i == 0) sps.assign(record.begin() + static_cast<std::ptrdiff_t>(at), record.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    }
    if (at >= record.size()) return {};
    const int pps_count = record[at++];
    for (int i = 0; i < pps_count && at + 2 <= record.size(); ++i) {
        const std::size_t n = get16(record.data() + at);
        at += 2;
        if (at + n > record.size()) return {};
        if (i == 0) pps.assign(record.begin() + static_cast<std::ptrdiff_t>(at), record.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    }
    return sps;
}

std::uint64_t ntp_now() {
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(since).count();
    const std::uint64_t seconds = static_cast<std::uint64_t>(micros / 1000000) + 2208988800ULL;
    const std::uint64_t fraction = (static_cast<std::uint64_t>(micros % 1000000) << 32) / 1000000;
    return seconds << 32 | fraction;
}
} // namespace

struct WhipPublisher::Session {
    std::unique_ptr<UdpSocket> socket;
    Endpoint remote;
    std::string local_ufrag, local_pwd, remote_ufrag, remote_pwd;
    std::array<std::uint8_t, 8> tiebreak{};
    DtlsClient dtls;
    std::unique_ptr<Srtp> out, in;
    std::uint8_t video_pt = 96, audio_pt = 111;
    std::uint32_t video_ssrc = 0, audio_ssrc = 0;
    std::uint16_t video_seq = 0, audio_seq = 0;
    std::uint32_t video_base = 0, audio_base = 0;
    std::uint32_t video_packets = 0, video_octets = 0, audio_packets = 0, audio_octets = 0;
    std::vector<Bytes> history = std::vector<Bytes>(kHistory);
    Url resource;
    bool has_resource = false;
    Clock::time_point last_rx{}, next_consent{}, next_report{};
    // Pacing: media time t0_ms leaves at `at`. Send one audio packet at or before the opening
    // keyframe first so receivers that establish their mux timeline from first-packet arrival
    // cannot place the keyframe after the recording start.
    bool anchored = false, opening_audio_sent = false;
    bool video_keyframe_sent = false;
    std::uint32_t t0_ms = 0;
    std::int64_t ref_sample = 0;
    Clock::time_point at{};

    std::uint32_t video_rtp(double ms) const {
        return video_base + static_cast<std::uint32_t>(static_cast<std::int64_t>((ms * 48.0 - static_cast<double>(ref_sample)) * 90.0 / 48.0));
    }
    std::uint32_t audio_rtp(std::int64_t sample) const {
        return audio_base + static_cast<std::uint32_t>(sample - ref_sample);
    }
    bool send(const std::uint8_t* data, std::size_t size) { return socket->send(remote, data, size); }
    bool send(const Bytes& data) { return send(data.data(), data.size()); }
    void consent() {
        std::uint8_t txid[12];
        crypto::random(txid, 12);
        stun::Message m(0x0001, txid);
        m.attribute(0x0006, remote_ufrag + ":" + local_ufrag);
        m.attribute(0x802A, tiebreak.data(), 8);
        const std::uint8_t priority[4] = {0x6E, 0xFF, 0xFF, 0xFF};
        m.attribute(0x0024, priority, 4);
        m.attribute(0x0025, nullptr, 0);
        send(m.finish(remote_pwd));
    }
};

WhipPublisher::WhipPublisher() = default;
WhipPublisher::~WhipPublisher() { close(0); }

bool WhipPublisher::is_whip(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

std::string WhipPublisher::redact(const std::string& url) {
    Url parsed;
    if (!parse_url(url, parsed).empty()) return "(invalid WHIP URL)";
    const bool ipv6 = parsed.host.find(':') != std::string::npos;
    return std::string(parsed.secure ? "https://" : "http://") + (ipv6 ? "[" + parsed.host + "]" : parsed.host) + ":" +
           parsed.port + "/****";
}

std::string WhipPublisher::open(const std::string& url, const StreamInfo& info) {
    Url parsed;
    if (auto error = parse_url(url, parsed); !error.empty()) return error;
    url_ = url;
    info_ = info;
    if (auto error = connect(kConnectMs); !error.empty()) return error;
    live_.store(true);
    sender_ = std::thread([this] { run(); });
    return {};
}

std::string WhipPublisher::connect(int timeout_ms) {
    Url url;
    parse_url(url_, url);
    auto s = std::make_unique<Session>();
    if (auto error = s->dtls.init(); !error.empty()) return error;
    std::string error;
    s->socket = UdpSocket::toward(url.host, error);
    if (!s->socket) return error;
    s->local_ufrag = random_text(8);
    s->local_pwd = random_text(24);
    crypto::random(s->tiebreak.data(), 8);
    s->video_ssrc = random32();
    s->audio_ssrc = random32();
    s->video_seq = static_cast<std::uint16_t>(random32());
    s->audio_seq = static_cast<std::uint16_t>(random32());
    s->video_base = random32();
    s->audio_base = random32();

    const auto& local = s->socket->local();
    const std::string ip = endpoint_ip(local), family = local.v6 ? "IP6" : "IP4";
    // rec only plays the DTLS client, so the offer says setup:active, which WHIP (RFC 9725
    // section 4.2) allows in place of actpass for such clients.
    const std::string candidate = "a=candidate:1 1 udp 2130706431 " + ip + " " + std::to_string(local.port) + " typ host\r\n";
    auto media = [&](const std::string& kind, int pt, const std::string& codec, const std::string& extra,
                     const std::string& mid, std::uint32_t ssrc) {
        return "m=" + kind + " 9 UDP/TLS/RTP/SAVPF " + std::to_string(pt) + "\r\n" +
               "c=IN " + family + " " + (local.v6 ? "::" : "0.0.0.0") + "\r\n" +
               "a=rtcp:9 IN " + family + " " + (local.v6 ? "::" : "0.0.0.0") + "\r\n" +
               "a=ice-ufrag:" + s->local_ufrag + "\r\na=ice-pwd:" + s->local_pwd + "\r\n" +
               "a=fingerprint:sha-256 " + s->dtls.fingerprint() + "\r\na=setup:active\r\n" +
               "a=mid:" + mid + "\r\na=sendonly\r\na=msid:rec " + kind + "\r\na=rtcp-mux\r\n" +
               "a=rtpmap:" + std::to_string(pt) + " " + codec + "\r\n" + extra +
               "a=ssrc:" + std::to_string(ssrc) + " cname:rec\r\n" + candidate + "a=end-of-candidates\r\n";
    };
    const std::string offer =
        "v=0\r\no=- " + std::to_string(random32()) + " 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n" +
        "a=group:BUNDLE 0 1\r\na=msid-semantic: WMS rec\r\n" +
        media("video", 96, "H264/90000",
              "a=rtcp-fb:96 nack\r\na=rtcp-fb:96 nack pli\r\na=rtcp-fb:96 ccm fir\r\n"
              "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n",
              "0", s->video_ssrc) +
        media("audio", 111, "opus/48000/2", "a=fmtp:111 minptime=10;useinbandfec=1\r\n", "1", s->audio_ssrc);

    Response response;
    Url endpoint = url;
    for (int hop = 0;; ++hop) {
        if (auto failed = http(endpoint, "POST", endpoint.path, offer, response, std::min(timeout_ms, 10000)); !failed.empty())
            return failed;
        if ((response.status != 307 && response.status != 308) || hop == 2 || response.headers["location"].empty()) break;
        const auto& location = response.headers["location"];
        if (location.find("://") != std::string::npos) {
            const auto token = endpoint.token;
            if (!parse_url(location, endpoint).empty()) return "the WHIP server redirected to a bad URL";
            endpoint.token = token;
        } else {
            endpoint.path = location;
        }
    }
    if (response.status == 401 || response.status == 403)
        return "the WHIP server refused the stream key (HTTP " + std::to_string(response.status) + ")";
    if (response.status != 200 && response.status != 201) {
        auto first = response.body.substr(0, response.body.find('\n'));
        if (first.size() > 160) first.resize(160);
        return "the WHIP server answered HTTP " + std::to_string(response.status) + (first.empty() ? "" : ": " + first);
    }
    if (const auto& location = response.headers["location"]; !location.empty()) {
        s->resource = endpoint;
        if (location.find("://") != std::string::npos) {
            const auto token = endpoint.token;
            s->has_resource = parse_url(location, s->resource).empty();
            s->resource.token = token;
        } else {
            s->resource.path = location[0] == '/' ? location : "/" + location;
            s->has_resource = true;
        }
    }
    Answer answer;
    if (auto failed = parse_answer(response.body, answer); !failed.empty()) {
        if (s->has_resource) { Response ignored; http(s->resource, "DELETE", s->resource.path, {}, ignored, 3000); }
        return failed;
    }
    s->remote_ufrag = answer.ufrag;
    s->remote_pwd = answer.pwd;
    s->video_pt = static_cast<std::uint8_t>(answer.video_pt);
    s->audio_pt = static_cast<std::uint8_t>(answer.audio_pt);
    if (!s->dtls.expect_fingerprint(answer.fingerprint)) return "the WHIP answer has a bad DTLS fingerprint";

    // ICE: check every candidate of the server's family until one answers, then DTLS on it.
    std::vector<Endpoint> candidates;
    for (const auto& c : answer.candidates)
        if (c.v6 == local.v6) candidates.push_back(c);
    const auto deadline = Clock::now() + milliseconds(timeout_ms);
    std::map<std::string, std::size_t> checks;  // Transaction ID to candidate.
    bool selected = false;
    auto next_check = Clock::now();
    std::uint8_t buffer[2048];
    std::string result;
    while (result.empty()) {
        const auto now = Clock::now();
        if (now > deadline) {
            result = candidates.empty() ? "the WHIP server offered no address rec can reach"
                     : selected        ? "the WebRTC connection timed out (DTLS)"
                                       : "the WebRTC connection timed out (ICE)";
            break;
        }
        if (!selected && now >= next_check) {
            next_check = now + milliseconds(50);
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                std::uint8_t txid[12];
                crypto::random(txid, 12);
                checks[std::string(reinterpret_cast<char*>(txid), 12)] = i;
                stun::Message m(0x0001, txid);
                m.attribute(0x0006, s->remote_ufrag + ":" + s->local_ufrag);
                m.attribute(0x802A, s->tiebreak.data(), 8);
                const std::uint8_t priority[4] = {0x6E, 0xFF, 0xFF, 0xFF};
                m.attribute(0x0024, priority, 4);
                m.attribute(0x0025, nullptr, 0);
                const Bytes request = m.finish(s->remote_pwd);
                s->socket->send(candidates[i], request.data(), request.size());
            }
        }
        if (selected) {
            s->dtls.poll(now);
            for (const auto& d : s->dtls.take_outgoing()) s->send(d);
            if (!s->dtls.failure().empty()) { result = s->dtls.failure(); break; }
            if (s->dtls.connected()) break;
        }
        Endpoint from;
        const std::size_t got = s->socket->receive(buffer, sizeof buffer, 10, from);
        if (!got) continue;
        if (buffer[0] < 4 && got >= 20 && get32(buffer + 4) == stun::kMagic) {
            const auto type = get16(buffer);
            if (type == 0x0101 && !selected) {
                const auto found = checks.find(std::string(reinterpret_cast<char*>(buffer + 8), 12));
                if (found != checks.end()) {
                    s->remote = candidates[found->second];
                    selected = true;
                    s->dtls.start(Clock::now());
                    for (const auto& d : s->dtls.take_outgoing()) s->send(d);
                }
            } else if (type == 0x0001) {
                // A full-ICE server checks back. Answer, and check its address too: it may be
                // one its answer did not list (peer reflexive).
                handle_datagram(*s, buffer, got, from);
                if (!selected && std::find(candidates.begin(), candidates.end(), from) == candidates.end())
                    candidates.push_back(from);
            }
        } else if (selected && buffer[0] >= 20 && buffer[0] <= 63) {
            s->dtls.receive(buffer, got, Clock::now());
            for (const auto& d : s->dtls.take_outgoing()) s->send(d);
            if (!s->dtls.failure().empty()) result = s->dtls.failure();
            else if (s->dtls.connected()) break;
        }
    }
    if (!result.empty()) {
        if (s->has_resource) { Response ignored; http(s->resource, "DELETE", s->resource.path, {}, ignored, 3000); }
        return result;
    }
    const auto& keys = s->dtls.keys();
    s->out = std::make_unique<Srtp>(keys.local_key.data(), keys.local_salt.data());
    s->in = std::make_unique<Srtp>(keys.remote_key.data(), keys.remote_salt.data());
    if (!s->out->valid() || !s->in->valid()) return "could not set up SRTP";
    s->last_rx = Clock::now();
    s->next_consent = s->last_rx + kConsentEvery;
    s->next_report = s->last_rx + kReportEvery;
    {
        std::lock_guard lock(mutex_);
        queue_.clear();
        need_key_ = true;
    }
    session_ = std::move(s);
    return {};
}

bool WhipPublisher::video(std::uint32_t ms, bool key, bool header, const Bytes& body) {
    std::lock_guard lock(mutex_);
    if (header) {
        if (body.size() > 5) {
            const Bytes record(body.begin() + 5, body.end());
            avc_parameter_sets_to(record, sps_, pps_);
        }
        return true;
    }
    if (!live_.load() || closing_ || body.size() < 5) {
        need_key_ = true;
        return false;
    }
    if (need_key_ && !key) {
        // Waiting for a keyframe after a drop or a reconnect: ask the encoder for one.
        keyframe_wanted_.store(true);
        dropped_.fetch_add(1);
        return false;
    }
    // Behind by more than the queue allows: drop the queued video and restart at a keyframe.
    auto oldest = std::find_if(queue_.begin(), queue_.end(), [](const Media& m) { return m.video; });
    if (!key && oldest != queue_.end() && ms - oldest->ms > kQueueMs) {
        const auto before = queue_.size();
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const Media& m) { return m.video; }), queue_.end());
        dropped_.fetch_add(before - queue_.size() + 1);
        need_key_ = true;
        return false;
    }
    need_key_ = false;
    // Composition time offset (signed 24-bit) gives the presentation time.
    std::int32_t cts = static_cast<std::int32_t>(body[2] << 16 | body[3] << 8 | body[4]);
    if (cts & 0x800000) cts -= 0x1000000;
    Media m;
    m.video = true;
    m.key = key;
    m.ms = static_cast<std::uint32_t>(static_cast<std::int64_t>(ms) + cts);
    m.data.assign(body.begin() + 5, body.end());
    queue_.push_back(std::move(m));
    // MediaMTX sends PLI every 2 s whether or not it lost anything.
    if (key) last_key_ns_.store(Clock::now().time_since_epoch().count());
    return true;
}

bool WhipPublisher::take_keyframe_request() {
    const auto now = Clock::now().time_since_epoch().count();
    const auto second = std::chrono::duration_cast<Clock::duration>(milliseconds(1000)).count();
    // FIR and a failed send mean the receiver really needs a refresh; limit them to one per second.
    if (refresh_wanted_.load() && now - last_forced_ns_.load() >= second) {
        refresh_wanted_.store(false);
        keyframe_wanted_.store(false);
        last_forced_ns_.store(now);
        forced_keyframes_.fetch_add(1);
        return true;
    }
    if (!keyframe_wanted_.exchange(false)) return false;
    // Some servers (MediaMTX) send PLI every 2 s whether or not they lost anything. A keyframe
    // less than a second ago answers that; the regular one comes within 2 s.
    if (now - last_key_ns_.load() < second) return false;
    last_forced_ns_.store(now);
    forced_keyframes_.fetch_add(1);
    return true;
}

bool WhipPublisher::audio(std::int64_t sample, Bytes packet) {
    // Retain audio while the opening keyframe is sent. take_due() trims only samples
    // captured before that keyframe and sends the keyframe first.
    std::lock_guard lock(mutex_);
    if (!live_.load() || closing_ || queue_.size() > kMaxQueue) return false;
    Media m;
    m.sample = sample;
    m.ms = static_cast<std::uint32_t>(sample / 48);
    m.data = std::move(packet);
    queue_.push_back(std::move(m));
    return true;
}

std::uint32_t WhipPublisher::queued_ms() const {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) return 0;
    std::uint32_t low = UINT32_MAX, high = 0;
    for (const auto& m : queue_) { low = std::min(low, m.ms); high = std::max(high, m.ms); }
    return high - low;
}

std::string WhipPublisher::failure() const {
    std::lock_guard lock(mutex_);
    return failure_;
}

void WhipPublisher::fail(std::string text) {
    std::lock_guard lock(mutex_);
    if (failure_.empty()) failure_ = std::move(text);
}

void WhipPublisher::close(int flush_ms) {
    {
        std::lock_guard lock(mutex_);
        if (closing_ && !sender_.joinable()) return;
        closing_ = true;
        flush_deadline_ = Clock::now() + milliseconds(flush_ms);
    }
    wake_.notify_all();
    if (sender_.joinable()) sender_.join();
    else if (session_) hang_up(*session_, true);
    session_.reset();
    live_.store(false);
}

void WhipPublisher::run() {
    for (;;) {
        if (serve(*session_)) return;
        live_.store(false);
        reconnecting_.store(true);
        hang_up(*session_, false);
        session_.reset();
        const auto give_up = Clock::now() + kReconnectFor;
        // The same waits as RTMP: 0.5 s, 1 s, then every 2 s.
        milliseconds wait(500);
        std::string error = "the connection was lost";
        bool back = false;
        while (Clock::now() < give_up) {
            {
                std::unique_lock lock(mutex_);
                if (wake_.wait_for(lock, wait, [this] { return closing_; })) break;
            }
            error = connect(10000);
            if (error.empty()) { back = true; break; }
            wait = std::min(wait * 2, milliseconds(2000));
        }
        reconnecting_.store(false);
        if (!back) {
            std::lock_guard lock(mutex_);
            if (!closing_) failure_ = "stream ended: " + error;
            return;
        }
        reconnects_.fetch_add(1);
        live_.store(true);
    }
}

bool WhipPublisher::take_due(Session& s, Clock::time_point now, Media& item, Clock::time_point& next) {
    next = now + milliseconds(5);
    if (!s.anchored) {
        const auto key = std::find_if(queue_.begin(), queue_.end(), [](const Media& m) { return m.video; });
        if (key == queue_.end()) {
            if (!queue_.empty()) {
                const auto newest = queue_.back().ms;
                queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Media& m) { return newest - m.ms > 1000; }),
                             queue_.end());
            }
            return false;
        }
        const std::uint32_t tk = key->ms;
        const std::int64_t key_sample = static_cast<std::int64_t>(tk) * 48;
        std::uint32_t newest_audio = 0, newest_video = tk;
        const Media* best_before = nullptr;
        const Media* best_after = nullptr;
        for (const auto& m : queue_) {
            if (m.video) { newest_video = std::max(newest_video, m.ms); continue; }
            newest_audio = std::max(newest_audio, m.ms);
            if (m.sample <= key_sample && key_sample - m.sample < 20 * 48 &&
                (!best_before || m.sample > best_before->sample)) best_before = &m;
            if (m.sample > key_sample && m.sample - key_sample < 20 * 48 &&
                (!best_after || m.sample - key_sample < best_after->sample - key_sample)) best_after = &m;
        }
        // Wait for the audio around the keyframe (it may still be encoding), but not for
        // audio that never comes.
        if (newest_audio < tk + 20 && newest_video - tk < 500) return false;
        // MediaMTX's fMP4 writer buffers each track's first sample until its next packet sets
        // the duration. If the first audio timestamp is later than the keyframe, its sample can
        // establish a later segment start before the opening IDR is written. Prefer the latest
        // Opus sample at or before the IDR; if none is queued, use the nearest later one.
        const Media* first_audio = best_before ? best_before : best_after;
        const std::int64_t ref = first_audio ? first_audio->sample : key_sample;
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                    [&](const Media& m) { return m.video ? m.ms < tk : m.sample < ref; }),
                     queue_.end());
        s.ref_sample = ref;
        s.t0_ms = tk;
        s.at = now + kSendMargin;
        s.anchored = true;
        s.opening_audio_sent = false;
    }
    for (;;) {
        if (queue_.empty()) return false;
        std::size_t pick = queue_.size();
        if (!s.video_keyframe_sent) {
            // Establish the receiver's timeline with one opening audio packet, then the IDR.
            // An earlier audio packet is intentionally sent before the pacing origin.
            if (!s.opening_audio_sent) {
                const auto audio = std::find_if(queue_.begin(), queue_.end(), [&](const Media& m) {
                    return !m.video && m.sample == s.ref_sample;
                });
                if (audio != queue_.end()) {
                    // Preserve the captured lead and add a small delivery margin. RTP from
                    // separate SSRCs is read on separate receiver goroutines, so merely
                    // sending the audio datagram a few instructions before the IDR does not
                    // guarantee the receiver processes it first.
                    const auto lead_ms = audio->ms < s.t0_ms ? s.t0_ms - audio->ms : 0;
                    const auto audio_due = s.at - milliseconds(lead_ms + 5);
                    if (audio_due > now) {
                        next = audio_due;
                        return false;
                    }
                    item = std::move(*audio);
                    queue_.erase(audio);
                    s.opening_audio_sent = true;
                    return true;
                }
            }
            const auto key = std::find_if(queue_.begin(), queue_.end(),
                                          [](const Media& m) { return m.video && m.key; });
            if (key != queue_.end()) pick = static_cast<std::size_t>(key - queue_.begin());
        } else {
            const std::size_t span = std::min<std::size_t>(queue_.size(), 64);
            pick = 0;
            for (std::size_t i = 1; i < span; ++i)
                if (queue_[i].ms < queue_[pick].ms) pick = i;
        }
        if (pick == queue_.size()) return false;
        const auto& m = queue_[static_cast<std::ptrdiff_t>(pick)];
        if (m.ms < s.t0_ms && (m.video || m.sample != s.ref_sample)) {
            queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(pick));
            continue;
        }
        const auto due = s.at + milliseconds(m.ms - s.t0_ms);
        if (due > now) { next = due; return false; }
        item = std::move(queue_[static_cast<std::ptrdiff_t>(pick)]);
        queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(pick));
        return true;
    }
}

namespace {
Bytes rtp_header(std::uint8_t pt, bool marker, std::uint16_t seq, std::uint32_t ts, std::uint32_t ssrc) {
    Bytes p;
    p.reserve(12 + kMaxPayload + 16);
    p.push_back(0x80);
    p.push_back(static_cast<std::uint8_t>((marker ? 0x80 : 0) | pt));
    put16(p, seq);
    put32(p, ts);
    put32(p, ssrc);
    return p;
}
} // namespace

void WhipPublisher::send_video(Session& s, const Media& item) {
    const std::uint32_t packets_before = s.video_packets;
    Bytes sps, pps;
    {
        std::lock_guard lock(mutex_);
        sps = sps_;
        pps = pps_;
    }
    std::vector<std::pair<const std::uint8_t*, std::size_t>> nals;
    if (item.key && !sps.empty() && !pps.empty()) {
        nals.emplace_back(sps.data(), sps.size());
        nals.emplace_back(pps.data(), pps.size());
    }
    for (std::size_t at = 0; at + 4 <= item.data.size();) {
        const std::size_t n = get32(item.data.data() + at);
        at += 4;
        if (!n || at + n > item.data.size()) break;
        const std::uint8_t type = item.data[at] & 0x1F;
        if (type != 9 && !(item.key && !sps.empty() && (type == 7 || type == 8))) nals.emplace_back(item.data.data() + at, n);
        at += n;
    }
    const std::uint32_t ts = s.video_rtp(item.ms);
    bool sent_all = true;
    std::size_t packets = 0, paced_ms = 0;
    auto pacing_deadline = Clock::now();
    auto emit = [&](Bytes packet) {
        const std::size_t payload = packet.size() - 12;
        if (!s.out->protect_rtp(packet)) {
            sent_all = false;
            return;
        }
        if (!s.send(packet)) {
            sent_all = false;
            send_failures_.fetch_add(1);
            return;
        }
        bytes_.fetch_add(packet.size());
        ++s.video_packets;
        s.video_octets += static_cast<std::uint32_t>(payload);
        const std::uint16_t seq = get16(packet.data() + 2);
        s.history[seq % kHistory] = std::move(packet);
        // Pace IDR bursts every 32 datagrams. A bounded steady-clock spin avoids Windows'
        // coarse sleep timer and scheduler-yield overshoot; total pacing is capped at 20 ms,
        // half the 40 ms send margin.
        if (item.key && ++packets % 32 == 0 && paced_ms < 20) {
            pacing_deadline += milliseconds(1);
            ++paced_ms;
            while (Clock::now() < pacing_deadline) {}
        }
    };
    for (std::size_t i = 0; i < nals.size(); ++i) {
        const auto [data, size] = nals[i];
        const bool last = i + 1 == nals.size();
        if (size <= kMaxPayload) {
            Bytes p = rtp_header(s.video_pt, last, s.video_seq++, ts, s.video_ssrc);
            p.insert(p.end(), data, data + size);
            emit(std::move(p));
            continue;
        }
        // FU-A fragments (RFC 6184 5.8).
        const std::uint8_t indicator = static_cast<std::uint8_t>((data[0] & 0xE0) | 28), type = data[0] & 0x1F;
        for (std::size_t at = 1; at < size;) {
            const std::size_t n = std::min(kMaxPayload - 2, size - at);
            const bool first = at == 1, end = at + n == size;
            Bytes p = rtp_header(s.video_pt, last && end, s.video_seq++, ts, s.video_ssrc);
            p.push_back(indicator);
            p.push_back(static_cast<std::uint8_t>((first ? 0x80 : 0) | (end ? 0x40 : 0) | type));
            p.insert(p.end(), data + at, data + at + n);
            emit(std::move(p));
            at += n;
        }
    }
    if (!sent_all) {
        // A failed UDP send can leave an IDR incomplete even after the bounded ENOBUFS retry.
        // Ask the encoder for another keyframe instead of waiting for its regular 2 s interval.
        refresh_wanted_.store(true);
    }
    if (item.key && sent_all && s.video_packets > packets_before) {
        s.video_keyframe_sent = true;
    }
}

void WhipPublisher::send_audio(Session& s, const Media& item) {
    Bytes p = rtp_header(s.audio_pt, false, s.audio_seq++, s.audio_rtp(item.sample), s.audio_ssrc);
    p.insert(p.end(), item.data.begin(), item.data.end());
    const std::size_t payload = item.data.size();
    if (!s.out->protect_rtp(p)) return;
    s.send(p);
    bytes_.fetch_add(p.size());
    ++s.audio_packets;
    s.audio_octets += static_cast<std::uint32_t>(payload);
}

void WhipPublisher::handle_datagram(Session& s, const std::uint8_t* data, std::size_t size, const Endpoint& from) {
    if (data[0] < 4) {
        if (size < 20 || get32(data + 4) != stun::kMagic) return;
        if (get16(data) != 0x0001) return;  // Responses only refresh consent.
        // Success response with XOR-MAPPED-ADDRESS, signed with our password.
        stun::Message m(stun::kBindingSuccess, data + 8);
        m.mapped_address(from);
        const Bytes reply = m.finish(s.local_pwd);
        s.socket->send(from, reply.data(), reply.size());
        return;
    }
    if (data[0] >= 20 && data[0] <= 63) {
        s.dtls.receive(data, size, Clock::now());
        for (const auto& d : s.dtls.take_outgoing()) s.send(d);
        return;
    }
    if (data[0] < 128 || data[0] > 191 || size < 12 || data[1] < 192 || data[1] > 223) return;
    Bytes packet(data, data + size);
    if (!s.in->unprotect_rtcp(packet)) return;
    for (std::size_t at = 0; at + 4 <= packet.size();) {
        const std::uint8_t format = packet[at] & 0x1F, type = packet[at + 1];
        const std::size_t length = (static_cast<std::size_t>(get16(packet.data() + at + 2)) + 1) * 4;
        if (at + length > packet.size()) break;
        if (type == 205 && format == 1 && length >= 16) {
            // Generic NACK: resend the packets still kept.
            for (std::size_t f = at + 12; f + 4 <= at + length; f += 4) {
                const std::uint16_t pid = get16(packet.data() + f), mask = get16(packet.data() + f + 2);
                for (int bit = -1; bit < 16; ++bit) {
                    if (bit >= 0 && !(mask & (1 << bit))) continue;
                    const auto seq = static_cast<std::uint16_t>(pid + (bit + 1));
                    nacked_packets_.fetch_add(1);
                    const auto& kept = s.history[seq % kHistory];
                    if (kept.size() >= 12 && get16(kept.data() + 2) == seq) s.send(kept);
                }
            }
        } else if (type == 201 && length >= 8) {
            for (std::size_t i = 0, f = at + 8; i < format && f + 24 <= at + length; ++i, f += 24) {
                if (get32(packet.data() + f) != s.video_ssrc) continue;
                std::int32_t lost = static_cast<std::int32_t>((packet[f + 5] << 16) |
                                                               (packet[f + 6] << 8) | packet[f + 7]);
                if (lost & 0x800000) lost -= 0x1000000;
                server_lost_.store(lost);
            }
        } else if (type == 206 && format == 1 && length >= 12) {
            pli_count_.fetch_add(1);
            keyframe_wanted_.store(true);
        } else if (type == 206 && format == 4 && length >= 20) {
            fir_count_.fetch_add(1);
            refresh_wanted_.store(true);
        }
        at += length;
    }
}

bool WhipPublisher::serve(Session& s) {
    std::uint8_t buffer[2048];
    for (;;) {
        auto now = Clock::now();
        Media item;
        Clock::time_point next;
        bool done = false, due = false;
        {
            std::lock_guard lock(mutex_);
            if (closing_ && (queue_.empty() || now >= flush_deadline_)) done = true;
            else due = take_due(s, now, item, next);
        }
        if (done) {
            hang_up(s, true);
            return true;
        }
        if (due) {
            if (item.video) send_video(s, item);
            else send_audio(s, item);
            continue;
        }
        if (now >= s.next_consent) {
            s.next_consent = now + kConsentEvery;
            s.consent();
        }
        if (now >= s.next_report && s.anchored) {
            s.next_report = now + kReportEvery;
            const std::uint64_t ntp = ntp_now();
            const double media_ms = s.t0_ms + std::chrono::duration<double, std::milli>(now - s.at).count();
            Bytes report;
            for (const bool video : {true, false}) {
                if (!video && s.audio_packets == 0) continue;
                report.clear();
                report.push_back(0x80);
                report.push_back(200);
                put16(report, 6);
                put32(report, video ? s.video_ssrc : s.audio_ssrc);
                put32(report, static_cast<std::uint32_t>(ntp >> 32));
                put32(report, static_cast<std::uint32_t>(ntp));
                put32(report, video ? s.video_rtp(media_ms)
                                    : s.audio_rtp(static_cast<std::int64_t>(media_ms * 48.0)));
                put32(report, video ? s.video_packets : s.audio_packets);
                put32(report, video ? s.video_octets : s.audio_octets);
                // SDES with the CNAME, as compound RTCP requires.
                report.push_back(0x81);
                report.push_back(202);
                put16(report, 3);
                put32(report, video ? s.video_ssrc : s.audio_ssrc);
                report.insert(report.end(), {1, 3, 'r', 'e', 'c', 0, 0, 0});
                if (s.out->protect_rtcp(report)) s.send(report);
            }
        }
        if (now - s.last_rx > kSilence) return false;
        if (s.dtls.closed() || !s.dtls.failure().empty()) return false;
        const auto wait = std::chrono::duration_cast<milliseconds>(next - now).count();
        Endpoint from;
        std::size_t got = s.socket->receive(buffer, sizeof buffer, static_cast<int>(std::clamp<long long>(wait, 0, 5)), from);
        while (got) {
            if (from == s.remote) s.last_rx = Clock::now();
            handle_datagram(s, buffer, got, from);
            got = s.socket->receive(buffer, sizeof buffer, 0, from);
        }
    }
}

void WhipPublisher::hang_up(Session& s, bool polite) {
    if (polite) {
        const Bytes bye = s.dtls.close_notify();
        if (!bye.empty()) s.send(bye);
    }
    if (s.has_resource) {
        Response ignored;
        http(s.resource, "DELETE", s.resource.path, {}, ignored, 3000);
        s.has_resource = false;
    }
}

} // namespace rec::core
