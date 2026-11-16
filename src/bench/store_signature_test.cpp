// Store signature regression: the bytes produced by tools/extensions/sign.py
// must verify in the client, and a signature may not cross domains.
#include "../app/store_signature.h"

#include <cstdio>
#include <string>

namespace {

constexpr char kPublicKey[] =
    "525341310008000003000000000100000000000000000000010001f113813de66415f87f9eaf83ed00759c"
    "bb0400765a2f8bd6566b4c3e8563cd0ab6ea181deba2be5a872d4c5f085d36166415f1f47d6ad3f095de1c"
    "369c36486b0ffe144e0af8bd4ee91e7c8796080ad1d7cf1c3bbb5668021dbbe666b08141c94b2858c75e2"
    "e27a37d23e032a7f6d2a737c1faf218110b24fc36abc49d273d6ce2ff574b5400f348f2a52e3718015c7"
    "d8f85eab05aca1dc5d56ae27b2beb7d59fece80c3d69bfb95a8107cc8954848bd8cf594e56bef52f9408"
    "9ff92aec078a4d0ab7a05b36b5fac4a2bba91301bcf6f3ef5880296e9d49952abccd311e590fd6ef0852"
    "bfb88de980aa85b8ff18ec071b8a3070d5f5d7d8666eab377f114f019";
constexpr char kSignature[] =
    "06ab17aa6a695bad4094355162ad2cbb140469ae04a71846f7c82ab45f4d70ef7d7b2706d1f4ee92a5a0"
    "afc5ac682420e9289d6b3de1bfd6e8d23e1b7de6aa6834206b0bbdba20faada743a9cc0cd98b1582d16d"
    "b04f0fec14f56db659b209a01afe126fb0f0f291cb87b3aaa8de0f8fb3c5beff5d617c79ad88463a2e31"
    "d3edfd334bd7056794ea767add1e341e466df7e5c8e1e26ee30385d72dfdb7d2e9936deff8e74d393084c"
    "32329595e19cfdfca77127c80478ad56849571d90af1eac68941f8960386f644418f007b72b1f57bfcc4"
    "da59ad43526fda4690c77dd801a46236cf2af4d1506320ef943c94ae8f72da83470fa04eef9927a957ae"
    "fa1e0f6";
constexpr char kPublisher[] = "5c2406b71d3b19b4233798a704e704c3203b0098622f09c2313636468dac003b";
constexpr char kMessage[] = "Luling extension store signature self-test\n";

int g_failures = 0;

void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

} // namespace

int main() {
    using namespace luling::app;

    Check(VerifyStoreSignature(kPublicKey, kCatalogSignatureDomain, kMessage, kSignature),
          "catalog signature verifies");
    Check(!VerifyStoreSignature(kPublicKey, kManifestSignatureDomain, kMessage, kSignature),
          "a catalog signature is not a manifest signature");

    std::string tampered = kMessage;
    tampered.back() = 'X';
    Check(!VerifyStoreSignature(kPublicKey, kCatalogSignatureDomain, tampered, kSignature),
          "tampered message is rejected");

    std::string identity;
    Check(PublisherIdFromPublicKey(kPublicKey, identity) && identity == kPublisher,
          "publisher id is the SHA-256 of the public blob");

    Check(!VerifyStoreSignature("00", kCatalogSignatureDomain, kMessage, kSignature),
          "a malformed key is rejected");
    Check(!VerifyStoreSignature(kPublicKey, kCatalogSignatureDomain, kMessage, "00"),
          "a malformed signature is rejected");

    std::string hex;
    Check(Sha256BytesHex("abc", 3, hex) &&
              hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 matches the known vector");

    if (g_failures != 0) std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
