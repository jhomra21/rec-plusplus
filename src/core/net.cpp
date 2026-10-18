#include "core/net.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#else
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string_view>

namespace rec::core {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
void close_socket(Socket s) { closesocket(s); }
#else
using Socket = int;
const Socket kNoSocket = -1;
void close_socket(Socket s) { ::close(s); }
#endif

constexpr int kIoTimeoutMs = 10000;

bool connect_within(Socket s, const sockaddr* address, int length, int timeout_ms) {
#ifdef _WIN32
    u_long blocking = 1;
    ioctlsocket(s, FIONBIO, &blocking);
    if (::connect(s, address, length) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        timeval wait{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select(0, nullptr, &writable, &failed, &wait) <= 0 || FD_ISSET(s, &failed)) return false;
    }
    int error = 0;
    int size = sizeof error;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) != 0 || error) return false;
    blocking = 0;
    ioctlsocket(s, FIONBIO, &blocking);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    if (::connect(s, address, static_cast<socklen_t>(length)) != 0) {
        if (errno != EINPROGRESS) return false;
        pollfd wait{s, POLLOUT, 0};
        if (poll(&wait, 1, timeout_ms) <= 0) return false;
    }
    int error = 0;
    socklen_t size = sizeof error;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error) return false;
    fcntl(s, F_SETFL, flags);
#endif
    return true;
}

void configure(Socket s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef _WIN32
    const DWORD timeout = kIoTimeoutMs;
#else
    const timeval timeout{kIoTimeoutMs / 1000, 0};
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
#endif
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
}

class TcpTransport final : public Transport {
public:
    TcpTransport() {
#ifdef _WIN32
        WSADATA data;
        winsock_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
    }
    ~TcpTransport() override {
        if (socket_ != kNoSocket) close_socket(socket_);
#ifdef _WIN32
        if (winsock_) WSACleanup();
#endif
    }

    bool started() const {
#ifdef _WIN32
        return winsock_;
#else
        return true;
#endif
    }

    bool connect(const std::string& host, const std::string& port, int timeout_ms, std::string& error) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* found = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || !found) {
            error = "could not resolve " + host;
            return false;
        }
        for (auto* a = found; a && socket_ == kNoSocket; a = a->ai_next) {
            const Socket s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == kNoSocket) continue;
            if (connect_within(s, a->ai_addr, static_cast<int>(a->ai_addrlen), timeout_ms)) socket_ = s;
            else close_socket(s);
        }
        freeaddrinfo(found);
        if (socket_ == kNoSocket) {
            error = "could not connect to " + host + ":" + port;
            return false;
        }
        configure(socket_);
        return true;
    }

    bool send(const std::uint8_t* data, std::size_t size) override {
        while (size) {
#ifdef _WIN32
            const int sent = ::send(socket_, reinterpret_cast<const char*>(data),
                                    static_cast<int>(std::min<std::size_t>(size, 1 << 20)), 0);
#else
#ifdef MSG_NOSIGNAL
            const ssize_t sent = ::send(socket_, data, size, MSG_NOSIGNAL);
#else
            const ssize_t sent = ::send(socket_, data, size, 0);
#endif
            if (sent < 0 && errno == EINTR) continue;
#endif
            if (sent <= 0) return false;
            data += sent;
            size -= static_cast<std::size_t>(sent);
        }
        return true;
    }

    std::size_t receive(std::uint8_t* data, std::size_t size) override {
        for (;;) {
#ifdef _WIN32
            const int got = ::recv(socket_, reinterpret_cast<char*>(data),
                                   static_cast<int>(std::min<std::size_t>(size, 1 << 20)), 0);
#else
            const ssize_t got = ::recv(socket_, data, size, 0);
            if (got < 0 && errno == EINTR) continue;
#endif
            return got > 0 ? static_cast<std::size_t>(got) : 0;
        }
    }

    bool readable() override {
#ifdef _WIN32
        fd_set set;
        FD_ZERO(&set);
        FD_SET(socket_, &set);
        timeval none{0, 0};
        return select(0, &set, nullptr, nullptr, &none) > 0;
#else
        pollfd wait{socket_, POLLIN, 0};
        return poll(&wait, 1, 0) > 0;
#endif
    }

    void interrupt() override {
#ifdef _WIN32
        shutdown(socket_, SD_BOTH);
#else
        shutdown(socket_, SHUT_RDWR);
#endif
    }

    void finish() override {
#ifdef _WIN32
        shutdown(socket_, SD_SEND);
#else
        shutdown(socket_, SHUT_WR);
#endif
    }

private:
    Socket socket_ = kNoSocket;
#ifdef _WIN32
    bool winsock_ = false;
#endif
};

std::string environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
    std::string text(value);
    free(value);
    return text;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

std::vector<std::uint8_t> base64(std::string_view text) {
    std::vector<std::uint8_t> out;
    std::uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else continue;
        bits = bits << 6 | static_cast<std::uint32_t>(v);
        if ((count += 6) >= 8) {
            count -= 8;
            out.push_back(static_cast<std::uint8_t>(bits >> count));
        }
    }
    return out;
}
} // namespace

std::unique_ptr<Transport> connect_tcp(const std::string& host, const std::string& port, int timeout_ms,
                                       std::string& error) {
    auto transport = std::make_unique<TcpTransport>();
    if (!transport->started()) { error = "could not start Windows networking"; return nullptr; }
    if (!transport->connect(host, port, timeout_ms, error)) return nullptr;
    return transport;
}

std::vector<std::uint8_t> extra_trusted_certificate() {
    const auto path = environment("REC_TLS_TRUST_CERT");
    if (path.empty()) return {};
    std::ifstream file(path, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::string begin = "-----BEGIN CERTIFICATE-----", end = "-----END CERTIFICATE-----";
    const auto from = data.find(begin);
    if (from == std::string::npos) return {data.begin(), data.end()};
    const auto to = data.find(end, from);
    return base64(std::string_view(data).substr(from + begin.size(), to == std::string::npos ? std::string::npos : to - from - begin.size()));
}

} // namespace rec::core
