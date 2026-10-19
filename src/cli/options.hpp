#pragma once

#include "rec/rec.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What `rec record` and the engine (`rec serve`) share: the record options, the control
// vocabulary that changes a running recording, and the texts that report on it.
namespace rec::cli {

// Record options, as on the `rec record` command line (without "record").
bool parse_options(const std::vector<std::string>& args, Options& options, std::string& error);

enum class Action { toggle, pause, resume, stop, save_replay, show, hide, flip, move, place, audio };
inline constexpr std::size_t kScreen = SIZE_MAX;  // show, hide and move the main capture.

struct Request {
    Action action = Action::toggle;
    std::size_t layer = 0;      // show, hide, flip, move and place: 0-based overlay, or kScreen.
    std::optional<Area> area;   // move and place.
    bool visible = true;        // place.
    // audio: the source, and its new mute (none: toggle) or volume.
    AudioInput input = AudioInput::system;
    std::optional<bool> muted;
    std::optional<double> gain_db;
};

// One control line: pause, resume, toggle, save, show N|screen, hide N|screen,
// move N|screen X,Y,W,H, place N|screen 0|1 X,Y,W,H (visibility and area at once),
// mute system|mic, unmute system|mic, volume system|mic DB, level system|mic DB 0|1 (volume
// and mute at once), stop.
// Empty with error set when line is not one.
std::optional<Request> parse_request(std::string_view line, std::string& error);

// Applies requests other than stop to a running recording, and remembers what toggling
// needs: which layers show, each source's volume and mute, replays saved so far.
class Controls {
public:
    Controls(Recorder& recorder, const Options& options);
    // Describes the change ("Paused at 00:03.0") in message, or why it failed in the result.
    // recorded is the movie time so far, in seconds.
    Result apply(const Request& request, double recorded, std::string& message);
    // "system audio", "mic", both, or empty.
    std::string muted() const;

private:
    std::string layer_name(std::size_t layer) const;

    Recorder& recorder_;
    const Options& options_;
    std::vector<bool> visible_;
    bool screen_visible_ = true;
    double gains_[2]{};
    bool mutes_[2]{};
    bool has_input_[2]{};
    unsigned replays_ = 0;
};

// 00:12.3
std::string clock_text(double seconds);
// A stream URL with its key (the last path segment) hidden.
std::string redact(const std::string& url);
// The lines printed when a recording ends: frames, audio buffers, pause, stream.
std::string summary(const Options& options, const Stats& stats, double wall_seconds);

} // namespace rec::cli
