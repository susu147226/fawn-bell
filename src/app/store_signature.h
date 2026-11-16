#pragma once
// store_signature.h — publisher and catalog signatures for the extension store.
//
// Packages and the catalog index are signed with RSA-PKCS1-SHA256 through CNG.
// Keys are CNG blobs in lower-case hex: RSAPUBLICBLOB (2048-4096 bits) is
// published, RSAFULLPRIVATEBLOB stays with the publisher. A publisher id is
// the SHA-256 of the exact public blob, which is what the catalog and the
// local trust list name.
//
// Signed bytes are domain separated so a manifest signature can never be
// replayed as a catalog signature (or the other way round).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace luling::app {

inline constexpr char kCatalogSignaturePrefix[] =
    "Luling.Extension.Catalog.Signature\0v1\0";
inline constexpr char kManifestSignaturePrefix[] =
    "Luling.Extension.Manifest.Signature\0v1\0";
inline constexpr std::string_view kCatalogSignatureDomain{kCatalogSignaturePrefix,
                                                          sizeof(kCatalogSignaturePrefix) - 1};
inline constexpr std::string_view kManifestSignatureDomain{kManifestSignaturePrefix,
                                                           sizeof(kManifestSignaturePrefix) - 1};

inline constexpr std::string_view kStoreSignatureAlgorithm = "RSA-PKCS1-SHA256";

// Strict lower/upper-case hex, even length, bounded by `maximum_bytes`.
bool DecodeHex(std::string_view text, size_t maximum_bytes, std::vector<uint8_t>& out);
std::string EncodeHex(const void* data, size_t size);

bool Sha256Bytes(const void* data, size_t size, std::vector<uint8_t>& out);
// Lower-case SHA-256 hex.
bool Sha256BytesHex(const void* data, size_t size, std::string& out);

// Accepts only an RSA public blob: magic "RSA1", 2048-4096 bits, no private
// fields. A well-formed key yields its publisher id (SHA-256 of the blob).
bool PublisherIdFromPublicKey(std::string_view public_key_hex, std::string& id);

// true only when the RSA-PKCS1-SHA256 signature over `message` verifies under
// `public_key_hex`. An unsupported key or a malformed signature is false.
bool VerifyStoreSignature(std::string_view public_key_hex, std::string_view domain,
                          std::string_view message, std::string_view signature_hex);

} // namespace luling::app
