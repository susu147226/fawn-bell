// shell_new.h — HKCR\<ext>\ShellNew entries for the 新建 submenu.
//
// Explorer builds 新建 from the registry: every extension that carries a
// ShellNew key contributes a row. Three shapes matter — NullFile (create an
// empty file), FileName (copy a template file) and Data (write raw bytes).
// The Command shape is deliberately skipped: it runs an arbitrary program.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace luling::app {

struct ShellNewItem {
    std::wstring display;        // menu label, localized when the registry says so
    std::wstring extension;      // ".docx"
    std::wstring template_path;  // FileName shape, environment expanded
    std::vector<uint8_t> data;   // Data shape
};

// Every usable ShellNew entry, display-name sorted. Enumerated once per
// process; call InvalidateShellNewCache after an install.
const std::vector<ShellNewItem>& ShellNewItems();
void InvalidateShellNewCache();

} // namespace luling::app
