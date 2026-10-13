#pragma once
#ifdef LULING_WITH_SELFTEST
#include <windows.h>
#include <functional>
#include <string>
namespace luling::ui {
using DialogRecoveryCheck = std::function<void(bool, const char*)>;
void TestAdvancedSearchRecovery(HWND owner, float scale, bool dark, const std::wstring& png,
    const DialogRecoveryCheck& check);
void TestColorPickerFallback(HWND owner, float scale, bool dark, const std::wstring& png,
    const DialogRecoveryCheck& check);
}
#endif
