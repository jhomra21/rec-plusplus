#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rec::core {

// A connected byte stream to a stream server: plain TCP, or TLS for rtmps://.
// One thread sends and receives; interrupt() may be called from any thread.
class Transport {
public:
    virtual ~Transport() = default;
    // Sends all of data. False when the connection failed or a write stalled for 10 s.
    virtual bool send(const std::uint8_t* data, std::size_t size) = 0;
    // Waits up to 10 s for at least one byte. 0 when the connection closed, failed or timed out.
    virtual std::size_t receive(std::uint8_t* data, std::size_t size) = 0;
    // True when receive() returns without waiting, including to report the end.
    virtual bool readable() = 0;
    // Fails pending and future sends and receives.
    virtual void interrupt() = 0;
    // Tells the server no more data follows.
    virtual void finish() = 0;
};

// Both wait at most timeout_ms to connect; error says why they returned null.
std::unique_ptr<Transport> connect_tcp(const std::string& host, const std::string& port, int timeout_ms,
                                       std::string& error);
// Verifies the server certificate against the system's trusted roots and the host name.
// Implemented per platform (Schannel, Network.framework).
std::unique_ptr<Transport> connect_tls(const std::string& host, const std::string& port, int timeout_ms,
                                       std::string& error);

// The DER certificate named by REC_TLS_TRUST_CERT (PEM or DER), trusted in addition to the
// system roots, so tests can stream to a server with a self-signed certificate. Empty if unset.
std::vector<std::uint8_t> extra_trusted_certificate();

// An IPv4 or IPv6 address and port, in network byte order.
struct Endpoint {
    bool v6 = false;
    std::array<std::uint8_t, 16> address{};  // IPv4 uses the first 4 bytes.
    std::uint16_t port = 0;
    bool operator==(const Endpoint&) const = default;
};
// A numeric address ("192.0.2.1", "2001:db8::1"). False if it is not one.
bool parse_endpoint(const std::string& ip, std::uint16_t port, Endpoint& out);
std::string endpoint_ip(const Endpoint& endpoint);

// A TCP socket listening on every IPv4 interface. One thread accepts; the connections it
// returns are plain TCP transports with the same 10 s waits.
class TcpListener {
public:
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    static std::unique_ptr<TcpListener> listen(std::uint16_t port, std::string& error);
    // Waits up to timeout_ms for a connection. Null when none came.
    std::unique_ptr<Transport> accept(int timeout_ms, Endpoint& peer);

private:
    TcpListener() = default;
    std::intptr_t socket_ = -1;
    bool winsock_ = false;
};

// The local address the system would use to reach host, as text; nothing is sent. Empty when
// there is no route.
std::string local_address_toward(const std::string& host);

// A UDP socket for WebRTC media, bound to the local address that routes to a server. One
// thread sends and receives.
class UdpSocket {
public:
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    // Resolves host and binds to the interface the system would use to reach it. Empty on error.
    static std::unique_ptr<UdpSocket> toward(const std::string& host, std::string& error);
    const Endpoint& local() const noexcept { return local_; }
    bool send(const Endpoint& to, const std::uint8_t* data, std::size_t size);
    // Waits up to timeout_ms for one datagram. 0 when none arrived.
    std::size_t receive(std::uint8_t* data, std::size_t size, int timeout_ms, Endpoint& from);

private:
    UdpSocket() = default;
    std::intptr_t socket_ = -1;
    Endpoint local_;
    bool winsock_ = false;
};
} // namespace rec::core
