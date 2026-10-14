#include "asset_refile.h"
#include "app_internal.h"
#include "session.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../common/utf8_file.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>

// refile.py built into the menu, rewritten from scratch.
//
// For each selected .png: create a folder named after the file's prefix (the
// text before the first "_", or the stem without its extension), move the png
// into it, copy the containing folder's .xml files in with screenWidth /
// screenHeight and the Image src rewritten from the png, then delete the
// originals from the parent folder.
//
// Every path is stripped of its \\?\ prefix first: the extended form makes
// CreateDirectory/MoveFile fail in ways that look like "nothing happened".
namespace luling {
namespace {

void Trace(const std::wstring& line) {
    FILE* trace = nullptr;
    const std::wstring dir = app::GetLulingDataDir();
    if (dir.empty()) return;
    if (_wfopen_s(&trace, (dir + L"\\refile.log").c_str(), L"a") != 0 || !trace) return;
    std::fprintf(trace, "%ls\n", line.c_str());
    std::fclose(trace);
}

std::wstring Leaf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring Folder(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    if (slash == 2 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, slash);
}

std::wstring ExtLower(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return {};
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext;
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// PNG size from the IHDR chunk: signature, chunk length and type, then width
// and height big-endian.
bool ReadPngSize(const std::wstring& path, uint32_t& width, uint32_t& height) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    uint8_t head[24]{};
    DWORD read = 0;
    const bool ok = ReadFile(file, head, sizeof(head), &read, nullptr) && read == sizeof(head);
    CloseHandle(file);
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (!ok || std::memcmp(head, kSignature, 8) != 0 || std::memcmp(head + 12, "IHDR", 4) != 0)
        return false;
    const auto be32 = [](const uint8_t* p) {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
    };
    width = be32(head + 16);
    height = be32(head + 20);
    return width > 0 && height > 0;
}

// First `name="..."` for this attribute; these XML files are machine-written.
void SetAttribute(std::wstring& xml, const std::wstring& name, const std::wstring& value) {
    const std::wstring needle = name + L"=\"";
    const size_t at = xml.find(needle);
    if (at == std::wstring::npos) return;
    if (at == 0 || (xml[at - 1] != L' ' && xml[at - 1] != L'\t' && xml[at - 1] != L'\n'))
        return;
    const size_t from = at + needle.size();
    const size_t to = xml.find(L'"', from);
    if (to == std::wstring::npos) return;
    xml.replace(from, to - from, value);
}

std::vector<std::wstring> XmlFilesIn(const std::wstring& dir) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((dir + L"\\*.xml").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return names;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            names.push_back(found.cFileName);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace

int RefileAssets(AppState& s, const std::vector<std::wstring>& paths) {
    Trace(L"-- refile: " + std::to_wstring(paths.size()) + L" selected");
    // 1. Selected .png files, with the extended prefix removed.
    std::vector<std::wstring> pngs;
    for (const auto& raw : paths) {
        const std::wstring path = path::StripExtendedPathPrefix(raw);
        if (ExtLower(Leaf(path)) == L".png") pngs.push_back(path);
        else Trace(L"   skip " + path);
    }
    if (pngs.empty()) {
        MessageBoxW(s.hwnd, l10n::Pick(L"没有选中可归档的 PNG 文件。",
                                       L"No PNG files are selected to file."),
                    L"Luling", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    int filed = 0;
    std::wstring problem;
    for (const auto& png : pngs) {
        const std::wstring dir = Folder(png);
        const std::wstring leaf = Leaf(png);
        if (dir.empty() || leaf.empty()) continue;
        // Prefix: text before the first "_"; otherwise the stem without its
        // extension (a whole "6.7.png" would collide with the file itself).
        const size_t underscore = leaf.find(L'_');
        std::wstring prefix;
        if (underscore != std::wstring::npos) {
            prefix = leaf.substr(0, underscore);
        } else {
            const size_t dot = leaf.find_last_of(L'.');
            prefix = dot == std::wstring::npos || dot == 0 ? leaf : leaf.substr(0, dot);
        }
        if (prefix.empty()) continue;

        const std::wstring target_dir = dir + L"\\" + prefix;
        if (!Exists(target_dir)) CreateDirectoryW(target_dir.c_str(), nullptr);
        if (!Exists(target_dir)) {
            problem = L"无法创建文件夹 " + target_dir;
            Trace(L"   cannot create " + target_dir);
            continue;
        }
        const std::wstring target_png = target_dir + L"\\" + leaf;
        if (Exists(target_png)) {
            Trace(L"   already filed " + target_png);
            continue;
        }
        // Captured before the move so the listing is the one the script saw.
        const auto xml_files = XmlFilesIn(dir);
        if (!MoveFileW(png.c_str(), target_png.c_str())) {
            problem = L"无法移动 " + leaf;
            Trace(L"   cannot move " + png + L" err=" + std::to_wstring(GetLastError()));
            continue;
        }
        uint32_t width = 0, height = 0;
        const bool sized = ReadPngSize(target_png, width, height);
        for (const auto& xml : xml_files) {
            const std::wstring source = dir + L"\\" + xml;
            const std::wstring target = target_dir + L"\\" + xml;
            if (Exists(target)) continue;
            std::wstring text;
            if (!ReadUtf8File(source, text)) continue;
            if (sized) {
                SetAttribute(text, L"screenWidth", std::to_wstring(width));
                SetAttribute(text, L"screenHeight", std::to_wstring(height));
            }
            SetAttribute(text, L"src", leaf);
            if (WriteUtf8FileAtomic(target, text)) DeleteFileW(source.c_str());
        }
        ++filed;
        Trace(L"   filed " + leaf + L" -> " + target_dir);
    }

    Trace(L"-- refile done: " + std::to_wstring(filed));
    if (filed) {
        if (const auto* tab = ActiveTab(s)) {
            s.store.MarkDirty(tab->current_path);
            RefreshActiveTab(s);
        }
        return filed;
    }
    const std::wstring message = problem.empty()
        ? l10n::Pick(L"没有文件被归档：目标文件夹可能已存在。",
                     L"Nothing was filed: the target folder may already exist.")
        : problem;
    MessageBoxW(s.hwnd, message.c_str(), L"Luling", MB_OK | MB_ICONINFORMATION);
    return 0;
}

} // namespace luling
