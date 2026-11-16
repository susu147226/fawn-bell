#pragma once
// store_trust_ui.h — the one explicit consent dialog before a publisher's
// code may be installed. Catalog authentication is not consent: the user sees
// the publisher name and fingerprint, and only a confirmation records trust.

#include "store_catalog.h"

#include <windows.h>

namespace luling::app {

// Shows the publisher fingerprint and asks once. true means the publisher is
// now trusted (or already was).
bool ConfirmStorePublisherTrust(HWND owner, const StorePublisher& publisher);

} // namespace luling::app
