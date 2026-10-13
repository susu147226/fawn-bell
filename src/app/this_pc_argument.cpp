#include "default_file_manager.h"
#include <windows.h>
#include <cwctype>

namespace pulse::app {
bool IsThisPcArgument(std::wstring_view raw) {
    auto equals = [](std::wstring_view a, std::wstring_view b) {
        return a.size() == b.size() &&
            CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    };
    while (!raw.empty() && (raw.front() == L'"' || iswspace(raw.front()))) raw.remove_prefix(1);
    while (!raw.empty() && (raw.back() == L'"' || raw.back() == L'\\' || iswspace(raw.back())))
        raw.remove_suffix(1);
    constexpr std::wstring_view shell = L"shell:";
    if (raw.size() > shell.size() && equals(raw.substr(0, shell.size()), shell))
        raw.remove_prefix(shell.size());
    return equals(raw, kThisPcParsingName) || equals(raw, L"MyComputerFolder");
}
}
