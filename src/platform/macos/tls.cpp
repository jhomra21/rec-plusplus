// TLS for rtmps:// on macOS: a Network.framework connection behind the blocking Transport
// interface. Completion handlers run on a private serial queue and hand results to the
// calling thread through State.
#include "core/net.hpp"

#include <Network/Network.h>
#include <Security/Security.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace rec::core {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr auto kIoTimeout = std::chrono::seconds(10);

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    nw_connection_state_t connection = nw_connection_state_invalid;
    int error = 0;
    bool tls_error = false;
    bool untrusted = false;  // The certificate failed system and extra-certificate checks.
    Bytes received;
    std::size_t read_at = 0;
    bool receiving = false, ended = false;
};

// True when the server's leaf certificate is exactly the extra trusted one.
bool pinned(sec_trust_t trust, const Bytes& certificate) {
    SecTrustRef ref = sec_trust_copy_ref(trust);
    bool match = false;
    if (CFArrayRef chain = SecTrustCopyCertificateChain(ref)) {
        if (CFArrayGetCount(chain) > 0) {
            auto leaf = static_cast<SecCertificateRef>(const_cast<void*>(CFArrayGetValueAtIndex(chain, 0)));
            CFDataRef der = SecCertificateCopyData(leaf);
            match = static_cast<std::size_t>(CFDataGetLength(der)) == certificate.size() &&
                    std::memcmp(CFDataGetBytePtr(der), certificate.data(), certificate.size()) == 0;
            CFRelease(der);
        }
        CFRelease(chain);
    }
    CFRelease(ref);
    return match;
}

class NetworkTransport final : public Transport {
public:
    NetworkTransport(nw_connection_t connection, dispatch_queue_t queue, std::shared_ptr<State> state)
        : connection_(connection), queue_(queue), state_(std::move(state)) {}
    ~NetworkTransport() override {
        nw_connection_cancel(connection_);
        {
            std::unique_lock lock(state_->mutex);
            state_->changed.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return state_->connection == nw_connection_state_cancelled; });
        }
        nw_release(connection_);
        dispatch_release(queue_);
    }

    // Waits for the connection to become ready.
    std::string start(const std::string& where, int timeout_ms) {
        nw_connection_start(connection_);
        std::unique_lock lock(state_->mutex);
        state_->changed.wait_for(lock, std::chrono::milliseconds(timeout_ms + 1000), [&] {
            return state_->connection == nw_connection_state_ready || state_->ended ||
                   state_->connection == nw_connection_state_waiting;
        });
        if (state_->connection == nw_connection_state_ready && !state_->ended) return {};
        if (state_->untrusted) return "the certificate of " + where + " is not trusted";
        if (state_->tls_error) return "TLS handshake with " + where + " failed (" + std::to_string(state_->error) + ")";
        return "could not connect to " + where;
    }

    bool send(const std::uint8_t* data, std::size_t size) override {
        auto state = state_;
        auto done = std::make_shared<int>(0);  // 1 sent, 2 failed.
        dispatch_data_t content = dispatch_data_create(data, size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        nw_connection_send(connection_, content, NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, false, ^(nw_error_t error) {
            std::lock_guard lock(state->mutex);
            *done = error ? 2 : 1;
            state->changed.notify_all();
        });
        dispatch_release(content);
        std::unique_lock lock(state_->mutex);
        if (!state_->changed.wait_for(lock, kIoTimeout, [&] { return *done != 0; })) {
            lock.unlock();
            nw_connection_cancel(connection_);
            return false;
        }
        return *done == 1;
    }

    std::size_t receive(std::uint8_t* data, std::size_t size) override {
        std::unique_lock lock(state_->mutex);
        for (;;) {
            auto& s = *state_;
            if (s.read_at < s.received.size()) {
                const auto n = std::min(size, s.received.size() - s.read_at);
                std::memcpy(data, s.received.data() + s.read_at, n);
                s.read_at += n;
                if (s.read_at == s.received.size()) { s.received.clear(); s.read_at = 0; }
                return n;
            }
            if (s.ended) return 0;
            arm();
            if (!s.changed.wait_for(lock, kIoTimeout, [&] { return s.read_at < s.received.size() || s.ended; })) {
                s.ended = true;
                lock.unlock();
                nw_connection_cancel(connection_);
                return 0;
            }
        }
    }

    bool readable() override {
        std::lock_guard lock(state_->mutex);
        if (state_->read_at < state_->received.size() || state_->ended) return true;
        arm();
        return false;
    }

    void interrupt() override { nw_connection_cancel(connection_); }

    void finish() override {
        nw_connection_send(connection_, nullptr, NW_CONNECTION_FINAL_MESSAGE_CONTEXT, true, ^(nw_error_t) {});
    }

private:
    // Keeps one receive outstanding; the caller holds the state lock.
    void arm() {
        if (state_->receiving || state_->ended) return;
        state_->receiving = true;
        auto state = state_;
        nw_connection_receive(connection_, 1, 65536,
                              ^(dispatch_data_t content, nw_content_context_t context, bool complete, nw_error_t error) {
            std::lock_guard lock(state->mutex);
            if (content) {
                dispatch_data_apply(content, ^bool(dispatch_data_t, size_t, const void* bytes, size_t n) {
                    const auto* p = static_cast<const std::uint8_t*>(bytes);
                    state->received.insert(state->received.end(), p, p + n);
                    return true;
                });
            }
            if (error || (complete && (!content || (context && nw_content_context_get_is_final(context)))))
                state->ended = true;
            state->receiving = false;
            state->changed.notify_all();
        });
    }

    nw_connection_t connection_;
    dispatch_queue_t queue_;
    std::shared_ptr<State> state_;
};
} // namespace

std::unique_ptr<Transport> connect_tls(const std::string& host, const std::string& port, int timeout_ms,
                                       std::string& error) {
    auto state = std::make_shared<State>();
    const Bytes extra = extra_trusted_certificate();
    dispatch_queue_t queue = dispatch_queue_create("rec.stream.tls", DISPATCH_QUEUE_SERIAL);
    const int timeout_s = std::max(1, timeout_ms / 1000);
    nw_parameters_t parameters = nw_parameters_create_secure_tcp(
        ^(nw_protocol_options_t tls) {
            // The default checks (system roots, host name), plus the extra certificate, and a
            // record of the outcome for the error message.
            sec_protocol_options_t options = nw_tls_copy_sec_protocol_options(tls);
            sec_protocol_options_set_verify_block(
                options,
                ^(sec_protocol_metadata_t, sec_trust_t trust, sec_protocol_verify_complete_t complete) {
                    SecTrustRef ref = sec_trust_copy_ref(trust);
                    const bool trusted = SecTrustEvaluateWithError(ref, nullptr) || (!extra.empty() && pinned(trust, extra));
                    CFRelease(ref);
                    if (!trusted) {
                        std::lock_guard lock(state->mutex);
                        state->untrusted = true;
                    }
                    complete(trusted);
                },
                queue);
            sec_release(options);
        },
        ^(nw_protocol_options_t tcp) {
            nw_tcp_options_set_no_delay(tcp, true);
            nw_tcp_options_set_connection_timeout(tcp, static_cast<std::uint32_t>(timeout_s));
        });
    nw_endpoint_t endpoint = nw_endpoint_create_host(host.c_str(), port.c_str());
    nw_connection_t connection = nw_connection_create(endpoint, parameters);
    nw_release(endpoint);
    nw_release(parameters);
    nw_connection_set_queue(connection, queue);
    nw_connection_set_state_changed_handler(connection, ^(nw_connection_state_t now, nw_error_t failure) {
        std::lock_guard lock(state->mutex);
        state->connection = now;
        if (failure) {
            state->error = nw_error_get_error_code(failure);
            state->tls_error = nw_error_get_error_domain(failure) == nw_error_domain_tls;
        }
        if (now == nw_connection_state_failed || now == nw_connection_state_cancelled) state->ended = true;
        state->changed.notify_all();
    });
    auto transport = std::make_unique<NetworkTransport>(connection, queue, state);
    if (error = transport->start(host + ":" + port, timeout_ms); !error.empty()) return nullptr;
    return transport;
}

} // namespace rec::core
