// settings_backup.cpp - the 备份与迁移 card on 关于与诊断.
#include "settings_backup.h"

#include "app_internal.h"
#include "app_commands.h"
#include "app_restart.h"
#include "session.h"
#include "../common/json_value.h"
#include "../common/utf8_file.h"
#include "../ui/confirm_dialog.h"
#include "../ui/folder_picker_dialog.h"

#include <shlobj.h>
#include <shellapi.h>

#include <string>

namespace luling {
namespace {

constexpr const wchar_t* kFiles[] = {L"app.json", L"ai.json", L"context_menu.json"};

std::wstring DataDir() {
    const std::wstring dir = app::GetLulingDataDir();
    return dir.empty() ? std::wstring() : dir;
}

std::wstring AiDir() {
    const std::wstring base = DataDir();
    if (base.empty()) return {};
    return base + L"\\ai";
}

std::wstring FileFor(const wchar_t* name) {
    if (std::wstring(name) == L"ai.json") return AiDir() + L"\\" + name;
    return DataDir() + L"\\" + name;
}

std::wstring Stamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t text[32] = {};
    swprintf_s(text, L"%04u%02u%02u-%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return text;
}

void ShowResult(AppState& s, std::wstring title, std::wstring message, std::wstring confirm,
                std::vector<std::wstring> items = {}) {
    ui::ConfirmDialogSpec spec;
    spec.title = std::move(title);
    spec.message = std::move(message);
    spec.confirm_text = std::move(confirm);
    spec.cancel_text = L"关闭";
    spec.items = std::move(items);
    spec.tone = ui::ConfirmTone::Info;
    spec.default_choice = ui::ConfirmChoice::Confirm;
    (void)ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor);
}

} // namespace

void ExportSettingsBackup(AppState& s) {
    const std::wstring dir = DataDir();
    if (dir.empty()) return;
    ui::FolderPickerSpec picker;
    picker.mode = ui::PickerMode::Folder;
    picker.title = L"选择保存设置备份的文件夹";
    ui::FilePickerResult picked;
    if (!ui::ShowFilePicker(s.hwnd, picker, s.darkMode, s.accentColor, picked) ||
        picked.paths.empty())
        return;
    const std::wstring target = picked.paths.front() + L"\\Luling-settings-" + Stamp() + L".json";

    json::Value root = json::Value::Object();
    root.set("format", "luling-settings");
    root.set("version", static_cast<int64_t>(1));
    json::Value files = json::Value::Object();
    int written = 0;
    for (const wchar_t* name : kFiles) {
        std::wstring text;
        if (!ReadUtf8File(FileFor(name), text)) continue;
        files.set(json::Utf8(name), json::Utf8(text));
        ++written;
    }
    root.set("files", std::move(files));
    const std::string payload = json::Serialize(root) + "\n";
    if (!WriteUtf8FileAtomic(target, json::Wide(payload))) {
        ShowResult(s, L"导出失败", L"无法写入：" + target, L"好");
        return;
    }
    ui::ConfirmDialogSpec spec;
    spec.title = L"设置已导出";
    spec.message = L"备份文件：" + target;
    spec.confirm_text = L"打开文件夹";
    spec.cancel_text = L"完成";
    spec.tone = ui::ConfirmTone::Info;
    spec.default_choice = ui::ConfirmChoice::Confirm;
    if (ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor) ==
        ui::ConfirmChoice::Confirm) {
        const std::wstring args = L"/select,\"" + target + L"\"";
        ShellExecuteW(s.hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

void ImportSettingsBackup(AppState& s) {
    const std::wstring dir = DataDir();
    if (dir.empty()) return;
    ui::FolderPickerSpec picker;
    picker.mode = ui::PickerMode::File;
    picker.title = L"选择设置备份文件";
    picker.filters = {{L"Luling 设置备份", L"*.json"}, {L"所有文件", L"*.*"}};
    ui::FilePickerResult picked;
    if (!ui::ShowFilePicker(s.hwnd, picker, s.darkMode, s.accentColor, picked) ||
        picked.paths.empty())
        return;
    std::wstring text;
    if (!ReadUtf8File(picked.paths.front(), text)) {
        ShowResult(s, L"导入失败", L"无法读取这个文件。", L"好");
        return;
    }
    json::Value root;
    if (!json::Parse(json::Utf8(text), root) || !root.is_object() ||
        root.get_string("format") != "luling-settings") {
        ShowResult(s, L"导入失败", L"这不是鹿铃的设置备份。", L"好");
        return;
    }
    const json::Value* files = root.find("files");
    if (!files || !files->is_object()) {
        ShowResult(s, L"导入失败", L"备份里没有设置内容。", L"好");
        return;
    }
    ui::ConfirmDialogSpec ask;
    ask.title = L"导入设置？";
    ask.message = L"当前的设置会先备份成 .bak 文件，再换成这个备份里的设置。";
    ask.confirm_text = L"导入";
    ask.cancel_text = L"取消";
    ask.tone = ui::ConfirmTone::Warning;
    if (ui::ShowConfirmDialogEx(s.hwnd, ask, s.darkMode, s.accentColor) != ui::ConfirmChoice::Confirm)
        return;

    const std::wstring stamp = Stamp();
    int restored = 0;
    for (const wchar_t* name : kFiles) {
        const std::string content = files->get_string(json::Utf8(name));
        if (content.empty()) continue;
        const std::wstring path = FileFor(name);
        std::wstring existing;
        if (ReadUtf8File(path, existing)) {
            const std::wstring backup = path + L".bak-" + stamp;
            (void)WriteUtf8FileAtomic(backup, existing);
        }
        if (!WriteUtf8FileAtomic(path, json::Wide(content))) continue;
        ++restored;
    }
    if (restored == 0) {
        ShowResult(s, L"导入失败", L"备份里的文件没能写入。", L"好");
        return;
    }
    ui::ConfirmDialogSpec done;
    done.title = L"设置已导入";
    done.message = L"重启鹿铃后全部生效。当前设置备份为 .bak-" + stamp + L"。";
    done.confirm_text = L"立即重启";
    done.cancel_text = L"稍后";
    done.tone = ui::ConfirmTone::Info;
    done.default_choice = ui::ConfirmChoice::Confirm;
    if (ui::ShowConfirmDialogEx(s.hwnd, done, s.darkMode, s.accentColor) == ui::ConfirmChoice::Confirm)
        RestartLuling(s);
}

} // namespace luling
