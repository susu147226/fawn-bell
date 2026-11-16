#pragma once
// store_install_util.h — checksum and uninstall helpers for the extension
// store. Kept free of app state so they can be unit tested on their own.

#include <string>

namespace luling::app {

// Lower-case SHA-256 hex of a byte range or a file.
bool Sha256BytesHex(const void* data, size_t size, std::wstring& out);
bool Sha256FileHex(const std::wstring& path, std::wstring& out);

// Removes `<root>\<id>` and the id's entries in installed.json and state.json.
// An id that is not installed is still a success (the entries are simply
// absent).
bool UninstallExtensionFrom(const std::wstring& extensions_root, const std::wstring& id,
                            std::wstring& error);

} // namespace luling::app
