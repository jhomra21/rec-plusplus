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
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstring>
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

namespace {
void to_sockaddr(const Endpoint& e, sockaddr_storage& out, int& length) {
    std::memset(&out, 0, sizeof out);
    if (e.v6) {
        auto* a = reinterpret_cast<sockaddr_in6*>(&out);
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(e.port);
        std::memcpy(&a->sin6_addr, e.address.data(), 16);
        length = sizeof(sockaddr_in6);
    } else {
        auto* a = reinterpret_cast<sockaddr_in*>(&out);
        a->sin_family = AF_INET;
        a->sin_port = htons(e.port);
        std::memcpy(&a->sin_addr, e.address.data(), 4);
        length = sizeof(sockaddr_in);
    }
}
bool from_sockaddr(const sockaddr* s, Endpoint& out) {
    out = {};
    if (s->sa_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(s);
        out.v6 = true;
        std::memcpy(out.address.data(), &a->sin6_addr, 16);
        out.port = ntohs(a->sin6_port);
        return true;
    }
    if (s->sa_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(s);
        std::memcpy(out.address.data(), &a->sin_addr, 4);
        out.port = ntohs(a->sin_port);
        return true;
    }
    return false;
}
Socket as_socket(std::intptr_t s) { return static_cast<Socket>(s); }
} // namespace

bool parse_endpoint(const std::string& ip, std::uint16_t port, Endpoint& out) {
    out = {};
    out.port = port;
    if (inet_pton(AF_INET, ip.c_str(), out.address.data()) == 1) return true;
    if (inet_pton(AF_INET6, ip.c_str(), out.address.data()) == 1) { out.v6 = true; return true; }
    return false;
}

std::string endpoint_ip(const Endpoint& endpoint) {
    char text[INET6_ADDRSTRLEN] = {};
    inet_ntop(endpoint.v6 ? AF_INET6 : AF_INET, endpoint.address.data(), text, sizeof text);
    return text;
}

UdpSocket::~UdpSocket() {
    if (socket_ != -1) close_socket(as_socket(socket_));
#ifdef _WIN32
    if (winsock_) WSACleanup();
#endif
}

std::unique_ptr<UdpSocket> UdpSocket::toward(const std::string& host, std::string& error) {
    std::unique_ptr<UdpSocket> udp(new UdpSocket);
#ifdef _WIN32
    WSADATA data;
    udp->winsock_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    if (!udp->winsock_) { error = "could not start Windows networking"; return nullptr; }
#endif
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), "9", &hints, &found) != 0 || !found) { error = "could not resolve " + host; return nullptr; }
    // Connecting a UDP socket sends nothing; it only picks the route and so the local address.
    Endpoint local;
    bool routed = false;
    for (auto* a = found; a && !routed; a = a->ai_next) {
        const Socket probe = ::socket(a->ai_family, SOCK_DGRAM, 0);
        if (probe == kNoSocket) continue;
        sockaddr_storage name{};
        socklen_t length = sizeof name;
        if (::connect(probe, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0 &&
            getsockname(probe, reinterpret_cast<sockaddr*>(&name), &length) == 0)
            routed = from_sockaddr(reinterpret_cast<sockaddr*>(&name), local);
        close_socket(probe);
    }
    freeaddrinfo(found);
    if (!routed) { error = "no network route to " + host; return nullptr; }
    local.port = 0;
    sockaddr_storage bind_to{};
    int length = 0;
    to_sockaddr(local, bind_to, length);
    const Socket s = ::socket(local.v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
    if (s == kNoSocket) { error = "could not open a UDP socket"; return nullptr; }
    udp->socket_ = static_cast<std::intptr_t>(s);
    if (::bind(s, reinterpret_cast<sockaddr*>(&bind_to), length) != 0) { error = "could not bind a UDP socket"; return nullptr; }
    sockaddr_storage name{};
    socklen_t name_length = sizeof name;
    getsockname(s, reinterpret_cast<sockaddr*>(&name), &name_length);
    from_sockaddr(reinterpret_cast<sockaddr*>(&name), udp->local_);
    // Keyframes leave as bursts of a few hundred packets.
    const int buffer = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof buffer);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof buffer);
    return udp;
}

bool UdpSocket::send(const Endpoint& to, const std::uint8_t* data, std::size_t size) {
    sockaddr_storage address{};
    int length = 0;
    to_sockaddr(to, address, length);
#ifdef _WIN32
    return ::sendto(as_socket(socket_), reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
                    reinterpret_cast<sockaddr*>(&address), length) == static_cast<int>(size);
#else
    return ::sendto(as_socket(socket_), data, size, 0, reinterpret_cast<sockaddr*>(&address),
                    static_cast<socklen_t>(length)) == static_cast<ssize_t>(size);
#endif
}

std::size_t UdpSocket::receive(std::uint8_t* data, std::size_t size, int timeout_ms, Endpoint& from) {
    const Socket s = as_socket(socket_);
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval wait{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (select(0, &set, nullptr, nullptr, &wait) <= 0) return 0;
#else
    pollfd wait{s, POLLIN, 0};
    if (poll(&wait, 1, timeout_ms) <= 0) return 0;
#endif
    sockaddr_storage address{};
    socklen_t length = sizeof address;
#ifdef _WIN32
    const int got = ::recvfrom(s, reinterpret_cast<char*>(data), static_cast<int>(size), 0,
                               reinterpret_cast<sockaddr*>(&address), &length);
#else
    const ssize_t got = ::recvfrom(s, data, size, 0, reinterpret_cast<sockaddr*>(&address), &length);
#endif
    if (got <= 0 || !from_sockaddr(reinterpret_cast<sockaddr*>(&address), from)) return 0;
    return static_cast<std::size_t>(got);
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
