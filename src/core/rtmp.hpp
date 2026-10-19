#pragma once

#include "core/net.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rec::core {

using Bytes = std::vector<std::uint8_t>;

// FLV tag bodies carried by RTMP audio (8) and video (9) messages.
namespace flv {
// One encoded H.264 access unit in Annex-B form, converted to length-prefixed NAL units.
// SPS, PPS and access-unit delimiters are removed from the body and returned separately.
struct AccessUnit {
    Bytes body;  // Complete FLV video tag body (frame type, AVC packet type 1, NAL units).
    Bytes sps, pps;
    bool key = false;
};
bool avc_frame(const std::uint8_t* data, std::size_t size, AccessUnit& out);
Bytes avc_sequence_header(const Bytes& sps, const Bytes& pps);
// Splits Annex-B parameter sets, e.g. MF_MT_MPEG_SEQUENCE_HEADER.
void parameter_sets(const std::uint8_t* data, std::size_t size, Bytes& sps, Bytes& pps);
Bytes aac_sequence_header(std::uint32_t rate, std::uint32_t channels);
Bytes aac_frame(const std::uint8_t* data, std::size_t size);
} // namespace flv

struct StreamInfo {
    std::uint32_t width = 0, height = 0, fps = 0;
    std::uint32_t video_kbps = 0, audio_kbps = 0;
    std::uint32_t audio_rate = 48000, audio_channels = 2;
};

// Publishes one live stream to an rtmp:// or rtmps:// URL. open() blocks while it connects;
// after that video() and audio() only queue, and a sender thread owns the connection. The
// queue holds about three seconds of media. When the network falls behind, new video is
// dropped until the next keyframe so the stream recovers cleanly; sequence headers are never
// dropped. When the connection drops, the sender reconnects with backoff for up to two
// minutes, sends the metadata and sequence headers again, and resumes at the next keyframe;
// media produced meanwhile is dropped.
class RtmpPublisher {
public:
    RtmpPublisher();
    ~RtmpPublisher();
    RtmpPublisher(const RtmpPublisher&) = delete;
    RtmpPublisher& operator=(const RtmpPublisher&) = delete;

    std::string open(const std::string& url, const StreamInfo& info);  // Empty on success.
    // False when the message was not queued: not live, or dropped to keep up.
    bool video(std::uint32_t ms, bool key, bool header, Bytes body);
    bool audio(std::uint32_t ms, bool header, Bytes body);
    // Sends what is queued for at most flush_ms, ends the stream, and disconnects.
    void close(int flush_ms);

    bool live() const noexcept { return live_.load(); }
    bool reconnecting() const noexcept { return reconnecting_.load(); }
    std::uint32_t reconnects() const noexcept { return reconnects_.load(); }
    std::uint64_t bytes() const noexcept { return bytes_.load(); }
    std::uint64_t dropped_frames() const noexcept { return dropped_.load(); }
    // Media time between the oldest unsent message and the newest queued one.
    std::uint32_t queued_ms() const;
    std::string failure() const;
    // The URL without its stream key, safe to print.
    static std::string redact(const std::string& url);

private:
    struct Packet {
        std::uint8_t type = 0;
        std::uint32_t ms = 0;
        Bytes body;
    };
    struct Incoming {
        std::uint32_t timestamp = 0, delta = 0, length = 0, stream = 0;
        std::uint8_t type = 0;
        bool extended = false;
        Bytes payload;
    };
    struct Message {
        std::uint8_t type = 0;
        Bytes data;
    };
    struct Command {
        std::string name;
        double transaction = 0;
        std::vector<double> numbers;  // Number arguments after the transaction ID, in order.
        std::map<std::string, std::string> info;  // String fields of object arguments.
    };

    // Connects, handshakes, publishes and sends the metadata. Empty on success.
    std::string connect(int timeout_ms);
    // After a lost connection: retries until connected (true), or closing or out of time.
    bool reconnect();
    void drop_connection();
    bool push(std::uint8_t type, std::uint32_t ms, bool header, bool key, Bytes body);
    void run();
    bool send_all(const std::uint8_t* data, std::size_t size);
    bool read_exact(std::uint8_t* data, std::size_t size);
    bool write_message(std::uint32_t csid, std::uint8_t type, std::uint32_t stream, std::uint32_t ms, const Bytes& body);
    bool read_message(Message& out);
    bool handle_control(const Message& message);
    // Waits for the _result or _error of a transaction, or with status set, the next onStatus.
    bool await_command(double transaction, bool status, Command& out, std::string& error);
    void fail(std::string text);

    // Set by open() and only read afterwards.
    std::string host_, port_, authority_, app_, key_;
    bool secure_ = false;
    StreamInfo info_;

    // Owned by the thread that connects: open(), then the sender, then close() after it ends.
    std::unique_ptr<Transport> transport_;
    std::uint32_t chunk_in_ = 128, chunk_out_ = 128;
    std::uint32_t window_ = 0;
    std::uint64_t received_ = 0, acknowledged_ = 0;
    std::uint32_t stream_id_ = 0;
    std::map<std::uint32_t, Incoming> incoming_;
    Bytes out_;
    std::mutex transport_mutex_;  // Guards replacing transport_ against close() interrupting it.

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Packet> queue_;
    std::size_t queued_bytes_ = 0, max_bytes_ = 0;
    bool need_key_ = false;
    bool resync_ = false;  // After a reconnect: audio waits for the video keyframe too.
    Bytes video_header_, audio_header_;
    std::uint32_t last_ms_ = 0;
    bool closing_ = false;
    bool sender_done_ = false;
    std::int64_t flush_deadline_ms_ = 0;
    std::string failure_;
    std::thread sender_;
    std::atomic_bool live_{false}, reconnecting_{false};
    std::atomic<std::uint32_t> reconnects_{0};
    std::atomic<std::uint64_t> bytes_{0}, dropped_{0};
};

// One stream encode sent to several servers. Each server has its own publisher, queue and
// reconnects, so a slow or lost server holds back only itself.
class Broadcast {
public:
    struct Counters {
        bool live = false, reconnecting = false;
        std::uint32_t reconnects = 0, queued_ms = 0;
        std::uint64_t frames = 0, dropped = 0, bytes = 0;  // frames: video frames queued to send.
    };

    Broadcast() = default;
    Broadcast(const Broadcast&) = delete;
    Broadcast& operator=(const Broadcast&) = delete;

    // Connects to every URL at once. Empty on success. If any server fails, none stays
    // connected and the error names that server when there are several.
    std::string open(const std::vector<std::string>& urls, const StreamInfo& info);
    std::size_t size() const noexcept { return servers_.size(); }
    void video(std::uint32_t ms, bool key, bool header, Bytes body);
    void audio(std::uint32_t ms, bool header, Bytes body);
    // Closes every server at once, each sending what is queued for at most flush_ms. The
    // counters stay readable afterwards.
    void close(int flush_ms);
    Counters counters(std::size_t server) const;
    // Why servers ended early, each named when there are several; empty when none did.
    std::string failure() const;

private:
    struct Server {
        std::string name;  // Redacted URL.
        RtmpPublisher publisher;
        std::atomic<std::uint64_t> frames{0};
    };
    std::vector<std::unique_ptr<Server>> servers_;  // Set by open() only.
};

} // namespace rec::core
