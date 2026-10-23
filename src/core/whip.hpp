#pragma once

#include "core/dtls.hpp"
#include "core/net.hpp"
#include "core/rtmp.hpp"
#include "core/srtp.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rec::core {

// Publishes one live stream to a WHIP endpoint (WebRTC-HTTP ingestion, RFC 9725): the SDP
// offer is POSTed to http(s)://host/path, with the text after '#' in the URL as the Bearer
// token, and H.264 plus Opus then go out as SRTP over one UDP socket after ICE and DTLS.
// The session is DELETEd when the stream ends.
//
// Same contract as RtmpPublisher: open() blocks while it connects; video() and audio() only
// queue (about two seconds; when the network falls behind, video is dropped until the next
// keyframe); one thread sends, paced on the media timestamps so packets leave as they were
// captured. A lost connection is retried with backoff for up to two minutes.
class WhipPublisher {
public:
    WhipPublisher();
    ~WhipPublisher();
    WhipPublisher(const WhipPublisher&) = delete;
    WhipPublisher& operator=(const WhipPublisher&) = delete;

    static bool is_whip(const std::string& url);
    std::string open(const std::string& url, const StreamInfo& info);  // Empty on success.
    // An FLV video tag body, as Broadcast hands RTMP: the AVC sequence header carries the
    // parameter sets, frames carry length-prefixed NAL units.
    bool video(std::uint32_t ms, bool key, bool header, const Bytes& body);
    // One 20 ms Opus packet starting at a 48 kHz sample index on the stream timeline.
    bool audio(std::int64_t sample, Bytes packet);
    void close(int flush_ms);
    // True once when the server asked for a keyframe (RTCP PLI or FIR), subject only to a short
    // feedback rate limit: the encoder should make the next frame a keyframe.
    bool take_keyframe_request();

    bool live() const noexcept { return live_.load(); }
    bool reconnecting() const noexcept { return reconnecting_.load(); }
    std::uint32_t reconnects() const noexcept { return reconnects_.load(); }
    std::uint64_t bytes() const noexcept { return bytes_.load(); }
    std::uint64_t dropped_frames() const noexcept { return dropped_.load(); }
    std::uint64_t pli_count() const noexcept { return pli_count_.load(); }
    std::uint64_t fir_count() const noexcept { return fir_count_.load(); }
    std::uint64_t nacked_packets() const noexcept { return nacked_packets_.load(); }
    std::uint64_t forced_keyframes() const noexcept { return forced_keyframes_.load(); }
    std::uint64_t send_failures() const noexcept { return send_failures_.load(); }
    std::int64_t server_lost() const noexcept { return server_lost_.load(); }
    std::uint32_t queued_ms() const;
    std::string failure() const;
    // The URL without its path or token, safe to print.
    static std::string redact(const std::string& url);

private:
    struct Media {
        bool video = false, key = false;
        std::uint32_t ms = 0;     // Presentation time.
        std::int64_t sample = 0;  // Audio: the first sample's index at 48 kHz.
        Bytes data;               // Video: NAL units, each with a 4-byte length. Audio: one Opus packet.
    };
    struct Session;

    std::string connect(int timeout_ms);
    void run();
    bool serve(Session& session);  // False when the connection was lost.
    // Under mutex_: the next media item once it is due, or when to look again.
    bool take_due(Session& session, std::chrono::steady_clock::time_point now, Media& item,
                  std::chrono::steady_clock::time_point& next);
    void send_video(Session& session, const Media& item);
    void send_audio(Session& session, const Media& item);
    void handle_datagram(Session& session, const std::uint8_t* data, std::size_t size, const Endpoint& from);
    void hang_up(Session& session, bool polite);
    void fail(std::string text);

    std::string url_, token_;
    StreamInfo info_;
    std::unique_ptr<Session> session_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Media> queue_;
    Bytes sps_, pps_;
    bool need_key_ = true;
    bool closing_ = false;
    std::chrono::steady_clock::time_point flush_deadline_{};
    std::string failure_;
    std::thread sender_;
    std::atomic_bool live_{false}, reconnecting_{false}, keyframe_wanted_{false}, refresh_wanted_{false};
    std::atomic<std::int64_t> last_key_ns_{0}, last_forced_ns_{0};
    std::atomic<std::uint32_t> reconnects_{0};
    std::atomic<std::uint64_t> bytes_{0}, dropped_{0};
    std::atomic<std::uint64_t> pli_count_{0}, fir_count_{0}, nacked_packets_{0}, forced_keyframes_{0}, send_failures_{0};
    std::atomic<std::int64_t> server_lost_{0};
};

} // namespace rec::core
