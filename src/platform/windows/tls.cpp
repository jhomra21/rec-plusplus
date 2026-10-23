// TLS for rtmps:// on Windows: Schannel over the plain TCP transport.
#include "core/net.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS
#include <windows.h>
#include <subauth.h>
#include <wincrypt.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rec::core {
namespace {
using Bytes = std::vector<std::uint8_t>;

constexpr DWORD kFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                         ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR;

std::string hex(long code) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(code));
    return text;
}

std::wstring wide(const std::string& s) {
    std::wstring out(s.size(), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(),
                                      static_cast<int>(out.size()));
    out.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    return out;
}

class SchannelTransport final : public Transport {
public:
    explicit SchannelTransport(std::unique_ptr<Transport> tcp) : tcp_(std::move(tcp)) {}
    ~SchannelTransport() override {
        if (have_context_) DeleteSecurityContext(&context_);
        if (have_credentials_) FreeCredentialsHandle(&credentials_);
    }

    std::string open(const std::string& host, const std::string& where) {
        host_ = wide(host);
        SCH_CREDENTIALS credentials{};
        credentials.dwVersion = SCH_CREDENTIALS_VERSION;
        credentials.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
        auto status = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND,
                                                nullptr, &credentials, nullptr, nullptr, &credentials_, nullptr);
        if (status != SEC_E_OK) return "could not start TLS (" + hex(status) + ")";
        have_credentials_ = true;
        if (status = negotiate(true); status != SEC_E_OK)
            return "TLS handshake with " + where + " failed (" + hex(status) + ")";
        if (auto error = verify(where); !error.empty()) return error;
        if (status = QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_); status != SEC_E_OK)
            return "TLS handshake with " + where + " failed (" + hex(status) + ")";
        return {};
    }

    bool send(const std::uint8_t* data, std::size_t size) override {
        while (size) {
            const auto n = std::min<std::size_t>(size, sizes_.cbMaximumMessage);
            out_.resize(sizes_.cbHeader + n + sizes_.cbTrailer);
            std::memcpy(out_.data() + sizes_.cbHeader, data, n);
            SecBuffer buffers[4] = {
                {sizes_.cbHeader, SECBUFFER_STREAM_HEADER, out_.data()},
                {static_cast<unsigned long>(n), SECBUFFER_DATA, out_.data() + sizes_.cbHeader},
                {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, out_.data() + sizes_.cbHeader + n},
                {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc description{SECBUFFER_VERSION, 4, buffers};
            if (EncryptMessage(&context_, 0, &description, 0) != SEC_E_OK) return false;
            if (!tcp_->send(out_.data(), buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer)) return false;
            data += n;
            size -= n;
        }
        return true;
    }

    std::size_t receive(std::uint8_t* data, std::size_t size) override {
        for (;;) {
            if (plain_at_ < plain_.size()) {
                const auto n = std::min(size, plain_.size() - plain_at_);
                std::memcpy(data, plain_.data() + plain_at_, n);
                plain_at_ += n;
                return n;
            }
            if (ended_) return 0;
            if (!decrypt() && !fill()) return 0;
        }
    }

    bool readable() override {
        for (;;) {
            if (plain_at_ < plain_.size() || ended_) return true;
            if (decrypt()) continue;
            if (!tcp_->readable()) return false;
            if (!fill()) return true;
        }
    }

    void interrupt() override { tcp_->interrupt(); }

    void finish() override {
        DWORD type = SCHANNEL_SHUTDOWN;
        SecBuffer in{sizeof type, SECBUFFER_TOKEN, &type};
        SecBufferDesc in_description{SECBUFFER_VERSION, 1, &in};
        if (ApplyControlToken(&context_, &in_description) == SEC_E_OK) {
            SecBuffer out{0, SECBUFFER_TOKEN, nullptr};
            SecBufferDesc out_description{SECBUFFER_VERSION, 1, &out};
            DWORD attributes = 0;
            InitializeSecurityContextW(&credentials_, &context_, host_.data(), kFlags, 0, 0, nullptr, 0, nullptr,
                                       &out_description, &attributes, nullptr);
            if (out.pvBuffer) {
                tcp_->send(static_cast<const std::uint8_t*>(out.pvBuffer), out.cbBuffer);
                FreeContextBuffer(out.pvBuffer);
            }
        }
        tcp_->finish();
    }

private:
    // Reads more ciphertext. False when the connection ended.
    bool fill() {
        std::uint8_t chunk[16384];
        const auto got = tcp_->receive(chunk, sizeof chunk);
        if (!got) { ended_ = true; return false; }
        in_.insert(in_.end(), chunk, chunk + got);
        return true;
    }

    // Keeps the SECBUFFER_EXTRA bytes at the end of in_, which belong to the next record.
    void keep_extra(const SecBuffer* buffers, int count) {
        for (int i = 0; i < count; ++i) {
            if (buffers[i].BufferType == SECBUFFER_EXTRA) {
                in_.erase(in_.begin(), in_.end() - buffers[i].cbBuffer);
                return;
            }
        }
        in_.clear();
    }

    // The handshake, or with first false, the post-handshake messages after SEC_I_RENEGOTIATE
    // (TLS 1.3 session tickets and key updates).
    SECURITY_STATUS negotiate(bool first) {
        bool need_data = first || in_.empty();
        for (;;) {
            if (!first && need_data && !fill()) return SEC_E_INTERNAL_ERROR;
            SecBuffer in[2] = {{static_cast<unsigned long>(in_.size()), SECBUFFER_TOKEN, in_.data()},
                               {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc in_description{SECBUFFER_VERSION, 2, in};
            SecBuffer out[2] = {{0, SECBUFFER_TOKEN, nullptr}, {0, SECBUFFER_ALERT, nullptr}};
            SecBufferDesc out_description{SECBUFFER_VERSION, 2, out};
            DWORD attributes = 0;
            const auto status = InitializeSecurityContextW(
                &credentials_, first ? nullptr : &context_, host_.data(), kFlags, 0, 0, first ? nullptr : &in_description,
                0, &context_, &out_description, &attributes, nullptr);
            if (first) have_context_ = status == SEC_I_CONTINUE_NEEDED || status == SEC_E_OK;
            bool sent = true;
            if (out[0].pvBuffer) {
                if (out[0].cbBuffer) sent = tcp_->send(static_cast<const std::uint8_t*>(out[0].pvBuffer), out[0].cbBuffer);
                FreeContextBuffer(out[0].pvBuffer);
            }
            if (out[1].pvBuffer) FreeContextBuffer(out[1].pvBuffer);
            if (!sent) return SEC_E_INTERNAL_ERROR;
            if (status == SEC_E_INCOMPLETE_MESSAGE) { need_data = true; first = false; continue; }
            if (!first) keep_extra(in, 2);
            first = false;
            if (status == SEC_E_OK) return status;
            if (status != SEC_I_CONTINUE_NEEDED) return status;
            need_data = in_.empty();
        }
    }

    std::string verify(const std::string& where) {
        PCCERT_CONTEXT certificate = nullptr;
        if (QueryContextAttributesW(&context_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &certificate) != SEC_E_OK || !certificate)
            return "the stream server " + where + " sent no certificate";
        LPSTR usage = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
        CERT_CHAIN_PARA chain_parameters{sizeof chain_parameters};
        chain_parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
        chain_parameters.RequestedUsage.Usage.cUsageIdentifier = 1;
        chain_parameters.RequestedUsage.Usage.rgpszUsageIdentifier = &usage;
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        DWORD result = CERT_E_UNTRUSTEDROOT;
        if (CertGetCertificateChain(nullptr, certificate, nullptr, certificate->hCertStore, &chain_parameters, 0,
                                    nullptr, &chain)) {
            SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
            ssl.cbSize = sizeof ssl;
            ssl.dwAuthType = AUTHTYPE_SERVER;
            ssl.pwszServerName = host_.data();
            CERT_CHAIN_POLICY_PARA policy{sizeof policy};
            policy.pvExtraPolicyPara = &ssl;
            CERT_CHAIN_POLICY_STATUS status{sizeof status};
            if (CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status)) result = status.dwError;
            CertFreeCertificateChain(chain);
        }
        if (result != 0) {
            const auto pinned = extra_trusted_certificate();
            if (!pinned.empty() && pinned.size() == certificate->cbCertEncoded &&
                std::memcmp(pinned.data(), certificate->pbCertEncoded, pinned.size()) == 0)
                result = 0;
        }
        CertFreeCertificateContext(certificate);
        if (result != 0) return "the certificate of " + where + " is not trusted (" + hex(static_cast<long>(result)) + ")";
        return {};
    }

    // Decrypts one record from in_. False when in_ holds no complete record.
    bool decrypt() {
        if (in_.empty()) return false;
        SecBuffer buffers[4] = {{static_cast<unsigned long>(in_.size()), SECBUFFER_DATA, in_.data()},
                                {0, SECBUFFER_EMPTY, nullptr},
                                {0, SECBUFFER_EMPTY, nullptr},
                                {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc description{SECBUFFER_VERSION, 4, buffers};
        const auto status = DecryptMessage(&context_, &description, 0, nullptr);
        if (status == SEC_E_INCOMPLETE_MESSAGE) return false;
        if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE) {
            ended_ = true;  // Includes SEC_I_CONTEXT_EXPIRED, the server's close_notify.
            in_.clear();
            return true;
        }
        plain_.clear();
        plain_at_ = 0;
        for (const auto& b : buffers) {
            if (b.BufferType == SECBUFFER_DATA && b.cbBuffer) {
                const auto* p = static_cast<const std::uint8_t*>(b.pvBuffer);
                plain_.assign(p, p + b.cbBuffer);
            }
        }
        keep_extra(buffers, 4);
        if (status == SEC_I_RENEGOTIATE && negotiate(false) != SEC_E_OK) ended_ = true;
        return true;
    }

    std::unique_ptr<Transport> tcp_;
    std::wstring host_;
    CredHandle credentials_{};
    CtxtHandle context_{};
    bool have_credentials_ = false, have_context_ = false;
    SecPkgContext_StreamSizes sizes_{};
    Bytes in_, plain_, out_;
    std::size_t plain_at_ = 0;
    bool ended_ = false;
};
} // namespace

std::unique_ptr<Transport> connect_tls(const std::string& host, const std::string& port, int timeout_ms,
                                       std::string& error) {
    auto tcp = connect_tcp(host, port, timeout_ms, error);
    if (!tcp) return nullptr;
    auto tls = std::make_unique<SchannelTransport>(std::move(tcp));
    if (error = tls->open(host, host + ":" + port); !error.empty()) return nullptr;
    return tls;
}

} // namespace rec::core
