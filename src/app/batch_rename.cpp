#include "batch_rename.h"
#include "../fs/fs_enum.h"
#include "../common/rename_filename.h"
#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace luling::app {
namespace {

std::wstring FileNameOf(const std::wstring& path) {
    std::wstring_view view = path;
    while (view.size() > 1 && (view.back() == L'\\' || view.back() == L'/'))
        view.remove_suffix(1);
    const size_t slash = view.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? std::wstring(view)
                                            : std::wstring(view.substr(slash + 1));
}

std::wstring ParentOf(const std::wstring& path) {
    std::wstring copy = path;
    while (copy.size() > 1 && (copy.back() == L'\\' || copy.back() == L'/')) copy.pop_back();
    const size_t slash = copy.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return copy.substr(0, slash);
}

std::wstring JoinName(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

wchar_t Fold(wchar_t c) {
    return static_cast<wchar_t>(towlower(c));
}

std::wstring ReplaceInsensitive(const std::wstring& text, const std::wstring& find,
                                const std::wstring& replace) {
    if (find.empty()) return text;
    std::wstring out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (i + find.size() <= text.size()) {
            bool match = true;
            for (size_t n = 0; n < find.size(); ++n) {
                if (Fold(text[i + n]) != Fold(find[n])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                out += replace;
                i += find.size();
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

std::wstring FormatNumber(int64_t value, int digits) {
    wchar_t buf[32]{};
    if (digits > 0) swprintf_s(buf, L"%0*lld", std::min(digits, 9), static_cast<long long>(value));
    else swprintf_s(buf, L"%lld", static_cast<long long>(value));
    return buf;
}

bool ConsumeToken(std::wstring_view pattern, size_t& i, std::wstring_view token) {
    if (pattern.size() - i < token.size()) return false;
    if (pattern.substr(i, token.size()) != token) return false;
    i += token.size();
    return true;
}

std::wstring ExpandPattern(std::wstring_view pattern, std::wstring_view stem,
                           std::wstring_view ext, int64_t value) {
    std::wstring out;
    out.reserve(pattern.size() + stem.size() + ext.size() + 8);
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == L'{') {
            if (ConsumeToken(pattern, i, L"{name}")) {
                out.append(stem);
                continue;
            }
            if (ConsumeToken(pattern, i, L"{ext}")) {
                out.append(ext);
                continue;
            }
            if (ConsumeToken(pattern, i, L"{n}")) {
                out += FormatNumber(value, 0);
                continue;
            }
            if (pattern.size() - i >= 3 && pattern.substr(i, 3) == L"{n:") {
                size_t j = i + 3;
                int pad = 0;
                while (j < pattern.size() && pattern[j] >= L'0' && pattern[j] <= L'9') {
                    pad = std::min(9, pad * 10 + (pattern[j] - L'0'));
                    ++j;
                }
                if (j < pattern.size() && pattern[j] == L'}') {
                    i = j + 1;
                    out += FormatNumber(value, std::clamp(pad, 0, 9));
                    continue;
                }
            }
        }
        out += pattern[i++];
    }
    return out;
}

bool PatternUsesIndex(std::wstring_view pattern) {
    size_t i = 0;
    while (i < pattern.size()) {
        if (ConsumeToken(pattern, i, L"{name}") || ConsumeToken(pattern, i, L"{ext}"))
            continue;
        if (ConsumeToken(pattern, i, L"{n}")) return true;
        if (pattern.size() - i >= 3 && pattern.substr(i, 3) == L"{n:") {
            size_t j = i + 3;
            while (j < pattern.size() && pattern[j] >= L'0' && pattern[j] <= L'9') ++j;
            if (j < pattern.size() && pattern[j] == L'}') return true;
        }
        ++i;
    }
    return false;
}

} // namespace

bool BatchRenamePatternUsesIndex(std::wstring_view pattern) {
    return PatternUsesIndex(pattern);
}

void SplitFileName(const std::wstring& name, std::wstring& stem, std::wstring& ext) {
    stem = name;
    ext.clear();
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return;
    stem = name.substr(0, dot);
    ext = name.substr(dot);
}

bool IsValidFileName(const std::wstring& name) {
    return IsRenameFilename(name);
}

std::wstring ApplyBatchRenameRule(const std::wstring& name, size_t index,
                                  const BatchRenameRule& rule) {
    std::wstring working = name;
    if (!rule.find.empty()) {
        if (rule.replace_in_extension) {
            working = ReplaceInsensitive(working, rule.find, rule.replace);
        } else {
            std::wstring stem, ext;
            SplitFileName(working, stem, ext);
            working = ReplaceInsensitive(stem, rule.find, rule.replace) + ext;
        }
    }
    std::wstring stem, ext;
    SplitFileName(working, stem, ext);
    const int64_t value = static_cast<int64_t>(rule.start) + static_cast<int64_t>(index);
    if (!rule.pattern.empty()) {
        return ExpandPattern(rule.pattern, stem, ext, value);
    }
    stem = rule.prefix + stem + rule.suffix;
    if (rule.insert_number) {
        stem += L" (" + FormatNumber(value, rule.digits) + L")";
    }
    return stem + ext;
}

std::vector<BatchRenameItem> PreviewBatchRename(const std::vector<std::wstring>& paths,
                                                const BatchRenameRule& rule) {
    std::vector<BatchRenameItem> items;
    items.reserve(paths.size());
    std::unordered_map<std::wstring, int> new_counts;
    // Grouped mode (the 按序号 menu commands) numbers every extension group
    // from rule.start in natural order, so a.jpg b.jpg c.png becomes
    // a_0.jpg b_1.jpg c_0.png.
    std::vector<size_t> index_of(paths.size());
    for (size_t i = 0; i < index_of.size(); ++i) index_of[i] = i;
    if (rule.group_by_extension && paths.size() > 1) {
        auto group_ext = [&](size_t i) {
            std::wstring stem, ext;
            SplitFileName(FileNameOf(paths[i]), stem, ext);
            for (auto& c : ext) c = Fold(c);
            return ext;
        };
        std::vector<size_t> order(paths.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const std::wstring ea = group_ext(a);
            const std::wstring eb = group_ext(b);
            if (ea != eb) return ea < eb;
            return StrCmpLogicalW(FileNameOf(paths[a]).c_str(),
                                  FileNameOf(paths[b]).c_str()) < 0;
        });
        std::wstring current;
        size_t counter = 0;
        for (const size_t i : order) {
            const std::wstring ext = group_ext(i);
            if (ext != current) {
                current = ext;
                counter = 0;
            }
            index_of[i] = counter++;
        }
    }
    for (size_t i = 0; i < paths.size(); ++i) {
        BatchRenameItem item;
        item.source_path = paths[i];
        item.original_name = FileNameOf(paths[i]);
        item.new_name = ApplyBatchRenameRule(item.original_name, index_of[i], rule);
        if (item.new_name.empty()) item.status = BatchRenameStatus::Empty;
        else if (!IsValidFileName(item.new_name)) item.status = BatchRenameStatus::Invalid;
        else if (item.new_name == item.original_name)
            item.status = BatchRenameStatus::Unchanged;
        items.push_back(std::move(item));
        if (items.back().status == BatchRenameStatus::Ok) {
            std::wstring key = JoinName(ParentOf(paths[i]), items.back().new_name);
            for (auto& c : key) c = Fold(c);
            new_counts[key] += 1;
        }
    }
    for (auto& item : items) {
        if (item.status != BatchRenameStatus::Ok) continue;
        std::wstring key = JoinName(ParentOf(item.source_path), item.new_name);
        const std::wstring dest = key;
        for (auto& c : key) c = Fold(c);
        if (new_counts[key] > 1) {
            item.status = BatchRenameStatus::Collision;
            continue;
        }
        const std::wstring normalized = fs::NormalizePath(dest);
        if (GetFileAttributesW(normalized.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        const std::wstring source = fs::NormalizePath(item.source_path);
        if (_wcsicmp(normalized.c_str(), source.c_str()) != 0)
            item.status = BatchRenameStatus::Collision;
    }
    return items;
}

} // namespace luling::app
