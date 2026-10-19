#include "cli/local_socket.hpp"

#include <cstring>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace rec::cli {
namespace {
#ifdef _WIN32
bool started() {
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

void remove_file(const std::string& path) { DeleteFileW(wide(path).c_str()); }
void close_handle(unsigned long long handle) { closesocket(static_cast<SOCKET>(handle)); }
#else
bool started() { return true; }
void remove_file(const std::string& path) { unlink(path.c_str()); }
void close_handle(int handle) { ::close(handle); }
#endif

// A peer that went away must fail send(), not raise SIGPIPE.
template <typename Handle>
void no_sigpipe([[maybe_unused]] Handle handle) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool address(const std::string& path, sockaddr_un& out, std::string& error) {
    std::memset(&out, 0, sizeof out);
    out.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof out.sun_path) {
        error = "the engine socket path is empty or too long: " + path;
        return false;
    }
    std::memcpy(out.sun_path, path.data(), path.size());
    return true;
}
} // namespace

LocalSocket::~LocalSocket() { close(); }

LocalSocket::LocalSocket(LocalSocket&& other) noexcept
    : handle_(std::exchange(other.handle_, kInvalid)), buffer_(std::move(other.buffer_)) {}

LocalSocket& LocalSocket::operator=(LocalSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, kInvalid);
        buffer_ = std::move(other.buffer_);
    }
    return *this;
}

void LocalSocket::close() {
    if (handle_ != kInvalid) close_handle(handle_);
    handle_ = kInvalid;
}

LocalSocket LocalSocket::listen(const std::string& path, std::string& error) {
    sockaddr_un where;
    if (!started()) { error = "could not start Windows sockets"; return {}; }
    if (!address(path, where, error)) return {};
    LocalSocket s(static_cast<Handle>(::socket(AF_UNIX, SOCK_STREAM, 0)));
    if (!s.valid()) { error = "could not create the engine socket"; return {}; }
    remove_file(path);
#ifndef _WIN32
    // The socket file takes the umask's permissions at bind; owner only.
    const mode_t previous = umask(077);
#endif
    const bool bound = ::bind(s.handle_, reinterpret_cast<const sockaddr*>(&where), sizeof where) == 0;
#ifndef _WIN32
    umask(previous);
#endif
    if (!bound || ::listen(s.handle_, 8) != 0) {
        error = "could not listen on " + path;
        return {};
    }
    return s;
}

LocalSocket LocalSocket::connect(const std::string& path, std::string& error) {
    sockaddr_un where;
    if (!started()) { error = "could not start Windows sockets"; return {}; }
    if (!address(path, where, error)) return {};
    LocalSocket s(static_cast<Handle>(::socket(AF_UNIX, SOCK_STREAM, 0)));
    if (!s.valid()) { error = "could not create a socket"; return {}; }
    no_sigpipe(s.handle_);
    if (::connect(s.handle_, reinterpret_cast<const sockaddr*>(&where), sizeof where) != 0) {
        error = "the engine is not running";
        return {};
    }
    return s;
}

bool LocalSocket::wait_readable(int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD p{static_cast<SOCKET>(handle_), POLLRDNORM, 0};
    return WSAPoll(&p, 1, timeout_ms) > 0;
#else
    pollfd p{handle_, POLLIN, 0};
    return ::poll(&p, 1, timeout_ms) > 0;
#endif
}

LocalSocket LocalSocket::accept(int timeout_ms) {
    if (!valid() || !wait_readable(timeout_ms)) return {};
    LocalSocket s(static_cast<Handle>(::accept(handle_, nullptr, nullptr)));
    if (s.valid()) no_sigpipe(s.handle_);
    return s;
}

bool LocalSocket::read_line(std::string& line, int timeout_ms, bool& timed_out) {
    timed_out = false;
    for (;;) {
        if (const auto end = buffer_.find('\n'); end != std::string::npos) {
            line = buffer_.substr(0, end);
            buffer_.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        if (!valid()) return false;
        if (!wait_readable(timeout_ms)) { timed_out = true; return false; }
        char chunk[4096];
        const auto n = ::recv(handle_, chunk, sizeof chunk, 0);
        if (n <= 0) return false;
        buffer_.append(chunk, static_cast<std::size_t>(n));
        // A request is one short line; refuse to buffer without bound.
        if (buffer_.size() > 1 << 20) return false;
    }
}

bool LocalSocket::write(std::string_view text) {
    while (valid() && !text.empty()) {
        const auto n = ::send(handle_, text.data(), static_cast<int>(text.size()), kSendFlags);
        if (n <= 0) return false;
        text.remove_prefix(static_cast<std::size_t>(n));
    }
    return valid();
}

} // namespace rec::cli
