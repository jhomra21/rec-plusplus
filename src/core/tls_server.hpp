#pragma once

#include "core/crypto.hpp"
#include "core/net.hpp"

#include <memory>
#include <string>

namespace rec::core {

// The server side of TLS 1.2 (RFC 5246) on an accepted TCP connection, for the phone camera's
// page: ECDHE-ECDSA with AES-128-GCM (RFC 5289), P-256, extended master secret when offered,
// and the caller's self-signed certificate, which a browser accepts once the person confirms
// it. Blocks until the handshake is done. The transport it returns encrypts and decrypts
// application data; null with error set on failure.
std::unique_ptr<Transport> tls_accept(std::unique_ptr<Transport> tcp, const crypto::P256& key,
                                      const crypto::Bytes& certificate, std::string& error);

} // namespace rec::core
