#pragma once

#include "core/crypto.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rec::core {

// A phone (or any device with a browser) on the same network as a camera. The page that sends
// the phone's front or back camera over WebRTC comes from the relay (support/phone-relay) on a
// real HTTPS address, which also passes the page's offer to rec and rec's answer back over a
// WebSocket rec keeps open; or, without the relay, rec serves it over HTTPS on port 47110
// (REC_PHONE_PORT overrides it) with a self-signed certificate the person confirms once.
// Either way the video goes straight from the phone to rec. rec is the ICE-lite, DTLS-client
// end: it answers the page's offer, takes H.264 only, asks for lost packets (NACK) and
// keyframes (PLI), and hands each complete access unit to a sink. The URL's fragment carries a
// random token, so only a person who saw it can send; it signs offers and answers on the relay.
//
// Cameras with the same token share one session; the server runs while any camera is open.
// When a session ends (rec restarts the camera, the phone sleeps) the page offers again.
class PhoneCamera {
public:
    using Sink = std::function<void(const std::uint8_t* annexb, std::size_t size, bool key)>;

    ~PhoneCamera();
    PhoneCamera(const PhoneCamera&) = delete;
    PhoneCamera& operator=(const PhoneCamera&) = delete;

    static std::uint16_t port();
    // 22 random letters and digits.
    static std::string new_token();
    // The relay: REC_PHONE_RELAY, else rec's own. Empty when REC_PHONE_RELAY is "off".
    static std::string relay();
    // The page's address: on the relay, or local_url() without one.
    static std::string url(const std::string& token);
    // The page's address on this computer's network address. Empty when offline.
    static std::string local_url(const std::string& token);
    // Listens if no other camera does, and connects to the relay. Null with error set when
    // the port is taken.
    static std::shared_ptr<PhoneCamera> open(const std::string& token, std::string& error);

    // Receives each complete H.264 access unit (Annex B), in order, on the network thread.
    // Units before the first keyframe, and after a loss until the next one, are not passed on.
    void set_sink(Sink sink);
    // The decoder lost track: ask the phone for a keyframe.
    void request_keyframe();
    // Media arrived in the last two seconds.
    bool connected() const;
    std::uint64_t frames() const noexcept { return frames_.load(); }

    struct Session;
    struct Server;
    struct Relay;
    // For the server: answers the page's SDP offer from peer_ip. Empty answer with error set
    // when the offer cannot be served.
    std::string answer(const std::string& offer, const std::string& peer_ip, std::string& error);

private:
    explicit PhoneCamera(std::string token);
    void deliver(const crypto::Bytes& unit, bool key);
    void run(Session& session);

    std::string token_;
    std::shared_ptr<Server> server_;
    std::mutex mutex_, answer_mutex_;
    Sink sink_;
    std::unique_ptr<Session> session_;
    std::thread thread_;
    std::atomic_bool stop_{false}, keyframe_wanted_{false};
    std::atomic<std::int64_t> last_media_ns_{0};
    std::atomic<std::uint64_t> frames_{0};
    std::shared_ptr<Relay> relay_;
};

} // namespace rec::core
