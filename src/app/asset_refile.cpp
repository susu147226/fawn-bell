#include "asset_refile.h"
#include "app_internal.h"
#include "../common/path_utils.h"
#include "../common/utf8_file.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <cwctype>

namespace luling {
namespace {

std::wstring Lower(std::wstring value) {
    for (auto& c : value) c = static_cast<wchar_t>(towlower(c));
    return value;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ParentOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    if (slash == 2 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, slash);
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::wstring ExtensionOf(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? std::wstring{} : Lower(name.substr(dot));
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// PNG size straight from the IHDR chunk: 8-byte signature, chunk length and
// type, then width and height, all big-endian. No decoder needed.
bool ReadPngSize(const std::wstring& path, uint32_t& width, uint32_t& height) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    uint8_t head[24]{};
    DWORD read = 0;
    const bool ok = ReadFile(file, head, sizeof(head), &read, nullptr) && read == sizeof(head);
    CloseHandle(file);
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (!ok || std::memcmp(head, kSignature, 8) != 0 ||
        std::memcmp(head + 12, "IHDR", 4) != 0)
        return false;
    const auto be32 = [](const uint8_t* p) {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
    };
    width = be32(head + 16);
    height = be32(head + 20);
    return width > 0 && height > 0;
}

// Replaces the value of the first `name="..."` attribute. The XML in these
// files is machine-generated, so a targeted substitution beats a full parser.
void RewriteAttribute(std::wstring& xml, const std::wstring& name, const std::wstring& value) {
    const std::wstring needle = name + L"=\"";
    const size_t start = xml.find(needle);
    if (start == std::wstring::npos) return;
    if (start == 0 || (xml[start - 1] != L' ' && xml[start - 1] != L'\t' && xml[start - 1] != L'\n'))
        return;
    const size_t value_start = start + needle.size();
    const size_t value_end = xml.find(L'"', value_start);
    if (value_end == std::wstring::npos) return;
    xml.replace(value_start, value_end - value_start, value);
}

std::vector<std::wstring> XmlFilesIn(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW(Join(dir, L"*.xml").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return out;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            out.push_back(found.cFileName);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

int RefileAssets(AppState& s, const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> pngs;
    for (const auto& path : paths)
        if (ExtensionOf(FileNameOf(path)) == L".png") pngs.push_back(path);
    if (pngs.empty()) return 0;
    int filed = 0;
    for (const auto& png : pngs) {
        const std::wstring dir = ParentOf(png);
        const std::wstring name = FileNameOf(png);
        const size_t underscore = name.find(L'_');
        const std::wstring prefix = name.substr(0, underscore == std::wstring::npos
                                                       ? name.size()
                                                       : underscore);
        if (dir.empty() || prefix.empty()) continue;
        const std::wstring target_dir = Join(dir, prefix);
        CreateDirectoryW(target_dir.c_str(), nullptr);
        if (!PathExists(target_dir)) continue;
        const std::wstring target_png = Join(target_dir, name);
        if (PathExists(target_png)) continue;
        // The XML files are captured before the move so the folder listing is
        // the same one the script saw.
        const auto xml_files = XmlFilesIn(dir);
        if (!MoveFileW(png.c_str(), target_png.c_str())) continue;
        uint32_t width = 0, height = 0;
        const bool have_size = ReadPngSize(target_png, width, height);
        for (const auto& xml : xml_files) {
            const std::wstring source = Join(dir, xml);
            const std::wstring target = Join(target_dir, xml);
            if (PathExists(target)) continue;
            std::wstring text;
            if (!ReadUtf8File(source, text)) continue;
            if (have_size) {
                RewriteAttribute(text, L"screenWidth", std::to_wstring(width));
                RewriteAttribute(text, L"screenHeight", std::to_wstring(height));
            }
            RewriteAttribute(text, L"src", name);
            if (!WriteUtf8FileAtomic(target, text)) continue;
            DeleteFileW(source.c_str());
        }
        ++filed;
    }
    if (filed) {
        if (const auto* tab = ActiveTab(s)) {
            s.store.MarkDirty(tab->current_path);
            RefreshActiveTab(s);
        }
    }
    return filed;
}

} // namespace luling
