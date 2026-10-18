#pragma once

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

} // namespace rec::core
