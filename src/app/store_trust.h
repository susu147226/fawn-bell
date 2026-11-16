#pragma once
// store_trust.h — publishers the user has explicitly trusted.
//
// Catalog authentication proves the official catalog said something; it is not
// the user's consent for a third-party publisher's code to run. Installing an
// extension from an untrusted publisher first records that consent here, keyed
// by the publisher id (SHA-256 of its public key blob).

#include <string>
#include <string_view>
#include <vector>

namespace luling::app {

struct TrustedPublisher {
    std::wstring id;          // publisher id, SHA-256 of the public key blob
    std::wstring name;
    std::wstring public_key;  // lower-case hex CNG RSAPUBLICBLOB
};

std::vector<TrustedPublisher> TrustedPublishers();
bool IsPublisherTrusted(std::wstring_view id);
bool FindTrustedPublisher(std::wstring_view id, TrustedPublisher& out);

// Records explicit consent. The id must match the key, and an id already
// trusted under a different key is refused instead of replaced.
bool TrustPublisher(const TrustedPublisher& publisher, std::wstring& error);
bool UntrustPublisher(std::wstring_view id, std::wstring& error);

} // namespace luling::app
