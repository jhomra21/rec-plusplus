#include "core/crypto.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>

namespace rec::core::crypto {
namespace {

bool ok(NTSTATUS status) { return status >= 0; }

// Algorithm providers open once per process; CNG handles are thread-safe.
BCRYPT_ALG_HANDLE provider(const wchar_t* id, ULONG flags = 0) {
    BCRYPT_ALG_HANDLE handle = nullptr;
    return ok(BCryptOpenAlgorithmProvider(&handle, id, nullptr, flags)) ? handle : nullptr;
}
BCRYPT_ALG_HANDLE aes() {
    static BCRYPT_ALG_HANDLE handle = [] {
        BCRYPT_ALG_HANDLE h = provider(BCRYPT_AES_ALGORITHM);
        if (h && !ok(BCryptSetProperty(h, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
                                       sizeof(BCRYPT_CHAIN_MODE_ECB), 0))) {
            BCryptCloseAlgorithmProvider(h, 0);
            h = nullptr;
        }
        return h;
    }();
    return handle;
}
BCRYPT_ALG_HANDLE sha256_alg() { static BCRYPT_ALG_HANDLE h = provider(BCRYPT_SHA256_ALGORITHM); return h; }
BCRYPT_ALG_HANDLE hmac_alg(bool sha1) {
    static BCRYPT_ALG_HANDLE one = provider(BCRYPT_SHA1_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    static BCRYPT_ALG_HANDLE two = provider(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    return sha1 ? one : two;
}
BCRYPT_ALG_HANDLE ecdsa() { static BCRYPT_ALG_HANDLE h = provider(BCRYPT_ECDSA_P256_ALGORITHM); return h; }
BCRYPT_ALG_HANDLE ecdh() { static BCRYPT_ALG_HANDLE h = provider(BCRYPT_ECDH_P256_ALGORITHM); return h; }
BCRYPT_ALG_HANDLE rsa() { static BCRYPT_ALG_HANDLE h = provider(BCRYPT_RSA_ALGORITHM); return h; }

template <std::size_t N>
std::array<std::uint8_t, N> digest(BCRYPT_ALG_HANDLE alg, const std::uint8_t* key, std::size_t key_size,
                                   const std::uint8_t* data, std::size_t size) {
    std::array<std::uint8_t, N> out{};
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!alg || !ok(BCryptCreateHash(alg, &hash, nullptr, 0, const_cast<PUCHAR>(key), static_cast<ULONG>(key_size), 0)))
        return out;
    BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0);
    BCryptFinishHash(hash, out.data(), static_cast<ULONG>(N), 0);
    BCryptDestroyHash(hash);
    return out;
}

// A public P-256 point (0x04, X, Y) as a CNG public key blob for alg.
BCRYPT_KEY_HANDLE import_point(BCRYPT_ALG_HANDLE alg, ULONG magic, const Bytes& point) {
    if (!alg || point.size() != 65 || point[0] != 4) return nullptr;
    std::vector<std::uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = magic;
    header->cbKey = 32;
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), point.data() + 1, 64);
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!ok(BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob.data(), static_cast<ULONG>(blob.size()), 0)))
        return nullptr;
    return key;
}

// ECDSA signatures travel as DER SEQUENCE { r, s }; CNG works with fixed 32-byte r and s.
Bytes der_signature(const std::uint8_t* raw64);
bool raw_signature(const Bytes& der, std::uint8_t* raw64);

void append_integer(Bytes& out, const std::uint8_t* value, std::size_t size) {
    while (size > 1 && value[0] == 0) { ++value; --size; }
    const bool pad = value[0] & 0x80;
    out.push_back(0x02);
    out.push_back(static_cast<std::uint8_t>(size + pad));
    if (pad) out.push_back(0);
    out.insert(out.end(), value, value + size);
}
} // namespace

bool random(std::uint8_t* out, std::size_t size) {
    return ok(BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t size) {
    return digest<32>(sha256_alg(), nullptr, 0, data, size);
}
std::array<std::uint8_t, 20> hmac_sha1(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* data,
                                       std::size_t size) {
    return digest<20>(hmac_alg(true), key, key_size, data, size);
}
std::array<std::uint8_t, 32> hmac_sha256(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* data,
                                         std::size_t size) {
    return digest<32>(hmac_alg(false), key, key_size, data, size);
}

struct Aes128::Impl {
    BCRYPT_KEY_HANDLE key = nullptr;
};

Aes128::Aes128(const std::uint8_t* key) : impl_(std::make_unique<Impl>()) {
    if (aes()) BCryptGenerateSymmetricKey(aes(), &impl_->key, nullptr, 0, const_cast<PUCHAR>(key), 16, 0);
}
Aes128::~Aes128() { if (impl_->key) BCryptDestroyKey(impl_->key); }
bool Aes128::valid() const noexcept { return impl_->key != nullptr; }
void Aes128::encrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t size) const {
    ULONG written = 0;
    BCryptEncrypt(impl_->key, const_cast<PUCHAR>(in), static_cast<ULONG>(size), nullptr, nullptr, 0, out,
                  static_cast<ULONG>(size), &written, 0);
}

struct P256::Impl {
    BCRYPT_KEY_HANDLE key = nullptr;
    Use use = Use::sign;
    Bytes point;
    ~Impl() { if (key) BCryptDestroyKey(key); }
};

P256::P256(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
P256::~P256() = default;

std::unique_ptr<P256> P256::generate(Use use) {
    auto impl = std::make_unique<Impl>();
    impl->use = use;
    BCRYPT_ALG_HANDLE alg = use == Use::sign ? ecdsa() : ecdh();
    if (!alg || !ok(BCryptGenerateKeyPair(alg, &impl->key, 256, 0)) || !ok(BCryptFinalizeKeyPair(impl->key, 0)))
        return nullptr;
    std::uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    ULONG size = 0;
    if (!ok(BCryptExportKey(impl->key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof blob, &size, 0)) || size != sizeof blob)
        return nullptr;
    impl->point.push_back(4);
    impl->point.insert(impl->point.end(), blob + sizeof(BCRYPT_ECCKEY_BLOB), blob + sizeof blob);
    return std::make_unique<P256>(std::move(impl));
}

const Bytes& P256::public_point() const noexcept { return impl_->point; }

std::unique_ptr<P256> P256::import_private(const Bytes& key) {
    if (!ecdsa() || key.size() != 97 || key[0] != 4) return nullptr;
    std::vector<std::uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 96);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDSA_PRIVATE_P256_MAGIC;
    header->cbKey = 32;
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), key.data() + 1, 96);
    auto impl = std::make_unique<Impl>();
    const bool imported = ok(BCryptImportKeyPair(ecdsa(), nullptr, BCRYPT_ECCPRIVATE_BLOB, &impl->key, blob.data(),
                                                 static_cast<ULONG>(blob.size()), 0));
    SecureZeroMemory(blob.data(), blob.size());
    if (!imported) return nullptr;
    impl->point.assign(key.begin(), key.begin() + 65);
    return std::make_unique<P256>(std::move(impl));
}

Bytes P256::private_key() const {
    if (impl_->use != Use::sign) return {};
    std::uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96];
    ULONG size = 0;
    Bytes out;
    if (ok(BCryptExportKey(impl_->key, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob, sizeof blob, &size, 0)) && size == sizeof blob) {
        out.push_back(4);
        out.insert(out.end(), blob + sizeof(BCRYPT_ECCKEY_BLOB), blob + sizeof blob);
    }
    SecureZeroMemory(blob, sizeof blob);
    return out;
}
Bytes P256::sign(const std::uint8_t* data, std::size_t size) const {
    if (impl_->use != Use::sign) return {};
    auto hash = sha256(data, size);
    std::uint8_t raw[64];
    ULONG written = 0;
    if (!ok(BCryptSignHash(impl_->key, nullptr, hash.data(), 32, raw, sizeof raw, &written, 0)) || written != 64) return {};
    return der_signature(raw);
}

Bytes P256::agree(const Bytes& peer_point) const {
    if (impl_->use != Use::agree) return {};
    BCRYPT_KEY_HANDLE peer = import_point(ecdh(), BCRYPT_ECDH_PUBLIC_P256_MAGIC, peer_point);
    if (!peer) return {};
    BCRYPT_SECRET_HANDLE secret = nullptr;
    Bytes out(32);
    ULONG written = 0;
    const bool agreed = ok(BCryptSecretAgreement(impl_->key, peer, &secret, 0)) &&
                        ok(BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, out.data(), 32, &written, 0)) &&
                        written == 32;
    if (secret) BCryptDestroySecret(secret);
    BCryptDestroyKey(peer);
    if (!agreed) return {};
    // CNG returns the raw secret little-endian.
    std::reverse(out.begin(), out.end());
    return out;
}

bool verify_p256(const Bytes& point, const std::uint8_t* data, std::size_t size, const Bytes& signature) {
    std::uint8_t raw[64];
    if (!raw_signature(signature, raw)) return false;
    BCRYPT_KEY_HANDLE key = import_point(ecdsa(), BCRYPT_ECDSA_PUBLIC_P256_MAGIC, point);
    if (!key) return false;
    auto hash = sha256(data, size);
    const bool good = ok(BCryptVerifySignature(key, nullptr, hash.data(), 32, raw, 64, 0));
    BCryptDestroyKey(key);
    return good;
}

bool verify_rsa(const Bytes& modulus, const Bytes& exponent, const std::uint8_t* data, std::size_t size,
                const Bytes& signature) {
    auto trim = [](const Bytes& value) {
        auto first = std::find_if(value.begin(), value.end(), [](std::uint8_t b) { return b != 0; });
        return Bytes(first, value.end());
    };
    const Bytes n = trim(modulus), e = trim(exponent);
    if (!rsa() || n.empty() || e.empty()) return false;
    std::vector<std::uint8_t> blob(sizeof(BCRYPT_RSAKEY_BLOB) + e.size() + n.size());
    auto* header = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob.data());
    header->Magic = BCRYPT_RSAPUBLIC_MAGIC;
    header->BitLength = static_cast<ULONG>(n.size() * 8);
    header->cbPublicExp = static_cast<ULONG>(e.size());
    header->cbModulus = static_cast<ULONG>(n.size());
    std::memcpy(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB), e.data(), e.size());
    std::memcpy(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB) + e.size(), n.data(), n.size());
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!ok(BCryptImportKeyPair(rsa(), nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, blob.data(), static_cast<ULONG>(blob.size()), 0)))
        return false;
    auto hash = sha256(data, size);
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    const bool good = ok(BCryptVerifySignature(key, &padding, hash.data(), 32, const_cast<PUCHAR>(signature.data()),
                                               static_cast<ULONG>(signature.size()), BCRYPT_PAD_PKCS1));
    BCryptDestroyKey(key);
    return good;
}

namespace {
Bytes der_signature(const std::uint8_t* raw64) {
    Bytes body;
    append_integer(body, raw64, 32);
    append_integer(body, raw64 + 32, 32);
    Bytes out{0x30, static_cast<std::uint8_t>(body.size())};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

bool raw_signature(const Bytes& der, std::uint8_t* raw64) {
    std::memset(raw64, 0, 64);
    if (der.size() < 8 || der[0] != 0x30 || der[1] != der.size() - 2) return false;
    std::size_t at = 2;
    for (int part = 0; part < 2; ++part) {
        if (at + 2 > der.size() || der[at] != 0x02) return false;
        std::size_t length = der[at + 1];
        at += 2;
        if (at + length > der.size()) return false;
        const std::uint8_t* value = der.data() + at;
        at += length;
        while (length > 32 && *value == 0) { ++value; --length; }
        if (length > 32) return false;
        std::memcpy(raw64 + part * 32 + (32 - length), value, length);
    }
    return at == der.size();
}
} // namespace

} // namespace rec::core::crypto
