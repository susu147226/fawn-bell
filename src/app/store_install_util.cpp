#include "store_install_util.h"

#include "../common/json_value.h"

#include <windows.h>
#include <bcrypt.h>

#include <cwctype>
#include <filesystem>
#include <vector>

namespace luling::app {
namespace {

bool ValidId(const std::wstring& id) {
    if (id.empty() || id.size() > 64) return false;
    for (wchar_t c : id) {
        if (std::iswalnum(c) || c == L'-' || c == L'_' || c == L'.') continue;
        return false;
    }
    return true;
}

std::wstring HexOf(const std::vector<UCHAR>& bytes) {
    static const wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(bytes.size() * 2);
    for (UCHAR byte : bytes) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

bool ReadAllBytes(const std::wstring& path, std::string& out) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return false;
    char buffer[16384];
    size_t n = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, n);
    fclose(file);
    return !out.empty();
}

void WriteAllBytes(const std::wstring& path, std::string_view bytes) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return;
    fwrite(bytes.data(), 1, bytes.size(), file);
    fclose(file);
}

// Drops one id from a list-of-objects file (<key>: [...]), leaving every other
// member as it was.
bool RemoveListEntry(const std::wstring& path, const char* key, const std::wstring& id) {
    std::string body;
    if (!ReadAllBytes(path, body)) return true;  // nothing recorded
    json::Value root;
    if (!json::Parse(body, root, nullptr) || !root.is_object()) return false;
    const json::Value* list = root.find(key);
    if (!list || !list->is_array()) return true;
    json::Value items = json::Value::Array();
    for (const auto& item : list->items()) {
        if (item.is_object() && json::Wide(item.get_string("id")) == id) continue;
        items.push(item);
    }
    json::Value next = json::Value::Object();
    for (const std::string& member : root.keys()) {
        if (member == key) continue;
        if (const json::Value* value = root.find(member)) next.set(member, *value);
    }
    next.set(key, std::move(items));
    WriteAllBytes(path, json::Serialize(next));
    return true;
}

} // namespace

bool Sha256BytesHex(const void* data, size_t size, std::wstring& out) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD written = 0;
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                      sizeof(object_size), &written, 0);
    BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size),
                      sizeof(hash_size), &written, 0);
    std::vector<UCHAR> object(object_size);
    std::vector<UCHAR> hash(hash_size);
    BCRYPT_HASH_HANDLE handle = nullptr;
    bool ok = BCryptCreateHash(algorithm, &handle, object.data(), object_size, nullptr, 0, 0) >= 0;
    if (ok)
        ok = BCryptHashData(handle, static_cast<PUCHAR>(const_cast<void*>(data)),
                            static_cast<ULONG>(size), 0) >= 0;
    if (ok) ok = BCryptFinishHash(handle, hash.data(), hash_size, 0) >= 0;
    if (handle) BCryptDestroyHash(handle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return false;
    out = HexOf(hash);
    return true;
}

bool Sha256FileHex(const std::wstring& path, std::wstring& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD written = 0;
    if (ok) {
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                          sizeof(object_size), &written, 0);
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size),
                          sizeof(hash_size), &written, 0);
    }
    std::vector<UCHAR> object(object_size);
    std::vector<UCHAR> hash(hash_size);
    BCRYPT_HASH_HANDLE handle = nullptr;
    if (ok)
        ok = BCryptCreateHash(algorithm, &handle, object.data(), object_size, nullptr, 0, 0) >= 0;
    std::vector<UCHAR> buffer(256 * 1024);
    while (ok) {
        DWORD got = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) ||
            got == 0)
            break;
        ok = BCryptHashData(handle, buffer.data(), got, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(handle, hash.data(), hash_size, 0) >= 0;
    if (handle) BCryptDestroyHash(handle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!ok) return false;
    out = HexOf(hash);
    return true;
}

bool UninstallExtensionFrom(const std::wstring& extensions_root, const std::wstring& id,
                            std::wstring& error) {
    if (!ValidId(id)) {
        error = L"invalid extension id";
        return false;
    }
    const std::wstring dir = extensions_root + L"\\" + id;
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(dir), ec);
        if (ec) {
            error = L"could not delete the extension folder";
            return false;
        }
    }
    RemoveListEntry(extensions_root + L"\\installed.json", "installed", id);
    RemoveListEntry(extensions_root + L"\\state.json", "extensions", id);
    return true;
}

} // namespace luling::app
