#include "core/crypto.hpp"

#include <CommonCrypto/CommonCrypto.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <cstring>

namespace rec::core::crypto {
namespace {

template <typename T>
struct Ref {
    T value = nullptr;
    Ref() = default;
    explicit Ref(T v) : value(v) {}
    ~Ref() { if (value) CFRelease(value); }
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    T* out() { return &value; }
    explicit operator bool() const { return value != nullptr; }
};

Ref<CFDataRef> data_of(const std::uint8_t* data, std::size_t size) {
    return Ref<CFDataRef>(CFDataCreate(kCFAllocatorDefault, data, static_cast<CFIndex>(size)));
}

Bytes bytes_of(CFDataRef data) {
    if (!data) return {};
    const auto* begin = CFDataGetBytePtr(data);
    return Bytes(begin, begin + CFDataGetLength(data));
}

// A public key from its external representation (an X9.63 point, or PKCS#1 for RSA).
Ref<SecKeyRef> public_key(CFStringRef type, int bits, const Bytes& external) {
    auto data = data_of(external.data(), external.size());
    const int size = bits;
    Ref<CFNumberRef> number(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &size));
    const void* keys[] = {kSecAttrKeyType, kSecAttrKeyClass, kSecAttrKeySizeInBits};
    const void* values[] = {type, kSecAttrKeyClassPublic, number.value};
    Ref<CFDictionaryRef> attributes(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3,
                                                       &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    return Ref<SecKeyRef>(SecKeyCreateWithData(data.value, attributes.value, nullptr));
}

bool verify(SecKeyRef key, SecKeyAlgorithm algorithm, const std::uint8_t* data, std::size_t size, const Bytes& signature) {
    if (!key) return false;
    auto message = data_of(data, size);
    auto sig = data_of(signature.data(), signature.size());
    return SecKeyVerifySignature(key, algorithm, message.value, sig.value, nullptr);
}

void append_integer(Bytes& out, const Bytes& value) {
    std::size_t first = 0;
    while (first + 1 < value.size() && value[first] == 0) ++first;
    Bytes body(value.begin() + static_cast<std::ptrdiff_t>(first), value.end());
    if (body.empty() || body[0] & 0x80) body.insert(body.begin(), 0);
    out.push_back(0x02);
    if (body.size() < 0x80) out.push_back(static_cast<std::uint8_t>(body.size()));
    else if (body.size() < 0x100) { out.push_back(0x81); out.push_back(static_cast<std::uint8_t>(body.size())); }
    else { out.push_back(0x82); out.push_back(static_cast<std::uint8_t>(body.size() >> 8)); out.push_back(static_cast<std::uint8_t>(body.size())); }
    out.insert(out.end(), body.begin(), body.end());
}
} // namespace

bool random(std::uint8_t* out, std::size_t size) { return SecRandomCopyBytes(kSecRandomDefault, size, out) == errSecSuccess; }

std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t size) {
    std::array<std::uint8_t, 32> out{};
    CC_SHA256(data, static_cast<CC_LONG>(size), out.data());
    return out;
}
std::array<std::uint8_t, 20> hmac_sha1(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* data,
                                       std::size_t size) {
    std::array<std::uint8_t, 20> out{};
    CCHmac(kCCHmacAlgSHA1, key, key_size, data, size, out.data());
    return out;
}
std::array<std::uint8_t, 32> hmac_sha256(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* data,
                                         std::size_t size) {
    std::array<std::uint8_t, 32> out{};
    CCHmac(kCCHmacAlgSHA256, key, key_size, data, size, out.data());
    return out;
}

struct Aes128::Impl {
    CCCryptorRef cryptor = nullptr;
};

Aes128::Aes128(const std::uint8_t* key) : impl_(std::make_unique<Impl>()) {
    if (CCCryptorCreate(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 16, nullptr, &impl_->cryptor) != kCCSuccess)
        impl_->cryptor = nullptr;
}
Aes128::~Aes128() { if (impl_->cryptor) CCCryptorRelease(impl_->cryptor); }
bool Aes128::valid() const noexcept { return impl_->cryptor != nullptr; }
void Aes128::encrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t size) const {
    std::size_t moved = 0;
    CCCryptorUpdate(impl_->cryptor, in, size, out, size, &moved);
}

struct P256::Impl {
    SecKeyRef key = nullptr;
    Bytes point;
    ~Impl() { if (key) CFRelease(key); }
};

P256::P256(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
P256::~P256() = default;

std::unique_ptr<P256> P256::generate(Use) {
    const int bits = 256;
    Ref<CFNumberRef> number(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bits));
    const void* keys[] = {kSecAttrKeyType, kSecAttrKeySizeInBits};
    const void* values[] = {kSecAttrKeyTypeECSECPrimeRandom, number.value};
    Ref<CFDictionaryRef> attributes(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                                                       &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    auto impl = std::make_unique<Impl>();
    impl->key = SecKeyCreateRandomKey(attributes.value, nullptr);
    if (!impl->key) return nullptr;
    Ref<SecKeyRef> pub(SecKeyCopyPublicKey(impl->key));
    if (!pub) return nullptr;
    Ref<CFDataRef> external(SecKeyCopyExternalRepresentation(pub.value, nullptr));
    impl->point = bytes_of(external.value);
    if (impl->point.size() != 65) return nullptr;
    return std::make_unique<P256>(std::move(impl));
}

const Bytes& P256::public_point() const noexcept { return impl_->point; }

Bytes P256::sign(const std::uint8_t* data, std::size_t size) const {
    auto message = data_of(data, size);
    Ref<CFDataRef> signature(SecKeyCreateSignature(impl_->key, kSecKeyAlgorithmECDSASignatureMessageX962SHA256,
                                                   message.value, nullptr));
    return bytes_of(signature.value);
}

Bytes P256::agree(const Bytes& peer_point) const {
    auto peer = public_key(kSecAttrKeyTypeECSECPrimeRandom, 256, peer_point);
    if (!peer) return {};
    Ref<CFDictionaryRef> parameters(CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
                                                       &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    Ref<CFDataRef> shared(SecKeyCopyKeyExchangeResult(impl_->key, kSecKeyAlgorithmECDHKeyExchangeStandard, peer.value,
                                                      parameters.value, nullptr));
    return bytes_of(shared.value);
}

bool verify_p256(const Bytes& point, const std::uint8_t* data, std::size_t size, const Bytes& signature) {
    auto key = public_key(kSecAttrKeyTypeECSECPrimeRandom, 256, point);
    return verify(key.value, kSecKeyAlgorithmECDSASignatureMessageX962SHA256, data, size, signature);
}

bool verify_rsa(const Bytes& modulus, const Bytes& exponent, const std::uint8_t* data, std::size_t size,
                const Bytes& signature) {
    // PKCS#1 RSAPublicKey: SEQUENCE { modulus INTEGER, publicExponent INTEGER }.
    Bytes body;
    append_integer(body, modulus);
    append_integer(body, exponent);
    Bytes der{0x30};
    if (body.size() < 0x80) der.push_back(static_cast<std::uint8_t>(body.size()));
    else if (body.size() < 0x100) { der.push_back(0x81); der.push_back(static_cast<std::uint8_t>(body.size())); }
    else { der.push_back(0x82); der.push_back(static_cast<std::uint8_t>(body.size() >> 8)); der.push_back(static_cast<std::uint8_t>(body.size())); }
    der.insert(der.end(), body.begin(), body.end());
    auto key = public_key(kSecAttrKeyTypeRSA, static_cast<int>(modulus.size() * 8), der);
    return verify(key.value, kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256, data, size, signature);
}

} // namespace rec::core::crypto
