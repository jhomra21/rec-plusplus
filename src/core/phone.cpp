#include "core/phone.hpp"

#include "core/dtls.hpp"
#include "core/net.hpp"
#include "core/srtp.hpp"
#include "core/stun.hpp"
#include "core/tls_server.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace rec::core {
namespace {

using Bytes = crypto::Bytes;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr std::uint16_t kDefaultPort = 47110;
constexpr auto kNackAfter = milliseconds(20);     // A gap this old is asked for again.
constexpr auto kGiveUpAfter = milliseconds(300);  // Then the frame is dropped and a keyframe asked for.
constexpr auto kReportEvery = milliseconds(500);
constexpr auto kPliEvery = milliseconds(300);
constexpr auto kSessionIdle = std::chrono::seconds(30);
constexpr std::uint32_t kMaxBitrate = 8000000;

const char kPage[] =
    R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Rec camera</title>
<style>
body{margin:0;background:#111;color:#eee;font:17px -apple-system,system-ui,sans-serif;display:flex;flex-direction:column;height:100vh;height:100dvh}
video{flex:1;min-height:0;width:100%;object-fit:contain;background:#000}
#bar{display:flex;gap:10px;padding:12px 16px calc(12px + env(safe-area-inset-bottom));align-items:center}
#status{flex:1}
button{font:inherit;padding:10px 16px;border-radius:12px;border:0;background:#2c2c2e;color:#fff}
</style></head>
<body><video id="v" autoplay playsinline muted></video>
<div id="bar"><span id="status" role="status">Starting</span><button id="flip">Switch camera</button></div>
<script>
const token = location.hash.slice(1), v = document.getElementById('v'), status = document.getElementById('status');
let facing = localStorage.getItem('rec-facing') || 'environment', stream = null, pc = null, sender = null, timer = null;
const say = t => { status.textContent = t; };
async function camera() {
  const s = await navigator.mediaDevices.getUserMedia({audio: false, video: {facingMode: {ideal: facing},
    width: {ideal: 1920}, height: {ideal: 1080}, frameRate: {ideal: 30}}});
  if (stream) stream.getTracks().forEach(t => t.stop());
  stream = s;
  v.srcObject = s;
  v.style.transform = facing === 'user' ? 'scaleX(-1)' : '';
  return s.getVideoTracks()[0];
}
async function connect() {
  if (pc) { pc.onconnectionstatechange = null; pc.close(); }
  pc = new RTCPeerConnection();
  const transceiver = pc.addTransceiver(stream.getVideoTracks()[0], {direction: 'sendonly', streams: [stream]});
  sender = transceiver.sender;
  const caps = RTCRtpSender.getCapabilities && RTCRtpSender.getCapabilities('video');
  if (caps && transceiver.setCodecPreferences) {
    const h264 = caps.codecs.filter(c => c.mimeType.toLowerCase() === 'video/h264');
    if (h264.length) transceiver.setCodecPreferences(h264);
  }
  const mine = pc;
  const lost = () => { if (mine === pc) { say('Reconnecting'); retry(); } };
  pc.onconnectionstatechange = () => {
    if (mine !== pc) return;
    if (pc.connectionState === 'connected') say('Live in Rec');
    else if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected' || pc.connectionState === 'closed') lost();
  };
  await pc.setLocalDescription(await pc.createOffer());
  await new Promise(done => {
    if (pc.iceGatheringState === 'complete') return done();
    pc.onicegatheringstatechange = () => { if (pc.iceGatheringState === 'complete') done(); };
    setTimeout(done, 1500);
  });
  const response = await fetch('/offer', {method: 'POST', cache: 'no-store',
    headers: {'Content-Type': 'application/sdp', 'Authorization': 'Bearer ' + token}, body: pc.localDescription.sdp});
  if (!response.ok) throw new Error(response.status === 403 ? 'This link is old. Scan the code in Rec again.' : 'Rec answered ' + response.status + ': ' + await response.text());
  await pc.setRemoteDescription({type: 'answer', sdp: await response.text()});
  // Rec ends the session with a DTLS close_notify when it closes the camera.
  const transport = transceiver.sender.transport;
  if (transport) transport.onstatechange = () => { if (transport.state === 'closed' || transport.state === 'failed') lost(); };
  const p = sender.getParameters();
  if (!p.encodings || !p.encodings.length) p.encodings = [{}];
  p.encodings[0].maxBitrate = 8000000;
  p.degradationPreference = 'maintain-resolution';
  try { await sender.setParameters(p); } catch (e) {}
}
function retry() {
  if (timer) return;
  timer = setTimeout(async () => {
    timer = null;
    try { await connect(); } catch (e) { say(e.message); retry(); }
  }, 2000);
}
document.getElementById('flip').onclick = async () => {
  facing = facing === 'user' ? 'environment' : 'user';
  localStorage.setItem('rec-facing', facing);
  try { const track = await camera(); if (sender) await sender.replaceTrack(track); } catch (e) { say(e.message); }
};
async function awake() { try { await navigator.wakeLock.request('screen'); } catch (e) {} }
document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'visible') awake(); });
(async () => {
  if (!token) { say('Open the link Rec shows.'); return; }
  try { await camera(); } catch (e) { say('No camera: ' + e.message); return; }
  say('Connecting');
  awake();
  try { await connect(); } catch (e) { say(e.message); retry(); }
})();
</script></body></html>
)PAGE"
;

void put8(Bytes& b, std::uint64_t v) { b.push_back(static_cast<std::uint8_t>(v)); }
void put16(Bytes& b, std::uint64_t v) { put8(b, v >> 8); put8(b, v); }
void put32(Bytes& b, std::uint64_t v) { put16(b, v >> 16); put16(b, v); }
std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint32_t get32(const std::uint8_t* p) { return static_cast<std::uint32_t>(get16(p)) << 16 | get16(p + 2); }

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

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

bool same(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char differ = 0;
    for (std::size_t i = 0; i < a.size(); ++i) differ |= static_cast<unsigned char>(a[i] ^ b[i]);
    return differ == 0;
}

// --- The page's offer ---------------------------------------------------------------------

struct Offer {
    std::string ufrag, pwd, fingerprint;
    struct Media {
        std::string kind, mid, protocol, line;
        std::vector<int> formats;
    };
    std::vector<Media> media;
    int video = -1;  // Index into media of the camera's m-line.
    int pt = -1;
    std::string fmtp;
};

std::string parse_offer(const std::string& sdp, Offer& out) {
    std::map<int, std::string> codecs, fmtps;
    std::size_t at = 0;
    while (at < sdp.size()) {
        auto end = sdp.find('\n', at);
        if (end == std::string::npos) end = sdp.size();
        std::string line = sdp.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("m=", 0) == 0) {
            Offer::Media m;
            m.line = line;
            char kind[16] = {}, protocol[64] = {};
            std::sscanf(line.c_str(), "m=%15s %*u %63s", kind, protocol);
            m.kind = kind;
            m.protocol = protocol;
            for (std::size_t space = line.find(' ', line.find(protocol) + 1); space != std::string::npos; space = line.find(' ', space + 1))
                m.formats.push_back(std::atoi(line.c_str() + space + 1));
            out.media.push_back(m);
            continue;
        }
        if (line.rfind("a=", 0) != 0) continue;
        const auto colon = line.find(':');
        const std::string name = line.substr(2, colon == std::string::npos ? std::string::npos : colon - 2);
        const std::string value = colon == std::string::npos ? "" : line.substr(colon + 1);
        const bool in_video = !out.media.empty() && out.media.back().kind == "video";
        if (name == "ice-ufrag" && out.ufrag.empty()) out.ufrag = value;
        else if (name == "ice-pwd" && out.pwd.empty()) out.pwd = value;
        else if (name == "fingerprint" && out.fingerprint.empty() && lower(value).rfind("sha-256 ", 0) == 0) out.fingerprint = value.substr(8);
        else if (name == "mid" && !out.media.empty()) out.media.back().mid = value;
        else if (name == "rtpmap" && in_video) codecs[std::atoi(value.c_str())] = lower(value.substr(value.find(' ') + 1));
        else if (name == "fmtp" && in_video) fmtps[std::atoi(value.c_str())] = value.substr(value.find(' ') + 1);
    }
    for (std::size_t i = 0; i < out.media.size() && out.video < 0; ++i)
        if (out.media[i].kind == "video") out.video = static_cast<int>(i);
    if (out.video < 0) return "the page offered no video";
    // H.264 with packetization-mode=1, Constrained Baseline first (every decoder has it).
    int best = -1, best_score = -1;
    for (const int pt : out.media[static_cast<std::size_t>(out.video)].formats) {
        if (codecs[pt].rfind("h264/90000", 0) != 0) continue;
        const std::string f = lower(fmtps[pt]);
        if (f.find("packetization-mode=1") == std::string::npos) continue;
        const int score = f.find("profile-level-id=42e0") != std::string::npos ? 3 : f.find("profile-level-id=4200") != std::string::npos ? 2 : 1;
        if (score > best_score) { best = pt; best_score = score; }
    }
    if (best < 0) return "the phone's browser offered no H.264 video";
    out.pt = best;
    out.fmtp = fmtps[best];
    if (out.ufrag.empty() || out.pwd.empty()) return "the offer has no ICE credentials";
    if (out.fingerprint.empty()) return "the offer has no DTLS fingerprint";
    return {};
}

// --- H.264 from RTP (RFC 6184) ----------------------------------------------------------

struct Packet {
    std::int64_t seq = 0;
    std::uint32_t ts = 0;
    bool marker = false;
    Bytes payload;
};

class Depacketizer {
public:
    // A packet in sequence order; unit is called with each complete access unit.
    template <class Unit> void add(const Packet& p, Unit&& unit) {
        if (open_ && p.ts != ts_) finish(false, unit);  // The last packet of the unit was lost.
        if (!open_) {
            open_ = true;
            ts_ = p.ts;
            au_.clear();
            key_ = false;
            broken_ = pending_broken_;
            pending_broken_ = false;
            fu_open_ = false;
        }
        const std::uint8_t* d = p.payload.data();
        const std::size_t n = p.payload.size();
        if (n) {
            const std::uint8_t type = d[0] & 0x1F;
            if (type >= 1 && type <= 23) {
                nal(d, n);
            } else if (type == 24) {  // STAP-A
                for (std::size_t at = 1; at + 2 <= n;) {
                    const std::size_t size = get16(d + at);
                    at += 2;
                    if (!size || at + size > n) { broken_ = true; break; }
                    nal(d + at, size);
                    at += size;
                }
            } else if (type == 28 && n >= 2) {  // FU-A
                const bool start = d[1] & 0x80, end = d[1] & 0x40;
                if (start) {
                    fu_.assign(1, static_cast<std::uint8_t>((d[0] & 0xE0) | (d[1] & 0x1F)));
                    fu_open_ = true;
                }
                if (!fu_open_) broken_ = true;
                else {
                    fu_.insert(fu_.end(), d + 2, d + n);
                    if (end) { nal(fu_.data(), fu_.size()); fu_open_ = false; }
                }
            }
        }
        if (p.marker) finish(true, unit);
    }
    // Packets were lost: the unit they belonged to cannot be decoded.
    void lose() {
        if (open_) broken_ = true;
        else pending_broken_ = true;
    }
    bool waiting_for_key() const { return waiting_key_; }

private:
    void nal(const std::uint8_t* d, std::size_t n) {
        static const std::uint8_t kStart[4] = {0, 0, 0, 1};
        au_.insert(au_.end(), kStart, kStart + 4);
        au_.insert(au_.end(), d, d + n);
        if ((d[0] & 0x1F) == 5) key_ = true;
    }
    template <class Unit> void finish(bool marker, Unit& unit) {
        open_ = false;
        if (broken_ || !marker || fu_open_) { waiting_key_ = true; return; }
        if (waiting_key_ && !key_) return;
        waiting_key_ = false;
        unit(au_, key_);
    }

    bool open_ = false, broken_ = false, pending_broken_ = false, key_ = false, fu_open_ = false;
    bool waiting_key_ = true;
    std::uint32_t ts_ = 0;
    Bytes au_, fu_;
};

std::string http_response(int status, const char* reason, const std::string& type, const std::string& body,
                          const std::string& extra = {}) {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + type +
           "\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nCache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n" + extra + "\r\n" + body;
}
} // namespace

// --- One WebRTC session with the page -----------------------------------------------------

struct PhoneCamera::Session {
    std::unique_ptr<UdpSocket> socket;
    Endpoint remote;
    bool have_remote = false;
    std::string local_ufrag, local_pwd;
    DtlsClient dtls;
    bool dtls_started = false;
    std::unique_ptr<Srtp> in, out;
    std::uint8_t pt = 96;
    std::uint32_t ssrc = 0;         // Ours, for RTCP.
    std::uint32_t media_ssrc = 0;   // The phone's video.
    bool have_media = false;
    // Reordering and loss: packets wait for those before them.
    std::map<std::int64_t, Packet> waiting;
    std::int64_t next = -1, highest = -1, base = -1;
    std::map<std::int64_t, std::pair<Clock::time_point, int>> asked;  // NACKed: when, how often.
    Clock::time_point gap_since{};
    std::uint64_t received = 0, expected_prior = 0, received_prior = 0;
    std::uint32_t lsr = 0;
    Clock::time_point lsr_at{}, last_rx{}, next_report{}, last_pli{};
    Depacketizer depacketizer;

    std::int64_t unwrap(std::uint16_t seq) {
        if (highest < 0) return seq;
        std::int64_t candidate = (highest & ~0xFFFFLL) | seq;
        if (candidate - highest > 0x8000) candidate -= 0x10000;
        else if (highest - candidate > 0x8000) candidate += 0x10000;
        return candidate;
    }
    void send_rtcp(Bytes packet) {
        if (!out || !have_remote || !out->protect_rtcp(packet)) return;
        socket->send(remote, packet.data(), packet.size());
    }
    // A receiver report, which every compound RTCP packet starts with.
    Bytes report() {
        Bytes p{0x81, 201};
        put16(p, 7);
        put32(p, ssrc);
        put32(p, media_ssrc);
        const std::uint64_t expected = highest >= base && base >= 0 ? static_cast<std::uint64_t>(highest - base + 1) : 0;
        const std::uint64_t lost_total = expected > received ? expected - received : 0;
        const std::uint64_t expected_interval = expected - expected_prior, received_interval = received - received_prior;
        expected_prior = expected;
        received_prior = received;
        const std::uint64_t lost_interval = expected_interval > received_interval ? expected_interval - received_interval : 0;
        const std::uint8_t fraction = expected_interval ? static_cast<std::uint8_t>(std::min<std::uint64_t>(255, (lost_interval << 8) / expected_interval)) : 0;
        put8(p, fraction);
        put8(p, lost_total >> 16);
        put16(p, lost_total & 0xFFFF);
        put32(p, static_cast<std::uint32_t>(std::max<std::int64_t>(highest, 0)));
        put32(p, 0);  // Jitter.
        put32(p, lsr);
        const auto since = lsr ? std::chrono::duration_cast<milliseconds>(Clock::now() - lsr_at).count() * 65536 / 1000 : 0;
        put32(p, static_cast<std::uint32_t>(since));
        return p;
    }
};

// --- The HTTPS server ---------------------------------------------------------------------

namespace {

// The phone page's TLS key and certificate. One per user, kept in a file beside the background
// engine's socket, so the preview, a recording in the engine and later runs all show a phone the
// certificate its person already confirmed (a browser asks again for any other one).
struct Identity {
    std::unique_ptr<crypto::P256> key;
    Bytes certificate;
};

constexpr std::string_view kIdentityMagic = "rec phone identity 1\n";
constexpr int kIdentityDays = 365;
constexpr std::int64_t kRenewBefore = 30 * 86400;  // Seconds before the certificate expires.

std::filesystem::path identity_path() {
#ifdef _WIN32
    const char* local = std::getenv("LOCALAPPDATA");
    if (local && *local) return std::filesystem::path(local) / "rec" / "phone-identity";
#else
    const char* home = std::getenv("HOME");
    if (home && *home) return std::filesystem::path(home) / "Library" / "Application Support" / "rec" / "phone-identity";
#endif
    return {};
}

// The kept identity, unless it is missing, damaged or expires within kRenewBefore.
std::optional<Identity> load_identity(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t key_at = kIdentityMagic.size() + 8, certificate_at = key_at + 97;
    if (data.size() <= certificate_at || data.compare(0, kIdentityMagic.size(), kIdentityMagic) != 0) return std::nullopt;
    std::int64_t expires = 0;
    for (std::size_t i = kIdentityMagic.size(); i < key_at; ++i) expires = expires << 8 | static_cast<std::uint8_t>(data[i]);
    if (expires - kRenewBefore < static_cast<std::int64_t>(std::time(nullptr))) return std::nullopt;
    Identity kept;
    Bytes key(data.begin() + key_at, data.begin() + certificate_at);
    kept.key = crypto::P256::import_private(key);
    std::fill(key.begin(), key.end(), std::uint8_t{0});
    kept.certificate.assign(data.begin() + certificate_at, data.end());
    if (!kept.key) return std::nullopt;
    return kept;
}

// Saves made unless another process saved one first; false if made was not saved.
bool save_identity(const std::filesystem::path& path, const Identity& made, std::int64_t expires) {
    Bytes key = made.key->private_key();
    if (key.empty()) return false;
    std::string data(kIdentityMagic);
    for (int shift = 56; shift >= 0; shift -= 8) data.push_back(static_cast<char>(expires >> shift));
    data.append(key.begin(), key.end());
    data.append(made.certificate.begin(), made.certificate.end());
    std::fill(key.begin(), key.end(), std::uint8_t{0});
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::uint8_t suffix[8];
    crypto::random(suffix, sizeof suffix);
    auto temporary = path;
    temporary += ".";
    for (const auto byte : suffix) {
        char digits[3];
        std::snprintf(digits, sizeof digits, "%02x", byte);
        temporary += digits;
    }
    // Only its owner reads the private key: made empty and restricted before it is written.
    std::ofstream(temporary, std::ios::binary).close();
    std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, error);
    bool written = !error;
    if (written) {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        written = static_cast<bool>(out);
    }
    std::fill(data.begin(), data.end(), '\0');
    // A link fails when the file exists, so of two processes starting at once only one saves.
    if (written) std::filesystem::create_hard_link(temporary, path, error);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return written && !error;
}

const Identity* identity() {
    static const Identity id = [] {
        const auto path = identity_path();
        if (!path.empty()) {
            if (auto kept = load_identity(path)) return std::move(*kept);
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        Identity made;
        made.key = crypto::P256::generate(crypto::P256::Use::sign);
        if (made.key) made.certificate = self_signed_certificate(*made.key, kIdentityDays);
        if (!path.empty() && made.key && !made.certificate.empty()) {
            const auto expires = static_cast<std::int64_t>(std::time(nullptr)) + std::int64_t{kIdentityDays} * 86400;
            if (!save_identity(path, made, expires))
                if (auto kept = load_identity(path)) return std::move(*kept);
        }
        return made;
    }();
    return id.key && !id.certificate.empty() ? &id : nullptr;
}

} // namespace

struct PhoneCamera::Server {    std::unique_ptr<TcpListener> listener;
    std::thread thread;
    std::atomic_bool stop{false};
    std::mutex mutex;
    std::map<std::string, std::weak_ptr<PhoneCamera>> cameras;

    ~Server() {
        stop.store(true);
        if (thread.joinable()) thread.join();
    }

    static std::shared_ptr<Server> shared(std::string& error) {
        static std::mutex lock;
        static std::weak_ptr<Server> current;
        std::lock_guard guard(lock);
        if (auto running = current.lock()) return running;
        if (!identity()) { error = "could not make the phone camera's certificate"; return nullptr; }
        auto server = std::make_shared<Server>();
        server->listener = TcpListener::listen(PhoneCamera::port(), error);
        if (!server->listener) {
            error = "the phone camera cannot listen: " + error;
            return nullptr;
        }
        Server* raw = server.get();
        server->thread = std::thread([raw] { raw->run(); });
        current = server;
        return server;
    }

    void run() {
        while (!stop.load()) {
            Endpoint peer;
            auto tcp = listener->accept(200, peer);
            if (!tcp) continue;
            // One request per connection, one connection at a time: the page makes two.
            std::string error;
            const auto* id = identity();
            auto tls = tls_accept(std::move(tcp), *id->key, id->certificate, error);
            if (tls) serve(*tls, endpoint_ip(peer));
        }
    }

    void serve(Transport& t, const std::string& peer_ip) {
        std::string request;
        std::uint8_t buffer[4096];
        std::size_t header_end = std::string::npos, length = 0;
        while (header_end == std::string::npos || request.size() < header_end + 4 + length) {
            const std::size_t got = t.receive(buffer, sizeof buffer);
            if (!got) return;
            request.append(reinterpret_cast<const char*>(buffer), got);
            if (header_end == std::string::npos && (header_end = request.find("\r\n\r\n")) != std::string::npos) {
                const auto headers = lower(request.substr(0, header_end));
                if (const auto at = headers.find("\r\ncontent-length:"); at != std::string::npos)
                    length = std::strtoul(headers.c_str() + at + 17, nullptr, 10);
            }
            if (request.size() > 65536) return;
        }
        const std::string line = request.substr(0, request.find("\r\n"));
        const std::string headers = request.substr(0, header_end);
        const std::string body = request.substr(header_end + 4, length);
        std::string response;
        if (line.rfind("GET / ", 0) == 0) {
            response = http_response(200, "OK", "text/html; charset=utf-8", kPage,
                                     "Content-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; media-src 'self' blob: mediastream:\r\n");
        } else if (line.rfind("POST /offer ", 0) == 0) {
            std::string token;
            const auto lowered = lower(headers);
            static constexpr std::string_view kBearer = "\r\nauthorization: bearer ";
            if (const auto at = lowered.find(kBearer); at != std::string::npos) {
                const auto start = at + kBearer.size();
                token = headers.substr(start, headers.find("\r\n", start) - start);
            }
            std::shared_ptr<PhoneCamera> camera;
            {
                std::lock_guard guard(mutex);
                for (const auto& [known, weak] : cameras)
                    if (same(known, token)) camera = weak.lock();
            }
            if (!camera) {
                response = http_response(403, "Forbidden", "text/plain", "This link is not open in Rec.");
            } else {
                std::string error;
                const std::string answer = camera->answer(body, peer_ip, error);
                response = answer.empty() ? http_response(400, "Bad Request", "text/plain", error)
                                          : http_response(201, "Created", "application/sdp", answer);
            }
        } else {
            response = http_response(404, "Not Found", "text/plain", "Not found");
        }
        t.send(reinterpret_cast<const std::uint8_t*>(response.data()), response.size());
        t.finish();
    }
};

// --- PhoneCamera ------------------------------------------------------------------------

std::uint16_t PhoneCamera::port() {
    if (const char* text = std::getenv("REC_PHONE_PORT")) {
        const long value = std::strtol(text, nullptr, 10);
        if (value > 0 && value < 65536) return static_cast<std::uint16_t>(value);
    }
    return kDefaultPort;
}

std::string PhoneCamera::new_token() { return random_text(22); }

std::string PhoneCamera::url(const std::string& token) {
    // Routing toward a public address picks the interface on the home network; nothing is sent.
    const std::string ip = local_address_toward("8.8.8.8");
    if (ip.empty()) return {};
    return "https://" + ip + ":" + std::to_string(port()) + "/#" + token;
}

PhoneCamera::PhoneCamera(std::string token) : token_(std::move(token)) {}

std::shared_ptr<PhoneCamera> PhoneCamera::open(const std::string& token, std::string& error) {
    if (token.size() < 16) { error = "the phone camera needs a token of 16 or more characters"; return nullptr; }
    auto server = Server::shared(error);
    if (!server) return nullptr;
    std::shared_ptr<PhoneCamera> camera(new PhoneCamera(token));
    camera->server_ = server;
    std::lock_guard guard(server->mutex);
    for (auto it = server->cameras.begin(); it != server->cameras.end();)
        it = it->second.expired() ? server->cameras.erase(it) : std::next(it);
    if (server->cameras.count(token)) { error = "this phone camera is already open"; return nullptr; }
    server->cameras[token] = camera;
    return camera;
}

PhoneCamera::~PhoneCamera() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (server_) {
        std::lock_guard guard(server_->mutex);
        server_->cameras.erase(token_);
    }
}

void PhoneCamera::set_sink(Sink sink) {
    std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
}

void PhoneCamera::request_keyframe() { keyframe_wanted_.store(true); }

bool PhoneCamera::connected() const {
    const auto last = last_media_ns_.load();
    return last && Clock::now().time_since_epoch().count() - last <
                       std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(2)).count();
}

void PhoneCamera::deliver(const Bytes& unit, bool key) {
    frames_.fetch_add(1);
    std::lock_guard lock(mutex_);
    if (sink_) sink_(unit.data(), unit.size(), key);
}

std::string PhoneCamera::answer(const std::string& sdp, const std::string& peer_ip, std::string& error) {
    Offer offer;
    if (error = parse_offer(sdp, offer); !error.empty()) return {};
    auto s = std::make_unique<Session>();
    if (error = s->dtls.init(); !error.empty()) return {};
    if (!s->dtls.expect_fingerprint(offer.fingerprint)) { error = "the offer has a bad DTLS fingerprint"; return {}; }
    s->socket = UdpSocket::toward(peer_ip, error);
    if (!s->socket) return {};
    s->local_ufrag = random_text(8);
    s->local_pwd = random_text(24);
    s->ssrc = random32();
    s->pt = static_cast<std::uint8_t>(offer.pt);

    const auto& local = s->socket->local();
    const std::string ip = endpoint_ip(local), family = local.v6 ? "IP6" : "IP4", port = std::to_string(local.port);
    const auto& video = offer.media[static_cast<std::size_t>(offer.video)];
    const std::string pt = std::to_string(offer.pt);
    std::string answer = "v=0\r\no=- " + std::to_string(random32()) + " 2 IN " + family + " " + ip + "\r\ns=-\r\nt=0 0\r\n" +
                         "a=ice-lite\r\n" + (video.mid.empty() ? "" : "a=group:BUNDLE " + video.mid + "\r\n") +
                         "a=msid-semantic: WMS\r\n";
    for (std::size_t i = 0; i < offer.media.size(); ++i) {
        const auto& m = offer.media[i];
        if (static_cast<int>(i) != offer.video) {
            // Only the camera is taken.
            answer += "m=" + m.kind + " 0 " + m.protocol + " " + (m.formats.empty() ? "0" : std::to_string(m.formats[0])) +
                      "\r\nc=IN " + family + " " + ip + "\r\n" + (m.mid.empty() ? "" : "a=mid:" + m.mid + "\r\n") + "a=inactive\r\n";
            continue;
        }
        answer += "m=video " + port + " UDP/TLS/RTP/SAVPF " + pt + "\r\n" +
                  "c=IN " + family + " " + ip + "\r\n" +
                  "a=rtcp:" + port + " IN " + family + " " + ip + "\r\n" +
                  "a=ice-ufrag:" + s->local_ufrag + "\r\na=ice-pwd:" + s->local_pwd + "\r\n" +
                  "a=fingerprint:sha-256 " + s->dtls.fingerprint() + "\r\n" +
                  // rec plays the DTLS client; the page offered actpass.
                  "a=setup:active\r\n" + (m.mid.empty() ? "" : "a=mid:" + m.mid + "\r\n") +
                  "a=recvonly\r\na=rtcp-mux\r\n" +
                  "a=rtpmap:" + pt + " H264/90000\r\n" +
                  "a=rtcp-fb:" + pt + " nack\r\na=rtcp-fb:" + pt + " nack pli\r\na=rtcp-fb:" + pt + " ccm fir\r\n" +
                  "a=rtcp-fb:" + pt + " goog-remb\r\n" +
                  // Without transport-wide feedback, browsers would start near 300 kb/s and
                  // climb slowly; on a home network the camera can start high.
                  "a=fmtp:" + pt + " " + offer.fmtp + ";x-google-start-bitrate=4000;x-google-min-bitrate=1000;x-google-max-bitrate=" +
                  std::to_string(kMaxBitrate / 1000) + "\r\n" +
                  "a=candidate:1 1 udp 2130706431 " + ip + " " + port + " typ host\r\na=end-of-candidates\r\n";
    }

    // A new offer (the page reloaded or reconnected) replaces the session.
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    stop_.store(false);
    session_ = std::move(s);
    Session* session = session_.get();
    thread_ = std::thread([this, session] { run(*session); });
    return answer;
}

void PhoneCamera::run(Session& s) {
    std::uint8_t buffer[2048];
    s.last_rx = Clock::now();
    auto unit = [this](const Bytes& au, bool key) {
        last_media_ns_.store(Clock::now().time_since_epoch().count());
        deliver(au, key);
    };
    while (!stop_.load()) {
        const auto now = Clock::now();
        if (now - s.last_rx > kSessionIdle) return;
        if (s.dtls_started && !s.dtls.connected()) {
            s.dtls.poll(now);
            for (const auto& d : s.dtls.take_outgoing()) s.socket->send(s.remote, d.data(), d.size());
            if (!s.dtls.failure().empty()) return;
        }
        if (s.out && s.have_media) {
            // Loss: ask again for what is missing, then give up and wait for a keyframe.
            if (!s.waiting.empty() && s.next >= 0 && s.next < s.waiting.begin()->first) {
                if (s.gap_since == Clock::time_point{}) s.gap_since = now;
                std::vector<std::uint16_t> missing;
                for (std::int64_t seq = s.next; seq < s.waiting.begin()->first && seq < s.next + 256; ++seq) {
                    auto& [when, count] = s.asked[seq];
                    if (count >= 4 || (count && now - when < milliseconds(60))) continue;
                    if (!count && now - s.gap_since < kNackAfter) continue;
                    when = now;
                    ++count;
                    missing.push_back(static_cast<std::uint16_t>(seq));
                }
                // Generic NACK (RFC 4585 6.2.1): a packet ID and a mask of the 16 after it.
                Bytes nack;
                for (std::size_t i = 0; i < missing.size();) {
                    const std::uint16_t pid = missing[i++];
                    std::uint16_t mask = 0;
                    while (i < missing.size() && static_cast<std::uint16_t>(missing[i] - pid) <= 16) {
                        mask = static_cast<std::uint16_t>(mask | 1 << (static_cast<std::uint16_t>(missing[i] - pid) - 1));
                        ++i;
                    }
                    put16(nack, pid);
                    put16(nack, mask);
                }
                if (!nack.empty()) {
                    Bytes p = s.report();
                    put8(p, 0x81);
                    put8(p, 205);
                    put16(p, 2 + nack.size() / 4);
                    put32(p, s.ssrc);
                    put32(p, s.media_ssrc);
                    p.insert(p.end(), nack.begin(), nack.end());
                    s.send_rtcp(std::move(p));
                }
                if (now - s.gap_since > kGiveUpAfter || s.waiting.size() > 2000) {
                    s.depacketizer.lose();
                    s.next = s.waiting.begin()->first;
                    s.gap_since = {};
                    while (!s.waiting.empty() && s.waiting.begin()->first == s.next) {
                        s.depacketizer.add(s.waiting.begin()->second, unit);
                        s.waiting.erase(s.waiting.begin());
                        ++s.next;
                    }
                }
            }
            if (s.depacketizer.waiting_for_key() || keyframe_wanted_.exchange(false)) {
                if (now - s.last_pli > kPliEvery) {
                    s.last_pli = now;
                    Bytes p = s.report();
                    put8(p, 0x81);
                    put8(p, 206);
                    put16(p, 2);
                    put32(p, s.ssrc);
                    put32(p, s.media_ssrc);
                    s.send_rtcp(std::move(p));
                }
            }
            if (now >= s.next_report) {
                s.next_report = now + kReportEvery;
                Bytes p = s.report();
                // REMB (draft-alvestrand-rmcat-remb): the most the phone should send.
                put8(p, 0x8F);
                put8(p, 206);
                put16(p, 5);
                put32(p, s.ssrc);
                put32(p, 0);
                p.insert(p.end(), {'R', 'E', 'M', 'B'});
                std::uint32_t mantissa = kMaxBitrate, exponent = 0;
                while (mantissa >= (1u << 18)) { mantissa >>= 1; ++exponent; }
                put8(p, 1);
                put8(p, exponent << 2 | mantissa >> 16);
                put16(p, mantissa & 0xFFFF);
                put32(p, s.media_ssrc);
                s.send_rtcp(std::move(p));
            }
        }

        Endpoint from;
        const std::size_t got = s.socket->receive(buffer, sizeof buffer, 10, from);
        if (!got) continue;
        if (stun::is_stun(buffer, got)) {
            if (stun::type_of(buffer) != stun::kBindingRequest) continue;
            const std::string user = stun::text_attribute(buffer, got, stun::kUsername);
            if (user.rfind(s.local_ufrag + ":", 0) != 0) continue;
            s.last_rx = Clock::now();
            stun::Message m(stun::kBindingSuccess, buffer + 8);
            m.mapped_address(from);
            const Bytes reply = m.finish(s.local_pwd);
            s.socket->send(from, reply.data(), reply.size());
            if (!s.have_remote || stun::has_attribute(buffer, got, stun::kUseCandidate)) {
                s.remote = from;
                s.have_remote = true;
            }
            if (!s.dtls_started) {
                s.dtls_started = true;
                s.dtls.start(Clock::now());
                for (const auto& d : s.dtls.take_outgoing()) s.socket->send(s.remote, d.data(), d.size());
            }
            continue;
        }
        if (!s.have_remote || !(from == s.remote)) continue;
        s.last_rx = Clock::now();
        if (buffer[0] >= 20 && buffer[0] <= 63) {
            s.dtls.receive(buffer, got, Clock::now());
            for (const auto& d : s.dtls.take_outgoing()) s.socket->send(s.remote, d.data(), d.size());
            if (!s.dtls.failure().empty()) return;
            if (s.dtls.connected() && !s.in) {
                const auto& keys = s.dtls.keys();
                s.in = std::make_unique<Srtp>(keys.remote_key.data(), keys.remote_salt.data());
                s.out = std::make_unique<Srtp>(keys.local_key.data(), keys.local_salt.data());
                if (!s.in->valid() || !s.out->valid()) return;
            }
            continue;
        }
        if (!s.in || buffer[0] < 128 || buffer[0] > 191 || got < 12) continue;
        Bytes packet(buffer, buffer + got);
        if (buffer[1] >= 192 && buffer[1] <= 223) {
            if (!s.in->unprotect_rtcp(packet)) continue;
            for (std::size_t at = 0; at + 8 <= packet.size();) {
                const std::size_t length = (static_cast<std::size_t>(get16(packet.data() + at + 2)) + 1) * 4;
                if (at + length > packet.size()) break;
                if (packet[at + 1] == 200 && length >= 20) {
                    // A sender report: its NTP time's middle 32 bits, echoed in our reports.
                    s.lsr = get32(packet.data() + at + 10);
                    s.lsr_at = Clock::now();
                }
                at += length;
            }
            continue;
        }
        if ((buffer[1] & 0x7F) != s.pt) continue;
        if (!s.in->unprotect_rtp(packet)) continue;
        const std::uint32_t ssrc = get32(packet.data() + 8);
        if (!s.have_media || ssrc != s.media_ssrc) {
            // A new stream (the page switched camera tracks keep it; a new peer does not).
            s.media_ssrc = ssrc;
            s.have_media = true;
            s.waiting.clear();
            s.asked.clear();
            s.next = s.highest = s.base = -1;
            s.received = s.expected_prior = s.received_prior = 0;
            s.depacketizer = Depacketizer();
        }
        std::size_t header = 12 + 4 * (packet[0] & 0x0F);
        if (packet[0] & 0x10) {
            if (packet.size() < header + 4) continue;
            header += 4 + 4 * static_cast<std::size_t>(get16(packet.data() + header + 2));
        }
        std::size_t end = packet.size();
        if (packet[0] & 0x20) end -= packet.back();  // Padding.
        if (header > end) continue;
        Packet p;
        p.seq = s.unwrap(get16(packet.data() + 2));
        p.ts = get32(packet.data() + 4);
        p.marker = packet[1] & 0x80;
        p.payload.assign(packet.begin() + static_cast<std::ptrdiff_t>(header), packet.begin() + static_cast<std::ptrdiff_t>(end));
        if (s.base < 0) s.base = s.next = p.seq;
        if (p.seq > s.highest) s.highest = p.seq;
        if (p.seq < s.next || s.waiting.count(p.seq)) continue;  // Late or repeated.
        ++s.received;
        s.asked.erase(p.seq);
        s.waiting.emplace(p.seq, std::move(p));
        while (!s.waiting.empty() && s.waiting.begin()->first == s.next) {
            s.depacketizer.add(s.waiting.begin()->second, unit);
            s.waiting.erase(s.waiting.begin());
            ++s.next;
            s.gap_since = {};
        }
        while (!s.asked.empty() && s.asked.begin()->first < s.next) s.asked.erase(s.asked.begin());
    }
    // Rec closed the camera: the page offers again at once instead of waiting out ICE.
    if (s.dtls.connected() && s.have_remote) {
        const Bytes alert = s.dtls.close_notify();
        s.socket->send(s.remote, alert.data(), alert.size());
    }
}

} // namespace rec::core
