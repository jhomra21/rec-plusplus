#pragma once

#include <string>
#include <string_view>

// A Unix domain stream socket (AF_UNIX, also on Windows 10 1803 and later) for the engine's
// local control connection. Line-oriented: requests and replies are text lines.
namespace rec::cli {

class LocalSocket {
public:
    LocalSocket() = default;
    ~LocalSocket();
    LocalSocket(LocalSocket&& other) noexcept;
    LocalSocket& operator=(LocalSocket&& other) noexcept;
    LocalSocket(const LocalSocket&) = delete;
    LocalSocket& operator=(const LocalSocket&) = delete;

    // Binds path (replacing a stale socket file) and listens. Only this user may connect.
    static LocalSocket listen(const std::string& path, std::string& error);
    static LocalSocket connect(const std::string& path, std::string& error);

    bool valid() const { return handle_ != kInvalid; }
    // Waits up to timeout_ms for a connection; an invalid socket when none came.
    LocalSocket accept(int timeout_ms);
    // Reads one line without its end. Returns false on end or error; sets timed_out instead
    // when no complete line arrived within timeout_ms.
    bool read_line(std::string& line, int timeout_ms, bool& timed_out);
    bool write(std::string_view text);
    void close();

private:
#ifdef _WIN32
    using Handle = unsigned long long;
    static constexpr Handle kInvalid = ~0ull;
#else
    using Handle = int;
    static constexpr Handle kInvalid = -1;
#endif
    explicit LocalSocket(Handle handle) : handle_(handle) {}
    bool wait_readable(int timeout_ms);

    Handle handle_ = kInvalid;
    std::string buffer_;
};

} // namespace rec::cli
