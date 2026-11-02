// ai_model_key.cpp - DPAPI storage of the model API key (ai_model_key.h).
#include "ai_model_key.h"

#include "ai_providers.h"
#include "ai_settings.h"

#include <windows.h>
#include <dpapi.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <vector>

namespace luling::app::ai {
namespace {

// One stored key: its file under AiDirectory() and its DPAPI entropy, which
// ties the blob to this purpose (another program's DPAPI data, or another
// service's file, does not decrypt here).
struct Slot {
    std::wstring file;
    std::string entropy;
    std::wstring description;
};

std::string Utf8Of(std::wstring_view text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>((std::max)(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                   out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring WideOf(std::string_view text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>((std::max)(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                   out.data(), n);
    return out;
}

// DeepSeek keeps the file the first port used, so keys stored then still
// load; every other service gets its own file and entropy suffix.
Slot SlotFor(std::wstring_view service) {
    std::wstring id(service);
    for (wchar_t& ch : id) {
        if (!(std::iswalnum(ch) || ch == L'-' || ch == L'_')) ch = L'_';
    }
    if (id.empty() || id == L"deepseek")
        return Slot{L"model_key.bin", "Luling.AI.ModelKey.v1", L"Luling model key"};
    return Slot{L"model_key_" + id + L".bin", "Luling.AI.ModelKey.v1." + Utf8Of(id),
                L"Luling model key"};
}

constexpr const wchar_t* kVoiceFile = L"voice_key.bin";
constexpr const char* kVoiceEntropy = "Luling.AI.VoiceKey.v1";
constexpr const wchar_t* kVoiceDesc = L"Luling voice key";
Slot VoiceSlot() { return Slot{kVoiceFile, kVoiceEntropy, kVoiceDesc}; }

std::wstring KeyPath(const Slot& slot) {
    const std::wstring dir = AiDirectory();
    return dir.empty() ? std::wstring() : dir + L"\\" + slot.file;
}

DATA_BLOB Entropy(const Slot& slot) {
    return DATA_BLOB{static_cast<DWORD>(slot.entropy.size()),
                     reinterpret_cast<BYTE*>(const_cast<char*>(slot.entropy.data()))};
}

void Wipe(void* data, size_t bytes) {
    if (data && bytes) SecureZeroMemory(data, bytes);
}

bool ReadAll(const std::wstring& path, std::vector<BYTE>& out) {
    out.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 64 * 1024;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
    }
    CloseHandle(file);
    return ok;
}

bool WriteAll(const std::wstring& path, const BYTE* data, DWORD bytes) {
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, bytes, &written, nullptr) && written == bytes && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

bool SaveKey(const Slot& slot, std::wstring_view key) {
    const std::wstring path = KeyPath(slot);
    if (path.empty() || key.empty() || key.size() > 4096) return false;
    CreateDirectoryW(AiDirectory().c_str(), nullptr);
    DATA_BLOB in{static_cast<DWORD>(key.size() * sizeof(wchar_t)),
                 reinterpret_cast<BYTE*>(const_cast<wchar_t*>(key.data()))};
    DATA_BLOB entropy = Entropy(slot);
    DATA_BLOB out{};
    if (!CryptProtectData(&in, slot.description.c_str(), &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    const bool ok = WriteAll(path, out.pbData, out.cbData);
    LocalFree(out.pbData);
    return ok;
}

bool LoadKey(const Slot& slot, std::wstring& key) {
    key.clear();
    const std::wstring path = KeyPath(slot);
    std::vector<BYTE> blob;
    if (path.empty() || !ReadAll(path, blob)) return false;
    DATA_BLOB in{static_cast<DWORD>(blob.size()), blob.data()};
    DATA_BLOB entropy = Entropy(slot);
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    if (out.cbData % sizeof(wchar_t) == 0)
        key.assign(reinterpret_cast<const wchar_t*>(out.pbData), out.cbData / sizeof(wchar_t));
    Wipe(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return !key.empty();
}

bool HasKey(const Slot& slot) {
    const std::wstring path = KeyPath(slot);
    if (path.empty()) return false;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool ClearKey(const Slot& slot) {
    const std::wstring path = KeyPath(slot);
    if (path.empty()) return false;
    return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

} // namespace

std::wstring CurrentService() {
    bool malformed = false;
    const Settings st = LoadSettings(&malformed);
    if (!st.assistant.provider.empty()) return WideOf(st.assistant.provider);
    const std::wstring stored = WideOf(st.assistant.base_url);
    if (!stored.empty()) {
        if (const ProviderInfo* p = ProviderByBase(stored)) return WideOf(p->id);
        return L"custom";
    }
    return L"deepseek";
}

bool SaveModelKeyFor(std::wstring_view service, std::wstring_view key) {
    return SaveKey(SlotFor(service), key);
}
bool LoadModelKeyFor(std::wstring_view service, std::wstring& key) {
    return LoadKey(SlotFor(service), key);
}
bool HasModelKeyFor(std::wstring_view service) { return HasKey(SlotFor(service)); }
bool ClearModelKeyFor(std::wstring_view service) { return ClearKey(SlotFor(service)); }

bool SaveModelKey(std::wstring_view key) { return SaveModelKeyFor(CurrentService(), key); }
bool LoadModelKey(std::wstring& key) { return LoadModelKeyFor(CurrentService(), key); }
bool HasModelKey() { return HasModelKeyFor(CurrentService()); }
bool ClearModelKey() { return ClearModelKeyFor(CurrentService()); }

bool SaveVoiceKey(std::wstring_view key) { return SaveKey(VoiceSlot(), key); }
bool LoadVoiceKey(std::wstring& key) { return LoadKey(VoiceSlot(), key); }
bool HasVoiceKey() { return HasKey(VoiceSlot()); }
bool ClearVoiceKey() { return ClearKey(VoiceSlot()); }

std::wstring MaskModelKey(std::wstring_view key) {
    if (key.empty()) return {};
    const size_t tail = key.size() >= 8 ? 4 : 0;
    std::wstring head(key.substr(0, (std::min)(key.size(), static_cast<size_t>(3))));
    return head + L"…" + std::wstring(key.substr(key.size() - tail));
}

} // namespace luling::app::ai
