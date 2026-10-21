#pragma once

#include "cli/shared_frame.hpp"
#include "rec/rec.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// A recording hosted by the engine (cli/engine.hpp) and driven with rec::Recorder's
// vocabulary, so it outlives the process that started it. Preview frames come through
// shared memory; everything else is one request per call, so a call never waits behind
// another client's.
namespace rec::cli {

class EngineRecorder {
public:
    // start() launches this executable with launch_args as the engine when none answers at
    // socket.
    EngineRecorder(Options options, std::string socket, std::vector<std::string> launch_args);
    // The recording the engine at socket runs now, or null (error says why when the engine
    // could not be asked). options receives its options, stream keys hidden.
    static std::unique_ptr<EngineRecorder> attach(const std::string& socket, Options& options, std::string& error);

    EngineRecorder(const EngineRecorder&) = delete;
    EngineRecorder& operator=(const EngineRecorder&) = delete;

    Result start();
    // Stops and saves the recording, or learns how it ended when it already has.
    Result stop();
    Result pause();
    Result resume();
    // As of the last stats(): the recording ended (its duration, a failure, another client
    // stopped it) or the engine went away.
    bool finished() const;
    Stats stats();
    std::string failure() const;
    std::string stream_failure() const;
    Result save_replay(const std::string& path, double* seconds = nullptr);
    Result set_overlay(std::size_t index, bool visible, std::optional<Area> area = std::nullopt);
    Result set_screen(bool visible, std::optional<Area> area = std::nullopt);
    Result set_volume(AudioInput input, double gain_db, bool muted);
    Result set_layout(std::size_t canvas, std::size_t layer, const Layout& layout);
    Result set_style(std::size_t index, const Style& style);
    Result set_color(ColorTarget target, const ColorAdjust& color);
    bool preview(PreviewFrame& frame) const;
    bool preview(PreviewFrame& frame, std::size_t canvas) const;
    void show_preview(bool on);

private:
    struct Reply {
        bool ok = false;
        std::string text;                // After "ok " or "error ".
        std::vector<std::string> lines;  // Before the final line.
    };
    EngineRecorder(std::string socket, std::uint64_t id, const std::string& preview, const std::string& preview2);
    // timeout_ms bounds the wait for the reply; 0 waits as long as the engine is there.
    Reply exchange(const std::string& line, int timeout_ms = 0) const;
    Result simple(const std::string& line) const;
    void open_preview(const std::string& name, const std::string& second);
    void read_ending(const Reply& reply);

    Options options_;
    std::string socket_;
    std::vector<std::string> launch_args_;
    std::uint64_t id_ = 0;
    mutable std::mutex mutex_;  // Guards the fields below.
    SharedFrame preview_, preview2_;  // The second canvas's in preview2_.
    Stats stats_;
    bool finished_ = false, stopped_ = false;
    std::string failure_, stream_failure_;
};

} // namespace rec::cli
