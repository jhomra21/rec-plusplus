#pragma once

#include "rec/rec.hpp"

#include <string>
#include <string_view>
#include <vector>

// The engine's machine-readable forms of a recording's Options and Stats, for clients that
// drive it with the library's whole vocabulary (the desktop apps) rather than the
// command-line flags. Every value is a key=value word; join() and split() carry them on one
// request line.
namespace rec::cli {

std::vector<std::string> encode_options(const Options& options);
// Rejects unknown keys and malformed values.
bool decode_options(const std::vector<std::string>& words, Options& options, std::string& error);

// One key=value line per field, each ending in a newline.
std::string encode_stats(const Stats& stats);
// Applies one line of encode_stats(); false when it is not one.
bool decode_stat(std::string_view key, std::string_view value, Stats& stats);

// text on one line: line ends become spaces.
std::string one_line(std::string text);

} // namespace rec::cli
