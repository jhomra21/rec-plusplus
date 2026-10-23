#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

namespace rec::core {

// Pauses on the engine's host clock. The output timeline is host time since the origin
// with every paused span removed, so video slots and audio map through the same cut.
// Units are the host clock's (100 ns on Windows, nanoseconds on macOS).
class PauseTimeline {
public:
    struct Point {
        std::int64_t output = 0;  // Inside a pause: where the pause began.
        bool inside = false;
    };
    struct Piece {
        std::int64_t skip = 0;    // Host units from the start of the span.
        std::int64_t length = 0;  // Host units.
        std::int64_t output = 0;  // Output time of the piece's first instant.
    };
    // A short audio span crosses at most a pause and a resume; more pieces are dropped.
    struct Pieces {
        std::array<Piece, 4> items{};
        std::size_t count = 0;
        const Piece* begin() const { return items.data(); }
        const Piece* end() const { return items.data() + count; }
    };

    void reset(std::int64_t origin) {
        std::lock_guard lock(mutex_);
        origin_ = origin;
        spans_.clear();
    }

    bool pause(std::int64_t now) {
        std::lock_guard lock(mutex_);
        if (open()) return false;
        if (!spans_.empty()) now = std::max(now, spans_.back().end);
        spans_.push_back({now, kOpen});
        return true;
    }

    bool resume(std::int64_t now) {
        std::lock_guard lock(mutex_);
        if (!open()) return false;
        spans_.back().end = std::max(now, spans_.back().begin);
        return true;
    }

    bool paused() const {
        std::lock_guard lock(mutex_);
        return open();
    }

    std::int64_t paused_total(std::int64_t now) const {
        std::lock_guard lock(mutex_);
        std::int64_t total = 0;
        for (const auto& span : spans_) total += std::max<std::int64_t>(0, std::min(span.end, now) - span.begin);
        return total;
    }

    Point to_output(std::int64_t host) const {
        std::lock_guard lock(mutex_);
        std::int64_t removed = 0;
        for (const auto& span : spans_) {
            if (host < span.begin) break;
            if (host < span.end) return {span.begin - origin_ - removed, true};
            removed += span.end - span.begin;
        }
        return {host - origin_ - removed, false};
    }

    // The host instant shown at an output time; empty while that time lies past an open pause.
    std::optional<std::int64_t> to_host(std::int64_t output) const {
        std::lock_guard lock(mutex_);
        std::int64_t host = origin_ + output;
        for (const auto& span : spans_) {
            if (host < span.begin) break;
            if (span.end == kOpen) return std::nullopt;
            host += span.end - span.begin;
        }
        return host;
    }

    // The parts of [host, host + length) outside every pause.
    Pieces kept(std::int64_t host, std::int64_t length) const {
        std::lock_guard lock(mutex_);
        Pieces pieces;
        auto add = [&](std::int64_t from, std::int64_t to, std::int64_t removed) {
            if (to > from && pieces.count < pieces.items.size())
                pieces.items[pieces.count++] = {from - host, to - from, from - origin_ - removed};
        };
        const std::int64_t end = host + length;
        std::int64_t cursor = host, removed = 0;
        for (const auto& span : spans_) {
            if (span.end <= cursor) { removed += span.end - span.begin; continue; }
            if (span.begin >= end) break;
            add(cursor, span.begin, removed);
            if (span.end >= end) return pieces;
            cursor = span.end;
            removed += span.end - span.begin;
        }
        add(cursor, end, removed);
        return pieces;
    }

private:
    static constexpr std::int64_t kOpen = std::numeric_limits<std::int64_t>::max();
    struct Span { std::int64_t begin, end; };

    bool open() const { return !spans_.empty() && spans_.back().end == kOpen; }

    mutable std::mutex mutex_;
    std::int64_t origin_ = 0;
    std::vector<Span> spans_;  // Grows by one per pause, on the controlling thread.
};

} // namespace rec::core
