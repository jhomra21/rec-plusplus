#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

// The engine: one recording hosted by a background process (`rec serve`) that clients drive
// over a local socket, so the recording does not depend on the terminal or app that started
// it. Each request is one line; the reply is zero or more lines, then "ok[ TEXT]" or
// "error TEXT". Requests: start [--cwd DIR] RECORD-OPTIONS, stop, status, shutdown, and the
// control lines of `rec record` (pause, resume, toggle, save, show, hide, move, place, mute,
// unmute, volume, level). Programs that drive the whole library use: begin WORDS (the
// encode_options() words, absolute paths; replies id= and preview=, the shared memory of
// its preview frames), stats (state=, id=, the encode_stats() lines, finished=, failure=,
// stream_failure=), options (id=, preview=, options=WORDS with stream keys hidden),
// finish ID (stops recording ID or reports how it ended: statistics, failure=,
// stream_failure=, saved=), save-to PATH (replay; replies seconds=), preview on|off.
namespace rec::cli {

// REC_ENGINE_SOCKET, else a per-user path: ~/Library/Application Support/rec/engine.sock on
// macOS, %LOCALAPPDATA%\rec\engine.sock on Windows.
std::string engine_socket();

// Runs the engine in this process until a shutdown request or interrupted, or with
// idle_exit_seconds, until it has had no recording and no client for that long. Returns the
// exit status.
int serve(const std::string& socket, const std::atomic_bool& interrupted, int idle_exit_seconds = 0);

// Starts this executable with args, detached from this process, as the engine at socket. Its
// output goes to engine.log beside the socket; only the log is inherited, so the engine never
// holds the caller's pipes open.
bool launch_engine(const std::string& socket, const std::vector<std::string>& args, std::string& error);

// Sends one request and prints the reply: its lines to stdout, a final error to stderr.
// With launch, starts `rec serve` in the background when no engine answers. Returns the
// exit status: 0 for ok, 1 otherwise.
int request(const std::string& socket, const std::string& line, bool launch);

// Arguments as one request line; those with spaces, quotes or backslashes are quoted.
std::string join(const std::vector<std::string>& args);
// The arguments of a join() line.
std::vector<std::string> split(std::string_view line);

} // namespace rec::cli
