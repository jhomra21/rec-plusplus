#include "core/replay.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace rec::core {
namespace {
constexpr std::uint32_t kAudioRate = 48000;
constexpr std::uint32_t kAacFrame = 1024;

void u8(Bytes& b, std::uint32_t v) { b.push_back(static_cast<std::uint8_t>(v)); }
void u16(Bytes& b, std::uint32_t v) { u8(b, v >> 8); u8(b, v); }
void u24(Bytes& b, std::uint32_t v) { u8(b, v >> 16); u16(b, v); }
void u32(Bytes& b, std::uint32_t v) { u16(b, v >> 16); u16(b, v); }
void zeros(Bytes& b, std::size_t n) { b.insert(b.end(), n, 0); }
void append(Bytes& b, const Bytes& more) { b.insert(b.end(), more.begin(), more.end()); }

Bytes box(const char* type, const Bytes& body) {
    Bytes b;
    u32(b, static_cast<std::uint32_t>(body.size() + 8));
    b.insert(b.end(), type, type + 4);
    append(b, body);
    return b;
}

Bytes full_box(const char* type, std::uint32_t flags, const Bytes& body) {
    Bytes b;
    u32(b, flags);  // Version 0.
    append(b, body);
    return box(type, b);
}

Bytes boxes(std::initializer_list<Bytes> parts) {
    Bytes b;
    for (const auto& part : parts) append(b, part);
    return b;
}

void matrix(Bytes& b) {
    for (std::uint32_t v : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u}) u32(b, v);
}

// One track's samples, laid out as one chunk.
struct Track {
    std::uint32_t id = 0, timescale = 0;
    std::uint64_t offset = 0;
    std::vector<std::uint32_t> sizes, deltas;
    std::vector<std::uint32_t> keys;  // 1-based sample numbers; empty means all are sync.
    std::uint32_t delay_ms = 0;       // Movie time before the track starts.
    std::uint32_t skip = 0;           // Media time cut from the start, in the track's timescale.

    std::uint64_t media_duration() const {
        std::uint64_t d = 0;
        for (auto v : deltas) d += v;
        return d;
    }
    std::uint32_t movie_duration() const {
        return delay_ms + static_cast<std::uint32_t>((media_duration() - std::min<std::uint64_t>(skip, media_duration())) * 1000 / timescale);
    }
};

Bytes sample_table(const Track& t, const Bytes& description) {
    Bytes stsd;
    u32(stsd, 1);
    append(stsd, description);
    Bytes stts;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
    for (auto d : t.deltas) {
        if (!runs.empty() && runs.back().second == d) ++runs.back().first;
        else runs.push_back({1, d});
    }
    u32(stts, static_cast<std::uint32_t>(runs.size()));
    for (const auto& [count, delta] : runs) { u32(stts, count); u32(stts, delta); }
    Bytes stsc;
    u32(stsc, 1);
    u32(stsc, 1);
    u32(stsc, static_cast<std::uint32_t>(t.sizes.size()));
    u32(stsc, 1);
    Bytes stsz;
    u32(stsz, 0);
    u32(stsz, static_cast<std::uint32_t>(t.sizes.size()));
    for (auto s : t.sizes) u32(stsz, s);
    Bytes co64;
    u32(co64, 1);
    u32(co64, static_cast<std::uint32_t>(t.offset >> 32));
    u32(co64, static_cast<std::uint32_t>(t.offset));
    Bytes table = boxes({full_box("stsd", 0, stsd), full_box("stts", 0, stts)});
    if (!t.keys.empty()) {
        Bytes stss;
        u32(stss, static_cast<std::uint32_t>(t.keys.size()));
        for (auto k : t.keys) u32(stss, k);
        append(table, full_box("stss", 0, stss));
    }
    append(table, boxes({full_box("stsc", 0, stsc), full_box("stsz", 0, stsz), full_box("co64", 0, co64)}));
    return box("stbl", table);
}

Bytes track_box(const Track& t, bool video, std::uint32_t width, std::uint32_t height, const Bytes& description) {
    const std::uint32_t duration = t.movie_duration();
    Bytes tkhd;
    zeros(tkhd, 8);  // Creation and modification times.
    u32(tkhd, t.id);
    zeros(tkhd, 4);
    u32(tkhd, duration);
    zeros(tkhd, 8);
    u16(tkhd, 0);                 // Layer.
    u16(tkhd, 0);                 // Alternate group.
    u16(tkhd, video ? 0 : 0x0100);  // Volume.
    u16(tkhd, 0);
    matrix(tkhd);
    u32(tkhd, video ? width << 16 : 0);
    u32(tkhd, video ? height << 16 : 0);

    Bytes elst;
    const bool delayed = t.delay_ms > 0;
    u32(elst, delayed ? 2 : 1);
    if (delayed) {
        u32(elst, t.delay_ms);
        u32(elst, 0xFFFFFFFF);  // An empty edit.
        u32(elst, 0x00010000);
    }
    u32(elst, duration - t.delay_ms);
    u32(elst, t.skip);
    u32(elst, 0x00010000);

    Bytes mdhd;
    zeros(mdhd, 8);
    u32(mdhd, t.timescale);
    u32(mdhd, static_cast<std::uint32_t>(t.media_duration()));
    u16(mdhd, 0x55C4);  // "und"
    u16(mdhd, 0);
    Bytes hdlr;
    u32(hdlr, 0);
    const char* kind = video ? "vide" : "soun";
    hdlr.insert(hdlr.end(), kind, kind + 4);
    zeros(hdlr, 12);
    const char* name = video ? "VideoHandler" : "SoundHandler";
    hdlr.insert(hdlr.end(), name, name + std::char_traits<char>::length(name) + 1);

    Bytes header;
    if (video) {
        zeros(header, 8);  // Graphics mode and opcolor.
    } else {
        zeros(header, 4);  // Balance.
    }
    Bytes dref;
    u32(dref, 1);
    append(dref, full_box("url ", 1, {}));
    const Bytes minf = box("minf", boxes({full_box(video ? "vmhd" : "smhd", video ? 1 : 0, header),
                                          box("dinf", full_box("dref", 0, dref)), sample_table(t, description)}));
    const Bytes mdia = box("mdia", boxes({full_box("mdhd", 0, mdhd), full_box("hdlr", 0, hdlr), minf}));
    return box("trak", boxes({full_box("tkhd", 3, tkhd), box("edts", full_box("elst", 0, elst)), mdia}));
}

Bytes avc1(std::uint32_t width, std::uint32_t height, const Bytes& config) {
    Bytes b;
    zeros(b, 6);
    u16(b, 1);  // Data reference index.
    zeros(b, 16);
    u16(b, width);
    u16(b, height);
    u32(b, 0x00480000);
    u32(b, 0x00480000);
    u32(b, 0);
    u16(b, 1);  // Frames per sample.
    zeros(b, 32);  // Compressor name.
    u16(b, 0x0018);
    u16(b, 0xFFFF);
    append(b, box("avcC", config));
    return box("avc1", b);
}

void descriptor(Bytes& b, std::uint8_t tag, const Bytes& body) {
    u8(b, tag);
    u8(b, static_cast<std::uint32_t>(body.size()));  // Every descriptor here is under 128 bytes.
    append(b, body);
}

Bytes mp4a(const Bytes& config, std::uint32_t kbps) {
    Bytes specific;
    descriptor(specific, 5, config);
    Bytes decoder;
    u8(decoder, 0x40);  // MPEG-4 audio.
    u8(decoder, 0x15);  // Audio stream.
    u24(decoder, 6144);
    u32(decoder, kbps * 1000);
    u32(decoder, kbps * 1000);
    append(decoder, specific);
    Bytes es;
    u16(es, 2);
    u8(es, 0);
    descriptor(es, 4, decoder);
    descriptor(es, 6, {0x02});
    Bytes esds;
    descriptor(esds, 3, es);

    Bytes b;
    zeros(b, 6);
    u16(b, 1);
    zeros(b, 8);
    u16(b, 2);   // Channels.
    u16(b, 16);  // Sample size.
    zeros(b, 4);
    u32(b, kAudioRate << 16);
    append(b, full_box("esds", 0, esds));
    return box("mp4a", b);
}
} // namespace

ReplayBuffer::ReplayBuffer(std::uint32_t seconds, std::uint32_t width, std::uint32_t height, std::uint32_t fps)
    : window_ms_(seconds * 1000), width_(width), height_(height), fps_(fps ? fps : 30),
      // Room for the window plus a keyframe interval at 50 Mb/s, the highest stream bitrate.
      max_bytes_((static_cast<std::size_t>(seconds) + 4) * 50'000'000 / 8) {}

void ReplayBuffer::video(std::uint32_t ms, bool key, bool header, const Bytes& body) {
    if (body.size() < 5) return;
    std::lock_guard lock(mutex_);
    if (header) {
        avc_config_.assign(body.begin() + 5, body.end());
        return;
    }
    if (video_.empty() && !key) return;
    video_.push_back({ms, key, Bytes(body.begin() + 5, body.end())});
    bytes_ += video_.back().data.size();
    trim();
}

void ReplayBuffer::audio(std::uint32_t ms, bool header, const Bytes& body) {
    if (body.size() < 2) return;
    std::lock_guard lock(mutex_);
    if (header) {
        aac_config_.assign(body.begin() + 2, body.end());
        return;
    }
    if (body.size() == 2) return;
    audio_.push_back({ms, true, Bytes(body.begin() + 2, body.end())});
    bytes_ += audio_.back().data.size();
    trim();
}

// Drops whole keyframe intervals from the front while the next one still covers the window.
void ReplayBuffer::trim() {
    if (video_.empty()) return;
    const std::uint32_t newest = video_.back().ms;
    const std::uint32_t cutoff = newest > window_ms_ ? newest - window_ms_ : 0;
    for (;;) {
        std::size_t next = 1;
        while (next < video_.size() && !video_[next].key) ++next;
        if (next == video_.size()) break;
        if (video_[next].ms > cutoff && bytes_ <= max_bytes_) break;
        for (std::size_t i = 0; i < next; ++i) {
            bytes_ -= video_.front().data.size();
            video_.pop_front();
        }
    }
    // Audio more than one AAC frame before the first video frame is never written.
    const std::uint32_t start = video_.front().ms;
    while (!audio_.empty() && audio_.front().ms + 30 < start) {
        bytes_ -= audio_.front().data.size();
        audio_.pop_front();
    }
}

double ReplayBuffer::seconds() const {
    std::lock_guard lock(mutex_);
    return video_.size() < 2 ? 0.0 : (video_.back().ms - video_.front().ms) / 1000.0;
}

void ReplayBuffer::set_priming(std::uint32_t frames) {
    std::lock_guard lock(mutex_);
    priming_ = frames;
}

std::string ReplayBuffer::save(const std::string& path, double& seconds) const {
    seconds = 0;
    std::deque<Packet> video, audio;
    Bytes avc, aac;
    std::uint32_t priming = 0;
    {
        std::lock_guard lock(mutex_);
        if (video_.size() < 2 || avc_config_.empty()) return "the replay buffer is still empty";
        video = video_;
        audio = audio_;
        avc = avc_config_;
        aac = aac_config_;
        priming = priming_;
    }
    if (aac.empty()) audio.clear();
    const std::uint32_t start = video.front().ms;
    while (!audio.empty() && audio.front().ms + kAacFrame * 1000 / kAudioRate / 2 < start) audio.pop_front();

    Track v{1, 1000};
    for (std::size_t i = 0; i < video.size(); ++i) {
        v.sizes.push_back(static_cast<std::uint32_t>(video[i].data.size()));
        v.deltas.push_back(i + 1 < video.size() ? std::max<std::uint32_t>(1, video[i + 1].ms - video[i].ms) : 1000 / fps_);
        if (video[i].key) v.keys.push_back(static_cast<std::uint32_t>(i + 1));
    }
    Track a{2, kAudioRate};
    for (const auto& p : audio) {
        a.sizes.push_back(static_cast<std::uint32_t>(p.data.size()));
        a.deltas.push_back(kAacFrame);
    }
    if (!audio.empty()) {
        // Skipping the priming too drops that much real audio, as an empty edit, but keeps
        // every player's timing the same.
        const auto priming_ms = (priming * 1000 + kAudioRate / 2) / kAudioRate;
        a.skip = priming;
        a.delay_ms = priming_ms;
        if (audio.front().ms >= start) a.delay_ms += audio.front().ms - start;
        else a.skip += (start - audio.front().ms) * kAudioRate / 1000;
    }

    const std::filesystem::path target(reinterpret_cast<const char8_t*>(path.c_str()));
    std::error_code exists;
    if (std::filesystem::exists(target, exists)) return path + " already exists";
    std::ofstream file(target, std::ios::binary);
    if (!file) return "could not create " + path;
    const auto put = [&file](const Bytes& b) { file.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); };
    Bytes ftyp;
    ftyp.insert(ftyp.end(), {'i', 's', 'o', 'm'});
    u32(ftyp, 0x200);
    for (const char* brand : {"isom", "iso2", "avc1", "mp41"}) ftyp.insert(ftyp.end(), brand, brand + 4);
    const Bytes head = box("ftyp", ftyp);
    std::uint64_t payload = 0;
    for (const auto& p : video) payload += p.data.size();
    for (const auto& p : audio) payload += p.data.size();
    // A 64-bit mdat header, so any size fits.
    Bytes mdat;
    u32(mdat, 1);
    mdat.insert(mdat.end(), {'m', 'd', 'a', 't'});
    u32(mdat, static_cast<std::uint32_t>((payload + 16) >> 32));
    u32(mdat, static_cast<std::uint32_t>(payload + 16));
    v.offset = head.size() + mdat.size();
    a.offset = v.offset;
    for (const auto& p : video) a.offset += p.data.size();

    put(head);
    put(mdat);
    for (const auto* track : {&video, &audio})
        for (const auto& p : *track) put(p.data);

    Bytes mvhd;
    zeros(mvhd, 8);
    u32(mvhd, 1000);
    const std::uint32_t duration = std::max(v.movie_duration(), audio.empty() ? 0u : a.movie_duration());
    u32(mvhd, duration);
    u32(mvhd, 0x00010000);
    u16(mvhd, 0x0100);
    zeros(mvhd, 10);
    matrix(mvhd);
    zeros(mvhd, 24);
    u32(mvhd, 3);
    Bytes moov_body = boxes({full_box("mvhd", 0, mvhd), track_box(v, true, width_, height_, avc1(width_, height_, avc))});
    if (!audio.empty()) append(moov_body, track_box(a, false, 0, 0, mp4a(aac, 160)));
    const Bytes moov = box("moov", moov_body);
    put(moov);
    file.close();
    if (!file) {
        std::filesystem::remove(target, exists);
        return "could not write " + path;
    }
    seconds = v.movie_duration() / 1000.0;
    return {};
}

} // namespace rec::core
