// shell_file_icon.h — the registered icon for a file type, used by the 新建
// menu so each row shows what it will create (Word's icon for .docx, the
// packer's for .zip, the contact card for .contact) instead of one generic
// document glyph. Cached per extension; registry-only, never opens a file.
#pragma once
#include "../common/menu_icon.h"

#include <string>

namespace luling::ui {

// Large shell icon (32 px) of `extension` ("", ".txt", …). `directory` asks
// for the folder icon instead. Returns null when the shell cannot answer.
common::MenuIconRef FileTypeIcon(const std::wstring& extension, bool directory = false);

} // namespace luling::ui
