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

// One track's samples, in chunks of consecutive samples.
struct Track {
    std::uint32_t id = 0, timescale = 0;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> chunks;  // File offset, samples.
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

Bytes sample_table(const Track& t, bool audio, const Bytes& description) {
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
    Bytes stsc, entries;
    std::uint32_t runs_of_chunks = 0;
    for (std::size_t i = 0; i < t.chunks.size(); ++i) {
        if (i && t.chunks[i].second == t.chunks[i - 1].second) continue;
        u32(entries, static_cast<std::uint32_t>(i + 1));
        u32(entries, t.chunks[i].second);
        u32(entries, 1);
        ++runs_of_chunks;
    }
    u32(stsc, runs_of_chunks);
    append(stsc, entries);
    Bytes stsz;
    u32(stsz, 0);
    u32(stsz, static_cast<std::uint32_t>(t.sizes.size()));
    for (auto s : t.sizes) u32(stsz, s);
    Bytes co64;
    u32(co64, static_cast<std::uint32_t>(t.chunks.size()));
    for (const auto& chunk : t.chunks) {
        u32(co64, static_cast<std::uint32_t>(chunk.first >> 32));
        u32(co64, static_cast<std::uint32_t>(chunk.first));
    }
    Bytes table = boxes({full_box("stsd", 0, stsd), full_box("stts", 0, stts)});
    if (!t.keys.empty()) {
        Bytes stss;
        u32(stss, static_cast<std::uint32_t>(t.keys.size()));
        for (auto k : t.keys) u32(stss, k);
        append(table, full_box("stss", 0, stss));
    }
    append(table, boxes({full_box("stsc", 0, stsc), full_box("stsz", 0, stsz), full_box("co64", 0, co64)}));
    if (audio && !t.sizes.empty()) {
        // Every AAC frame needs the one before it, as AVAssetWriter declares. Without this group
        // AVFoundation also drops the encoder delay from the clip's first frames and plays the
        // audio about 44 ms early.
        Bytes sgpd;
        sgpd.insert(sgpd.end(), {'r', 'o', 'l', 'l'});
        u32(sgpd, 2);  // Entry length.
        u32(sgpd, 1);
        u16(sgpd, 0xFFFF);  // Roll distance -1.
        Bytes sbgp;
        sbgp.insert(sbgp.end(), {'r', 'o', 'l', 'l'});
        u32(sbgp, 1);
        u32(sbgp, static_cast<std::uint32_t>(t.sizes.size()));
        u32(sbgp, 1);
        append(table, boxes({full_box("sgpd", 0x01000000, sgpd), full_box("sbgp", 0, sbgp)}));  // sgpd is version 1.
    }
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
    u32(elst, duration > t.delay_ms ? duration - t.delay_ms : 0);  // 0 in a fragmented movie: all of it.
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
                                          box("dinf", full_box("dref", 0, dref)), sample_table(t, !video, description)}));
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
Bytes file_type() {
    Bytes ftyp;
    ftyp.insert(ftyp.end(), {'i', 's', 'o', 'm'});
    u32(ftyp, 0x200);
    for (const char* brand : {"isom", "iso2", "avc1", "mp41"}) ftyp.insert(ftyp.end(), brand, brand + 4);
    return box("ftyp", ftyp);
}

Bytes movie_header(std::uint32_t duration_ms) {
    Bytes mvhd;
    zeros(mvhd, 8);
    u32(mvhd, 1000);
    u32(mvhd, duration_ms);
    u32(mvhd, 0x00010000);
    u16(mvhd, 0x0100);
    zeros(mvhd, 10);
    matrix(mvhd);
    zeros(mvhd, 24);
    u32(mvhd, 3);
    return full_box("mvhd", 0, mvhd);
}

// A complete moov for the video track and, when given, the audio track.
Bytes movie_box(const Track& v, const Track* a, std::uint32_t width, std::uint32_t height, const Bytes& avc, const Bytes& aac) {
    const std::uint32_t duration = std::max(v.movie_duration(), a ? a->movie_duration() : 0u);
    Bytes body = boxes({movie_header(duration), track_box(v, true, width, height, avc1(width, height, avc))});
    if (a) append(body, track_box(*a, false, 0, 0, mp4a(aac, 160)));
    return box("moov", body);
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
    // Audio more than the priming and one AAC frame before the first video frame is never written.
    const std::uint32_t start = video_.front().ms;
    while (!audio_.empty() && audio_.front().ms + priming_ms_ + 30 < start) {
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
    priming_ms_ = (frames * 1000 + kAudioRate - 1) / kAudioRate;
}

std::string ReplayBuffer::save(const std::string& path, double& seconds) const {
    seconds = 0;
    std::deque<Packet> video, audio;
    Bytes avc, aac;
    std::uint32_t priming_ms = 0;
    {
        std::lock_guard lock(mutex_);
        if (video_.size() < 2 || avc_config_.empty()) return "the replay buffer is still empty";
        video = video_;
        audio = audio_;
        avc = avc_config_;
        aac = aac_config_;
        priming_ms = priming_ms_;
    }
    if (aac.empty()) audio.clear();
    const std::uint32_t start = video.front().ms;
    // Start from the last packet at least the priming before the video, when there is one.
    while (audio.size() > 1 && audio[1].ms + priming_ms <= start) audio.pop_front();

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
        if (audio.front().ms >= start) a.delay_ms = audio.front().ms - start;
        else a.skip = (start - audio.front().ms) * kAudioRate / 1000;
    }

    const std::filesystem::path target(reinterpret_cast<const char8_t*>(path.c_str()));
    std::error_code exists;
    if (std::filesystem::exists(target, exists)) return path + " already exists";
    std::ofstream file(target, std::ios::binary);
    if (!file) return "could not create " + path;
    const auto put = [&file](const Bytes& b) { file.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); };
    const Bytes head = file_type();
    std::uint64_t payload = 0;
    for (const auto& p : video) payload += p.data.size();
    for (const auto& p : audio) payload += p.data.size();
    // A 64-bit mdat header, so any size fits.
    Bytes mdat;
    u32(mdat, 1);
    mdat.insert(mdat.end(), {'m', 'd', 'a', 't'});
    u32(mdat, static_cast<std::uint32_t>((payload + 16) >> 32));
    u32(mdat, static_cast<std::uint32_t>(payload + 16));
    std::uint64_t at = head.size() + mdat.size();
    v.chunks.push_back({at, static_cast<std::uint32_t>(video.size())});
    for (const auto& p : video) at += p.data.size();
    if (!audio.empty()) a.chunks.push_back({at, static_cast<std::uint32_t>(audio.size())});

    put(head);
    put(mdat);
    for (const auto* track : {&video, &audio})
        for (const auto& p : *track) put(p.data);

    put(movie_box(v, audio.empty() ? nullptr : &a, width_, height_, avc, aac));
    file.close();
    if (!file) {
        std::filesystem::remove(target, exists);
        return "could not write " + path;
    }
    seconds = v.movie_duration() / 1000.0;
    return {};
}


namespace {
constexpr std::uint32_t kKeyFlags = 0x02000000, kOtherFlags = 0x01010000;

void u64(Bytes& b, std::uint64_t v) { u32(b, static_cast<std::uint32_t>(v >> 32)); u32(b, static_cast<std::uint32_t>(v)); }

Bytes trex(std::uint32_t id, std::uint32_t flags) {
    Bytes b;
    u32(b, id);
    u32(b, 1);
    u32(b, 0);
    u32(b, 0);
    u32(b, flags);
    return full_box("trex", 0, b);
}
} // namespace

MovieWriter::MovieWriter(std::uint32_t width, std::uint32_t height, std::uint32_t fps)
    : width_(width), height_(height), fps_(fps ? fps : 30) {}

MovieWriter::~MovieWriter() = default;

std::string MovieWriter::open(const std::string& path) {
    const std::filesystem::path target(reinterpret_cast<const char8_t*>(path.c_str()));
    std::error_code exists;
    if (std::filesystem::exists(target, exists)) return path + " already exists";
    file_ = std::make_unique<std::fstream>(target, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
    if (!*file_) { file_.reset(); return "could not create " + path; }
    path_ = path;
    put(file_type());
    return {};
}

void MovieWriter::put(const Bytes& b) {
    if (!file_ || !error_.empty()) return;
    file_->write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!*file_) error_ = "could not write " + path_;
    at_ += b.size();
}

void MovieWriter::set_priming(std::uint32_t frames) { priming_ms_ = (frames * 1000 + kAudioRate - 1) / kAudioRate; }

void MovieWriter::video(std::uint32_t ms, bool key, bool header, const Bytes& body) {
    if (!file_ || body.size() < 5) return;
    if (header) {
        if (avc_config_.empty()) avc_config_.assign(body.begin() + 5, body.end());
        return;
    }
    if (video_.empty() && !begun_ && !key) return;
    // A keyframe a second or more after the buffered interval began closes it as a fragment.
    if (key && !video_.empty() && ms >= video_.front().ms + 1000 && (begun_ || begin()))
        fragment(video_.size(), ms, false);
    video_.push_back({ms, key, Bytes(body.begin() + 5, body.end())});
    frames_.fetch_add(1);
}

void MovieWriter::audio(std::uint32_t ms, bool header, const Bytes& body) {
    if (!file_ || body.size() < 2) return;
    if (header) {
        if (aac_config_.empty()) aac_config_.assign(body.begin() + 2, body.end());
        return;
    }
    if (body.size() == 2 || (begun_ && !with_audio_)) return;
    audio_.push_back({ms, true, Bytes(body.begin() + 2, body.end())});
}

// Writes the fragmented movie header once the video's parameters are known, and lines the
// audio up with the first video frame the way ReplayBuffer::save() does.
bool MovieWriter::begin() {
    if (avc_config_.empty() || video_.empty()) return false;
    begun_ = true;
    first_ms_ = video_.front().ms;
    with_audio_ = !aac_config_.empty() && !audio_.empty();
    if (with_audio_) {
        while (audio_.size() > 1 && audio_[1].ms + priming_ms_ <= first_ms_) audio_.pop_front();
        if (audio_.front().ms >= first_ms_) delay_ms_ = audio_.front().ms - first_ms_;
        else skip_ = (first_ms_ - audio_.front().ms) * kAudioRate / 1000;
    } else {
        audio_.clear();
    }
    Track v{1, 1000}, a{2, kAudioRate};
    a.delay_ms = delay_ms_;
    a.skip = skip_;
    Bytes body = boxes({movie_header(0), track_box(v, true, width_, height_, avc1(width_, height_, avc_config_))});
    if (with_audio_) append(body, track_box(a, false, 0, 0, mp4a(aac_config_, 160)));
    Bytes mvex = trex(1, kOtherFlags);
    if (with_audio_) append(mvex, trex(2, kKeyFlags));
    append(body, box("mvex", mvex));
    moov_at_ = at_;
    put(box("moov", body));
    return true;
}

// Writes the first video_count buffered frames, and the audio before next_ms (all of it when
// last), as one moof and mdat.
void MovieWriter::fragment(std::size_t video_count, std::uint32_t next_ms, bool last) {
    std::size_t audio_count = 0;
    while (audio_count < audio_.size() && (last || audio_[audio_count].ms < next_ms)) ++audio_count;
    if (!video_count && !audio_count) return;
    std::vector<std::uint32_t> deltas(video_count);
    std::uint64_t video_bytes = 0, audio_bytes = 0;
    for (std::size_t i = 0; i < video_count; ++i) {
        const std::uint32_t next = i + 1 < video_count ? video_[i + 1].ms : next_ms;
        deltas[i] = next > video_[i].ms ? next - video_[i].ms : std::max<std::uint32_t>(1, 1000 / fps_);
        video_bytes += video_[i].data.size();
    }
    for (std::size_t i = 0; i < audio_count; ++i) audio_bytes += audio_[i].data.size();

    // Sizes are fixed by the counts, so the data offsets can be computed before the boxes.
    // traf: its header, tfhd, tfdt, then trun's header, count, data offset and samples.
    const std::size_t video_traf = video_count ? 8 + 16 + 20 + 20 + video_count * 12 : 0;
    const std::size_t audio_traf = audio_count ? 8 + 16 + 20 + 20 + audio_count * 8 : 0;
    const std::size_t moof_size = 8 + 16 + video_traf + audio_traf;
    Bytes moof;
    Bytes mfhd;
    u32(mfhd, ++sequence_);
    append(moof, full_box("mfhd", 0, mfhd));
    std::uint32_t data_at = static_cast<std::uint32_t>(moof_size + 8);
    const auto traf = [&](std::uint32_t id, std::uint64_t time, std::size_t count, bool video) {
        Bytes tfhd, tfdt, trun;
        u32(tfhd, id);
        u64(tfdt, time);
        u32(trun, static_cast<std::uint32_t>(count));
        u32(trun, data_at);
        for (std::size_t i = 0; i < count; ++i) {
            if (video) {
                u32(trun, deltas[i]);
                u32(trun, static_cast<std::uint32_t>(video_[i].data.size()));
                u32(trun, video_[i].key ? kKeyFlags : kOtherFlags);
            } else {
                u32(trun, kAacFrame);
                u32(trun, static_cast<std::uint32_t>(audio_[i].data.size()));
            }
        }
        Bytes body = boxes({full_box("tfhd", 0x020000, tfhd), full_box("tfdt", 0x01000000, tfdt),
                            full_box("trun", video ? 0x000701 : 0x000301, trun)});
        append(moof, box("traf", body));
    };
    if (video_count) {
        traf(1, video_time_, video_count, true);
        data_at += static_cast<std::uint32_t>(video_bytes);
    }
    if (audio_count) traf(2, audio_time_, audio_count, false);
    const Bytes moof_box = box("moof", moof);
    if (moof_box.size() != moof_size) { error_ = "could not lay out a fragment of " + path_; return; }
    moofs_.push_back(at_);
    put(moof_box);
    Bytes mdat;
    u32(mdat, static_cast<std::uint32_t>(8 + video_bytes + audio_bytes));
    mdat.insert(mdat.end(), {'m', 'd', 'a', 't'});
    put(mdat);
    if (video_count) video_out_.chunks.push_back({at_, static_cast<std::uint32_t>(video_count)});
    for (std::size_t i = 0; i < video_count; ++i) {
        video_out_.sizes.push_back(static_cast<std::uint32_t>(video_[i].data.size()));
        video_out_.deltas.push_back(deltas[i]);
        if (video_[i].key) video_out_.keys.push_back(static_cast<std::uint32_t>(video_out_.sizes.size()));
        video_time_ += deltas[i];
        put(video_[i].data);
    }
    if (audio_count) audio_out_.chunks.push_back({at_, static_cast<std::uint32_t>(audio_count)});
    for (std::size_t i = 0; i < audio_count; ++i) {
        audio_out_.sizes.push_back(static_cast<std::uint32_t>(audio_[i].data.size()));
        audio_out_.deltas.push_back(kAacFrame);
        audio_time_ += kAacFrame;
        put(audio_[i].data);
    }
    video_.erase(video_.begin(), video_.begin() + static_cast<std::ptrdiff_t>(video_count));
    audio_.erase(audio_.begin(), audio_.begin() + static_cast<std::ptrdiff_t>(audio_count));
    if (file_ && error_.empty()) file_->flush();
}

std::string MovieWriter::finish() {
    if (!file_) return error_;
    if (!begun_ && !begin()) {
        file_.reset();
        std::error_code ignored;
        std::filesystem::remove(std::filesystem::path(reinterpret_cast<const char8_t*>(path_.c_str())), ignored);
        return "the second canvas's movie got no frames";
    }
    const std::uint32_t end_ms = video_.empty() ? 0 : video_.back().ms + std::max<std::uint32_t>(1, 1000 / fps_);
    fragment(video_.size(), end_ms, true);
    if (!error_.empty()) { file_.reset(); return error_; }
    Track v{1, 1000}, a{2, kAudioRate};
    v.chunks = video_out_.chunks;
    v.sizes = video_out_.sizes;
    v.deltas = video_out_.deltas;
    v.keys = video_out_.keys;
    a.chunks = audio_out_.chunks;
    a.sizes = audio_out_.sizes;
    a.deltas = audio_out_.deltas;
    a.delay_ms = delay_ms_;
    a.skip = skip_;
    put(movie_box(v, with_audio_ && !a.sizes.empty() ? &a : nullptr, width_, height_, avc_config_, aac_config_));
    file_->flush();
    // Until the old header is freed, readers still see the fragments.
    for (const auto at : moofs_) {
        file_->seekp(static_cast<std::streamoff>(at + 4));
        file_->write("free", 4);
    }
    file_->flush();
    file_->seekp(static_cast<std::streamoff>(moov_at_ + 4));
    file_->write("free", 4);
    file_->flush();
    const bool ok = static_cast<bool>(*file_);
    file_.reset();
    return ok && error_.empty() ? std::string() : "could not write " + path_;
}
} // namespace rec::core
