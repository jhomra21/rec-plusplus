#include "core/mp4.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <vector>

namespace rec::core {
namespace {
using Bytes = std::vector<std::uint8_t>;

std::uint32_t r32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
}
std::uint64_t r64(const std::uint8_t* p) { return std::uint64_t{r32(p)} << 32 | r32(p + 4); }
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}
void put64(std::uint8_t* p, std::uint64_t v) {
    put32(p, static_cast<std::uint32_t>(v >> 32));
    put32(p + 4, static_cast<std::uint32_t>(v));
}
void w32(Bytes& b, std::uint32_t v) {
    b.resize(b.size() + 4);
    put32(b.data() + b.size() - 4, v);
}
void w64(Bytes& b, std::uint64_t v) {
    b.resize(b.size() + 8);
    put64(b.data() + b.size() - 8, v);
}
void append(Bytes& b, const std::uint8_t* data, std::size_t size) { b.insert(b.end(), data, data + size); }
void append(Bytes& b, const Bytes& more) { append(b, more.data(), more.size()); }

Bytes box(const char* type, const Bytes& body) {
    Bytes b;
    w32(b, static_cast<std::uint32_t>(body.size() + 8));
    append(b, reinterpret_cast<const std::uint8_t*>(type), 4);
    append(b, body);
    return b;
}
Bytes full_box(const char* type, std::uint32_t version, const Bytes& body) {
    Bytes b;
    w32(b, version << 24);
    append(b, body);
    return box(type, b);
}

struct Box {
    std::uint64_t offset = 0, size = 0, header = 8;
    char type[4]{};
    bool is(const char* name) const { return std::memcmp(type, name, 4) == 0; }
    std::uint64_t body() const { return offset + header; }
    std::uint64_t end() const { return offset + size; }
};

// The boxes in b[begin, end); stops at the first malformed one.
std::vector<Box> children(const Bytes& b, std::uint64_t begin, std::uint64_t end) {
    std::vector<Box> out;
    for (std::uint64_t o = begin; o + 8 <= end;) {
        Box x;
        x.offset = o;
        x.size = r32(&b[o]);
        std::memcpy(x.type, &b[o + 4], 4);
        if (x.size == 1) {
            if (o + 16 > end) break;
            x.size = r64(&b[o + 8]);
            x.header = 16;
        } else if (x.size == 0) {
            x.size = end - o;
        }
        if (x.size < x.header || x.size > end - o) break;
        out.push_back(x);
        o += x.size;
    }
    return out;
}

std::optional<Box> find(const std::vector<Box>& list, const char* type) {
    for (const auto& x : list)
        if (x.is(type)) return x;
    return std::nullopt;
}

struct Sample {
    std::uint32_t duration = 0, size = 0, flags = 0;
    std::int64_t offset = 0;  // Composition time minus decode time.
};

struct Track {
    std::uint32_t duration = 0, size = 0, flags = 0;  // trex defaults.
    std::vector<Sample> samples;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> chunks;  // File offset, samples.

    std::uint64_t media_duration() const {
        std::uint64_t total = 0;
        for (const auto& s : samples) total += s.duration;
        return total;
    }
};

constexpr std::uint32_t kNonSync = 0x10000;

// Reads one moof's samples into the tracks. False when it does not parse or its data lies
// outside the file.
bool read_fragment(const Bytes& moof, std::uint64_t at, std::uint64_t file_size, std::map<std::uint32_t, Track>& tracks) {
    std::uint64_t previous_end = at;
    for (const auto& traf : children(moof, 8, moof.size())) {
        if (!traf.is("traf")) continue;
        const auto parts = children(moof, traf.body(), traf.end());
        const auto tfhd = find(parts, "tfhd");
        if (!tfhd || tfhd->size < tfhd->header + 8) return false;
        const std::uint8_t* p = &moof[tfhd->body()];
        const std::uint32_t flags = r32(p) & 0xFFFFFF;
        auto found = tracks.find(r32(p + 4));
        if (found == tracks.end()) return false;
        Track& track = found->second;
        std::uint32_t duration = track.duration, size = track.size, sample_flags = track.flags;
        // Explicit, the moof's start, or (implicitly) where the previous traf's data ended.
        std::uint64_t base = flags & 0x20000 ? at : previous_end;
        std::size_t field = 8;
        const auto take = [&](std::size_t bytes) {
            const std::size_t here = field;
            field += bytes;
            return tfhd->header + field <= tfhd->size ? p + here : nullptr;
        };
        if (flags & 0x1) { if (auto v = take(8)) base = r64(v); else return false; }
        if (flags & 0x2) take(4);
        if (flags & 0x8) { if (auto v = take(4)) duration = r32(v); else return false; }
        if (flags & 0x10) { if (auto v = take(4)) size = r32(v); else return false; }
        if (flags & 0x20) { if (auto v = take(4)) sample_flags = r32(v); else return false; }

        std::uint64_t position = base;
        for (const auto& trun : parts) {
            if (!trun.is("trun")) continue;
            if (trun.size < trun.header + 8) return false;
            const std::uint8_t* q = &moof[trun.body()];
            const std::uint8_t* const q_end = &moof[0] + trun.end();
            const std::uint8_t version = q[0];
            const std::uint32_t run_flags = r32(q) & 0xFFFFFF;
            const std::uint32_t count = r32(q + 4);
            q += 8;
            if (run_flags & 0x1) {
                if (q + 4 > q_end) return false;
                position = base + static_cast<std::int64_t>(static_cast<std::int32_t>(r32(q)));
                q += 4;
            }
            std::uint32_t first_flags = sample_flags;
            const bool has_first = run_flags & 0x4;
            if (has_first) {
                if (q + 4 > q_end) return false;
                first_flags = r32(q);
                q += 4;
            }
            const std::size_t per_sample = 4 * (((run_flags >> 8) & 1) + ((run_flags >> 9) & 1) + ((run_flags >> 10) & 1) + ((run_flags >> 11) & 1));
            if (static_cast<std::uint64_t>(q_end - q) < static_cast<std::uint64_t>(per_sample) * count) return false;
            const std::uint64_t start = position;
            for (std::uint32_t i = 0; i < count; ++i) {
                Sample s{duration, size, i == 0 && has_first ? first_flags : sample_flags, 0};
                if (run_flags & 0x100) { s.duration = r32(q); q += 4; }
                if (run_flags & 0x200) { s.size = r32(q); q += 4; }
                if (run_flags & 0x400) { s.flags = r32(q); q += 4; }
                if (run_flags & 0x800) {
                    s.offset = version ? static_cast<std::int64_t>(static_cast<std::int32_t>(r32(q))) : static_cast<std::int64_t>(r32(q));
                    q += 4;
                }
                position += s.size;
                track.samples.push_back(s);
            }
            if (position > file_size) return false;
            if (count) track.chunks.emplace_back(start, count);
        }
        previous_end = position;
    }
    return true;
}

Bytes sample_table(const Bytes& stsd, const Track& t) {
    Bytes stts, ctts, stss, stsz, stsc, co64;
    std::uint32_t runs = 0;
    for (std::size_t i = 0; i < t.samples.size();) {
        std::size_t j = i;
        while (j < t.samples.size() && t.samples[j].duration == t.samples[i].duration) ++j;
        w32(stts, static_cast<std::uint32_t>(j - i));
        w32(stts, t.samples[i].duration);
        ++runs;
        i = j;
    }
    Bytes out = stsd;
    Bytes body;
    w32(body, runs);
    append(body, stts);
    append(out, full_box("stts", 0, body));

    const bool offsets = std::any_of(t.samples.begin(), t.samples.end(), [](const Sample& s) { return s.offset != 0; });
    if (offsets) {
        const bool negative = std::any_of(t.samples.begin(), t.samples.end(), [](const Sample& s) { return s.offset < 0; });
        runs = 0;
        for (std::size_t i = 0; i < t.samples.size();) {
            std::size_t j = i;
            while (j < t.samples.size() && t.samples[j].offset == t.samples[i].offset) ++j;
            w32(ctts, static_cast<std::uint32_t>(j - i));
            w32(ctts, static_cast<std::uint32_t>(t.samples[i].offset));
            ++runs;
            i = j;
        }
        body.clear();
        w32(body, runs);
        append(body, ctts);
        append(out, full_box("ctts", negative ? 1 : 0, body));
    }

    // No stss means every sample is a sync sample.
    if (std::any_of(t.samples.begin(), t.samples.end(), [](const Sample& s) { return s.flags & kNonSync; })) {
        std::uint32_t keys = 0;
        for (std::size_t i = 0; i < t.samples.size(); ++i)
            if (!(t.samples[i].flags & kNonSync)) { w32(stss, static_cast<std::uint32_t>(i + 1)); ++keys; }
        body.clear();
        w32(body, keys);
        append(body, stss);
        append(out, full_box("stss", 0, body));
    }

    // One entry wherever the samples per chunk change.
    for (std::size_t i = 0; i < t.chunks.size(); ++i) {
        if (i && t.chunks[i].second == t.chunks[i - 1].second) continue;
        w32(stsc, static_cast<std::uint32_t>(i + 1));
        w32(stsc, t.chunks[i].second);
        w32(stsc, 1);
    }
    body.clear();
    w32(body, static_cast<std::uint32_t>(stsc.size() / 12));
    append(body, stsc);
    append(out, full_box("stsc", 0, body));

    body.clear();
    w32(body, 0);
    w32(body, static_cast<std::uint32_t>(t.samples.size()));
    for (const auto& s : t.samples) w32(body, s.size);
    append(out, full_box("stsz", 0, body));

    body.clear();
    w32(body, static_cast<std::uint32_t>(t.chunks.size()));
    for (const auto& chunk : t.chunks) w64(body, chunk.first);
    append(out, full_box("co64", 0, body));
    return box("stbl", out);
}

// Durations live at a version-dependent offset in mvhd, tkhd and mdhd.
void set_duration(Bytes& b, const Box& x, std::size_t v0, std::size_t v1, std::uint64_t duration) {
    std::uint8_t* p = &b[x.body()];
    if (p[0] == 1) { if (x.header + v1 + 8 <= x.size) put64(p + v1, duration); }
    else if (x.header + v0 + 4 <= x.size) put32(p + v0, static_cast<std::uint32_t>(std::min<std::uint64_t>(duration, 0xFFFFFFFF)));
}
std::uint32_t timescale(const Bytes& b, const Box& x) {
    const std::uint8_t* p = &b[x.body()];
    const std::size_t at = p[0] == 1 ? 20 : 12;
    return x.header + at + 4 <= x.size ? r32(p + at) : 0;
}
std::uint32_t track_id(const Bytes& b, const Box& tkhd) {
    const std::uint8_t* p = &b[tkhd.body()];
    const std::size_t at = p[0] == 1 ? 20 : 12;
    return tkhd.header + at + 4 <= tkhd.size ? r32(p + at) : 0;
}

Bytes copy(const Bytes& b, const Box& x) { return Bytes(b.begin() + static_cast<std::ptrdiff_t>(x.offset), b.begin() + static_cast<std::ptrdiff_t>(x.end())); }

// The moov with real sample tables and durations, and without mvex. Empty when it cannot.
Bytes rebuild(Bytes moov, std::map<std::uint32_t, Track>& tracks) {
    const auto top = children(moov, 8, moov.size());
    const auto mvhd = find(top, "mvhd");
    if (!mvhd) return {};
    const std::uint32_t movie_scale = timescale(moov, *mvhd);
    if (!movie_scale) return {};
    std::uint64_t movie_duration = 0;
    Bytes body;
    for (const auto& x : top) {
        if (x.is("mvex") || x.is("mvhd")) continue;
        if (!x.is("trak")) { append(body, copy(moov, x)); continue; }
        const auto trak = children(moov, x.body(), x.end());
        const auto tkhd = find(trak, "tkhd");
        const auto mdia = find(trak, "mdia");
        if (!tkhd || !mdia) return {};
        auto found = tracks.find(track_id(moov, *tkhd));
        if (found == tracks.end()) return {};
        const Track& t = found->second;
        const auto media = children(moov, mdia->body(), mdia->end());
        const auto mdhd = find(media, "mdhd");
        const auto minf = find(media, "minf");
        if (!mdhd || !minf) return {};
        const std::uint32_t media_scale = timescale(moov, *mdhd);
        if (!media_scale) return {};
        const std::uint64_t media_duration = t.media_duration();
        const std::uint64_t duration = media_duration * movie_scale / media_scale;
        movie_duration = std::max(movie_duration, duration);
        set_duration(moov, *tkhd, 20, 28, duration);
        set_duration(moov, *mdhd, 16, 24, media_duration);

        Bytes minf_body;
        for (const auto& m : children(moov, minf->body(), minf->end())) {
            if (!m.is("stbl")) { append(minf_body, copy(moov, m)); continue; }
            const auto stsd = find(children(moov, m.body(), m.end()), "stsd");
            if (!stsd) return {};
            append(minf_body, sample_table(copy(moov, *stsd), t));
        }
        Bytes mdia_body;
        for (const auto& m : media) append(mdia_body, m.is("minf") ? box("minf", minf_body) : copy(moov, m));
        Bytes trak_body;
        for (const auto& m : trak) append(trak_body, m.is("mdia") ? box("mdia", mdia_body) : copy(moov, m));
        append(body, box("trak", trak_body));
    }
    set_duration(moov, *mvhd, 16, 24, movie_duration);
    Bytes out = copy(moov, *mvhd);
    append(out, body);
    return box("moov", out);
}
}  // namespace

std::string consolidate_fragments(const std::string& path) {
    const std::filesystem::path target(std::u8string(path.begin(), path.end()));
    std::error_code error;
    const std::uint64_t file_size = std::filesystem::file_size(target, error);
    if (error) return "could not read the movie's size";
    std::fstream file(target, std::ios::in | std::ios::out | std::ios::binary);
    if (!file) return "could not open the movie";
    const auto read = [&](std::uint64_t at, std::size_t size) {
        Bytes b(size);
        file.seekg(static_cast<std::streamoff>(at));
        file.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(size));
        if (!file) b.clear();
        return b;
    };

    std::vector<Box> top;
    for (std::uint64_t o = 0; o + 8 <= file_size;) {
        Bytes head = read(o, static_cast<std::size_t>(std::min<std::uint64_t>(16, file_size - o)));
        if (head.size() < 8) return "could not read the movie";
        Box x;
        x.offset = o;
        x.size = r32(head.data());
        std::memcpy(x.type, &head[4], 4);
        if (x.size == 1 && head.size() == 16) { x.size = r64(&head[8]); x.header = 16; }
        // A box running to the end of the file would swallow the moov appended after it.
        if (x.size < x.header || x.size > file_size - o) return "the movie's boxes do not add up";
        top.push_back(x);
        o += x.size;
    }
    const auto moov = find(top, "moov");
    if (!moov || !find(top, "moof")) return {};
    if (moov->size > 64u << 20) return "the movie header is too large";

    Bytes header = read(moov->offset, static_cast<std::size_t>(moov->size));
    if (header.empty()) return "could not read the movie header";
    std::map<std::uint32_t, Track> tracks;
    const auto parts = children(header, moov->header, header.size());
    for (const auto& x : parts) {
        if (!x.is("trak")) continue;
        const auto tkhd = find(children(header, x.body(), x.end()), "tkhd");
        if (tkhd) tracks[track_id(header, *tkhd)];
    }
    if (const auto mvex = find(parts, "mvex"))
        for (const auto& trex : children(header, mvex->body(), mvex->end())) {
            if (!trex.is("trex") || trex.size < trex.header + 24) continue;
            const std::uint8_t* p = &header[trex.body()];
            auto found = tracks.find(r32(p + 4));
            if (found == tracks.end()) continue;
            found->second.duration = r32(p + 12);
            found->second.size = r32(p + 16);
            found->second.flags = r32(p + 20);
        }
    for (const auto& x : top) {
        if (!x.is("moof")) continue;
        if (x.size > 16u << 20) return "a fragment header is too large";
        const Bytes moof = read(x.offset, static_cast<std::size_t>(x.size));
        if (moof.empty() || !read_fragment(moof, x.offset, file_size, tracks)) return "a fragment does not parse";
    }
    for (const auto& [id, t] : tracks)
        if (t.samples.empty()) return "a track has no samples";
    const Bytes consolidated = rebuild(header, tracks);
    if (consolidated.empty()) return "could not build the movie header";

    // The complete moov first: until the old one is freed, readers still see the fragments.
    file.clear();
    file.seekp(static_cast<std::streamoff>(file_size));
    file.write(reinterpret_cast<const char*>(consolidated.data()), static_cast<std::streamsize>(consolidated.size()));
    file.flush();
    for (const auto& x : top) {
        if (!x.is("moov") && !x.is("moof") && !x.is("mfra")) continue;
        file.seekp(static_cast<std::streamoff>(x.offset + 4));
        file.write("free", 4);
        if (x.is("moov")) file.flush();
    }
    file.flush();
    return file ? std::string() : "could not write the movie header";
}

}  // namespace rec::core
