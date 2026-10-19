#pragma once

#include <atomic>
#include <string>
#include <vector>

// The engine: one recording hosted by a background process (`rec serve`) that clients drive
// over a local socket, so the recording does not depend on the terminal or app that started
// it. Each request is one line; the reply is zero or more lines, then "ok[ TEXT]" or
// "error TEXT". Requests: start [--cwd DIR] RECORD-OPTIONS, stop, status, shutdown, and the
// control lines of `rec record` (pause, resume, toggle, save, show, hide, move, mute,
// unmute, volume).
namespace rec::cli {

// REC_ENGINE_SOCKET, else a per-user path: ~/Library/Application Support/rec/engine.sock on
// macOS, %LOCALAPPDATA%\rec\engine.sock on Windows.
std::string engine_socket();

// Runs the engine in this process until a shutdown request or interrupted. Returns the exit status.
int serve(const std::string& socket, const std::atomic_bool& interrupted);

// Sends one request and prints the reply: its lines to stdout, a final error to stderr.
// With launch, starts `rec serve` in the background when no engine answers. Returns the
// exit status: 0 for ok, 1 otherwise.
int request(const std::string& socket, const std::string& line, bool launch);

// Arguments as one request line; those with spaces, quotes or backslashes are quoted.
std::string join(const std::vector<std::string>& args);

} // namespace rec::cli
