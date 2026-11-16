#include "store_signature.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>

namespace luling::app {
namespace {

class AlgorithmHandle {
public:
    ~AlgorithmHandle() {
        if (value_) BCryptCloseAlgorithmProvider(value_, 0);
    }
    bool Open(LPCWSTR name) {
        return BCryptOpenAlgorithmProvider(&value_, name, nullptr, 0) >= 0;
    }
    BCRYPT_ALG_HANDLE Raw() const { return value_; }

private:
    BCRYPT_ALG_HANDLE value_ = nullptr;
};

class KeyHandle {
public:
    ~KeyHandle() {
        if (value_) BCryptDestroyKey(value_);
    }
    BCRYPT_KEY_HANDLE* Address() { return &value_; }
    BCRYPT_KEY_HANDLE Raw() const { return value_; }

private:
    BCRYPT_KEY_HANDLE value_ = nullptr;
};

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

uint32_t ReadLittleEndian(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

constexpr uint32_t kRsaPublicMagic = 0x31415352;  // "RSA1"

// Only the public half of a 2048-4096 bit RSA key is a valid store key.
bool ValidPublicBlob(const std::vector<uint8_t>& blob) {
    if (blob.size() < 24 || blob.size() > 4096) return false;
    const uint32_t magic = ReadLittleEndian(blob.data());
    const uint32_t bits = ReadLittleEndian(blob.data() + 4);
    const uint32_t exponent_bytes = ReadLittleEndian(blob.data() + 8);
    const uint32_t modulus_bytes = ReadLittleEndian(blob.data() + 12);
    const uint32_t prime1_bytes = ReadLittleEndian(blob.data() + 16);
    const uint32_t prime2_bytes = ReadLittleEndian(blob.data() + 20);
    if (magic != kRsaPublicMagic) return false;
    if (bits < 2048 || bits > 4096 || bits % 8 != 0) return false;
    if (modulus_bytes != bits / 8) return false;
    if (exponent_bytes < 1 || exponent_bytes > 8) return false;
    if (prime1_bytes != 0 || prime2_bytes != 0) return false;
    return blob.size() == 24u + exponent_bytes + modulus_bytes;
}

} // namespace

bool DecodeHex(std::string_view text, size_t maximum_bytes, std::vector<uint8_t>& out) {
    if (text.empty() || text.size() % 2 != 0 || text.size() / 2 > maximum_bytes) return false;
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int high = HexValue(text[i]);
        const int low = HexValue(text[i + 1]);
        if (high < 0 || low < 0) return false;
        bytes.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    out = std::move(bytes);
    return true;
}

std::string EncodeHex(const void* data, size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

bool Sha256Bytes(const void* data, size_t size, std::vector<uint8_t>& out) {
    AlgorithmHandle algorithm;
    if (!algorithm.Open(BCRYPT_SHA256_ALGORITHM)) return false;
    KeyHandle hash;
    if (BCryptCreateHash(algorithm.Raw(), hash.Address(), nullptr, 0, nullptr, 0, 0) < 0)
        return false;
    bool ok = true;
    if (size != 0 && BCryptHashData(hash.Raw(), static_cast<PUCHAR>(const_cast<void*>(data)),
                                    static_cast<ULONG>(size), 0) < 0)
        ok = false;
    std::vector<uint8_t> digest(32);
    if (ok && BCryptFinishHash(hash.Raw(), digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        ok = false;
    if (!ok) return false;
    out = std::move(digest);
    return true;
}

bool Sha256BytesHex(const void* data, size_t size, std::string& out) {
    std::vector<uint8_t> digest;
    if (!Sha256Bytes(data, size, digest)) return false;
    out = EncodeHex(digest.data(), digest.size());
    return true;
}

bool PublisherIdFromPublicKey(std::string_view public_key_hex, std::string& id) {
    std::vector<uint8_t> blob;
    if (!DecodeHex(public_key_hex, 4096, blob) || !ValidPublicBlob(blob)) return false;
    return Sha256BytesHex(blob.data(), blob.size(), id);
}

bool VerifyStoreSignature(std::string_view public_key_hex, std::string_view domain,
                          std::string_view message, std::string_view signature_hex) {
    std::vector<uint8_t> blob;
    if (!DecodeHex(public_key_hex, 4096, blob) || !ValidPublicBlob(blob)) return false;
    std::vector<uint8_t> signature;
    if (!DecodeHex(signature_hex, 512, signature) || signature.size() < 256) return false;

    // The domain prefix is signed together with the payload, so one kind of
    // signature can never be replayed as another.
    std::string signed_bytes;
    signed_bytes.reserve(domain.size() + message.size());
    signed_bytes.append(domain);
    signed_bytes.append(message);

    std::vector<uint8_t> digest;
    if (!Sha256Bytes(signed_bytes.data(), signed_bytes.size(), digest)) return false;

    AlgorithmHandle algorithm;
    if (!algorithm.Open(BCRYPT_RSA_ALGORITHM)) return false;
    KeyHandle key;
    if (BCryptImportKeyPair(algorithm.Raw(), nullptr, BCRYPT_RSAPUBLIC_BLOB, key.Address(),
                            blob.data(), static_cast<ULONG>(blob.size()), 0) < 0)
        return false;
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    return BCryptVerifySignature(key.Raw(), &padding, digest.data(),
                                 static_cast<ULONG>(digest.size()), signature.data(),
                                 static_cast<ULONG>(signature.size()), BCRYPT_PAD_PKCS1) >= 0;
}

} // namespace luling::app
