// convert_dialog.h — Modal preview UI for 转换 (format / resize / compress).
//
// Same shape as batch_rename_dialog: the options drive a live preview column,
// and what the user approves here is exactly the list the worker executes.
#pragma once
#include "../app/convert_rules.h"
#include <windows.h>
#include <d2d1.h>
#include <string>
#include <vector>

namespace luling::ui {

struct ConvertDialogResult {
    bool accepted = false;
    app::ConvertOptions options;
    std::vector<app::ConvertItem> items;
};

ConvertDialogResult ShowConvertDialog(HWND owner,
                                      const std::vector<std::wstring>& paths,
                                      bool dark,
                                      D2D1_COLOR_F accent,
                                      app::ConvertMode mode,
                                      const app::ConvertOptions* initial = nullptr);

} // namespace luling::ui
