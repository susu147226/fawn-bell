// ai_tools.cpp - The read-only tools (ai_tools.h).
#include "ai_tools.h"

#include "../fs/fs_enum.h"

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace luling::app::ai {
namespace {

constexpr size_t kMaxListItems = 500;

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool SameNoCase(const std::wstring& a, const wchar_t* b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b, -1, TRUE) == CSTR_EQUAL;
}

// "Downloads", "下载": the folders a model is likely to name.
bool KnownFolder(const std::wstring& name, std::wstring& out) {
    struct Row {
        const wchar_t* en;
        const wchar_t* zh;
        REFKNOWNFOLDERID id;
    };
    static const Row kRows[] = {
        {L"downloads", L"下载", FOLDERID_Downloads},
        {L"desktop", L"桌面", FOLDERID_Desktop},
        {L"documents", L"文档", FOLDERID_Documents},
        {L"pictures", L"图片", FOLDERID_Pictures},
        {L"videos", L"视频", FOLDERID_Videos},
        {L"music", L"音乐", FOLDERID_Music},
        {L"home", L"主文件夹", FOLDERID_Profile},
    };
    const std::wstring key = Lower(name);
    for (const Row& row : kRows) {
        if (!SameNoCase(key, row.en) && !SameNoCase(key, row.zh)) continue;
        PWSTR path = nullptr;
        if (FAILED(SHGetKnownFolderPath(row.id, KF_FLAG_DEFAULT, nullptr, &path))) return false;
        out = path;
        CoTaskMemFree(path);
        return true;
    }
    return false;
}

// "2026-10-30" from a FILETIME; empty when the source had none.
std::string DateText(const FILETIME& ft) {
    if (!ft.dwHighDateTime && !ft.dwLowDateTime) return {};
    FILETIME local{};
    if (!FileTimeToLocalFileTime(&ft, &local)) return {};
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&local, &st)) return {};
    char buf[16];
    sprintf_s(buf, "%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

json::Value StringField(const wchar_t* desc) {
    json::Value v = json::Value::Object();
    v.set("type", "string");
    v.set("description", json::Utf8(desc));
    return v;
}

json::Value ToolDef(const char* name, const char* desc, json::Value props, json::Value required) {
    json::Value fn = json::Value::Object();
    fn.set("name", name);
    fn.set("description", desc);
    json::Value params = json::Value::Object();
    params.set("type", "object");
    params.set("properties", std::move(props));
    params.set("required", std::move(required));
    fn.set("parameters", std::move(params));
    json::Value tool = json::Value::Object();
    tool.set("type", "function");
    tool.set("function", std::move(fn));
    return tool;
}

const json::Value& BuildTools() {
    static const json::Value tools = [] {
        json::Value list = json::Value::Array();
        list.push(ToolDef(
            "get_state",
            "The window's current state: the folder open in the active tab, how many items it has, "
            "what is selected, the view and the sort. Call it when the request is about this folder, "
            "the selection or what is open.",
            json::Value::Object(), json::Value::Array()));
        json::Value props = json::Value::Object();
        props.set("path",
                  StringField(L"Folder to list: an absolute path, a drive (D:), or a known folder "
                             L"(Downloads, Desktop, Documents, Pictures, Videos, Music, or the "
                             L"Chinese names 下载 桌面 文档 图片 视频 音乐). Omit for the folder open "
                             L"in the window."));
        json::Value limit = json::Value::Object();
        limit.set("type", "integer");
        limit.set("description", "How many items to return at most (1-500, default 200).");
        props.set("limit", std::move(limit));
        {
            json::Value orgProps = json::Value::Object();
            orgProps.set("folder", StringField(L"Absolute path of the folder whose items move."));
            json::Value itemProps = json::Value::Object();
            itemProps.set("name", StringField(L"Subfolder name: short, one segment, no slashes or colons."));
            json::Value items = json::Value::Object();
            items.set("type", "array");
            items.set("description", json::Utf8(L"Names of items directly inside the folder."));
            json::Value itemType = json::Value::Object();
            itemType.set("type", "string");
            items.set("items", std::move(itemType));
            itemProps.set("items", std::move(items));
            json::Value itemReq = json::Value::Array();
            itemReq.push("name");
            itemReq.push("items");
            json::Value group = json::Value::Object();
            group.set("type", "object");
            group.set("properties", std::move(itemProps));
            group.set("required", std::move(itemReq));
            json::Value groups = json::Value::Object();
            groups.set("type", "array");
            groups.set("description", json::Utf8(L"One entry per destination subfolder."));
            groups.set("items", std::move(group));
            orgProps.set("groups", std::move(groups));
            json::Value req = json::Value::Array();
            req.push("folder");
            req.push("groups");
            list.push(ToolDef(
                "organize_files",
                "Moves items of one folder into subfolders of that folder. Missing subfolders are "
                "created, existing ones reused; nothing is overwritten (a clashing name gets a suffix). "
                "The user confirms the plan on a card in the app before anything moves, and can undo it "
                "there; the result says whether it ran. After it runs, say in one sentence what was done.",
                std::move(orgProps), std::move(req)));
        }
        {
            json::Value delProps = json::Value::Object();
            json::Value paths = json::Value::Object();
            paths.set("type", "array");
            paths.set("description", json::Utf8(L"Absolute paths of the items; omit for the items selected in the window."));
            json::Value pathType = json::Value::Object();
            pathType.set("type", "string");
            paths.set("items", std::move(pathType));
            delProps.set("paths", std::move(paths));
            json::Value permanent = json::Value::Object();
            permanent.set("type", "boolean");
            permanent.set("description",
                          json::Utf8(L"true only when the user asks to delete something for good; the default moves "
                                     L"to the Recycle Bin, which can be undone."));
            delProps.set("permanent", std::move(permanent));
            json::Value req = json::Value::Array();
            list.push(ToolDef(
                "delete_files",
                "Moves items to the Recycle Bin, where the user can restore them and the result card can "
                "undo it; set permanent true only when the user asks to delete something for good (that "
                "takes two clicks on a red card). Either way the user confirms in the app before anything "
                "is removed. Network shares and mapped drives have no Recycle Bin: there only permanent true works. "
                "never propose deleting Windows folders, Program Files or a whole drive.",
                std::move(delProps), std::move(req)));
        }
        {
            json::Value rnProps = json::Value::Object();
            json::Value paths = json::Value::Object();
            paths.set("type", "array");
            paths.set("description", json::Utf8(L"Absolute paths of the files; omit to rename the files selected in the window."));
            json::Value pathType = json::Value::Object();
            pathType.set("type", "string");
            paths.set("items", std::move(pathType));
            rnProps.set("paths", std::move(paths));
            json::Value stepProps = json::Value::Object();
            json::Value type = json::Value::Object();
            type.set("type", "string");
            json::Value types = json::Value::Array();
            types.push("replace");
            types.push("template");
            type.set("enum", std::move(types));
            stepProps.set("type", std::move(type));
            stepProps.set("find", StringField(L"replace: the text to look for."));
            stepProps.set("with", StringField(L"replace: what takes its place."));
            stepProps.set("pattern",
                          StringField(L"template: the whole new name. Tokens: {name} (original name without "
                                      L"extension), {ext}, {n} (running number)."));
            json::Value start = json::Value::Object();
            start.set("type", "integer");
            start.set("description", json::Utf8(L"template: the first number (default 1)."));
            stepProps.set("start", std::move(start));
            json::Value digits = json::Value::Object();
            digits.set("type", "integer");
            digits.set("description", json::Utf8(L"template: pad {n} to this many digits (0-9, default none)."));
            stepProps.set("digits", std::move(digits));
            json::Value stepType = json::Value::Object();
            stepType.set("type", "string");
            json::Value stepReq = json::Value::Array();
            stepReq.push("type");
            json::Value step = json::Value::Object();
            step.set("type", "object");
            step.set("properties", std::move(stepProps));
            step.set("required", std::move(stepReq));
            json::Value steps = json::Value::Object();
            steps.set("type", "array");
            steps.set("description", json::Utf8(L"Applied in order: a replace step and/or a template step."));
            steps.set("items", std::move(step));
            rnProps.set("steps", std::move(steps));
            json::Value req = json::Value::Array();
            req.push("steps");
            list.push(ToolDef(
                "rename_files",
                "Renames files by one rule that the app applies on this PC. You give the rule, not the new "
                "names: the app makes every name, shows old and new, and asks the user on a card before "
                "anything changes, and the user can undo it there. Supported steps: {type:replace, find, "
                "with} and {type:template, pattern, start, digits} with the tokens {name}, {ext} and {n}. The "
                "extension stays unless the pattern names {ext}.",
                std::move(rnProps), std::move(req)));
        }
        list.push(ToolDef(
            "list_folder",
            "Lists one folder: folders first, then files, with sizes and modified dates. Use it "
            "before answering any question about a folder's contents. It only reads names and "
            "metadata; it never opens files.",
            std::move(props), json::Value::Array()));
        {
            json::Value revealProps = json::Value::Object();
            revealProps.set("path", StringField(L"Absolute path to show. A folder opens itself; a file opens the folder that contains it."));
            json::Value req = json::Value::Array();
            req.push("path");
            list.push(ToolDef(
                "reveal",
                "Shows a path in Luling: a folder opens in the window, a file opens its containing folder. "
                "It only navigates; it never changes, opens or deletes anything.",
                std::move(revealProps), std::move(req)));
        }
        list.push(ToolDef(
            "list_tabs",
            "Lists the open window tabs: their index, the folder each shows and which one is active. Use it "
            "before switching tabs or when the user talks about another tab.",
            json::Value::Object(), json::Value::Array()));
        {
            json::Value openProps = json::Value::Object();
            openProps.set("path", StringField(L"Absolute folder path (or a drive / known folder name)."));
            json::Value newTab = json::Value::Object();
            newTab.set("type", "boolean");
            newTab.set("description", json::Utf8(L"true (default): open in a new tab; false: use the active tab."));
            openProps.set("new_tab", std::move(newTab));
            json::Value req = json::Value::Array();
            req.push("path");
            list.push(ToolDef(
                "open_tab",
                "Opens a folder in Luling, in a new tab by default. Navigation only; nothing on disk changes.",
                std::move(openProps), std::move(req)));
        }
        auto transferProps = [&](bool move) {
            json::Value props = json::Value::Object();
            json::Value paths = json::Value::Object();
            paths.set("type", "array");
            paths.set("description", json::Utf8(L"Absolute paths of the items."));
            json::Value item = json::Value::Object();
            item.set("type", "string");
            paths.set("items", std::move(item));
            props.set("paths", std::move(paths));
            props.set("dest", StringField(move ? L"Absolute destination folder to move the items into."
                                               : L"Absolute destination folder to copy the items into."));
            json::Value req = json::Value::Array();
            req.push("paths");
            req.push("dest");
            list.push(ToolDef(
                move ? "move_files" : "copy_files",
                move ? "Moves files or folders into a folder. Nothing is overwritten (a clashing name gets a "
                       "suffix). The user confirms on a card in Luling first, and can undo it there."
                     : "Copies files or folders into a folder. Nothing is overwritten (a clashing name gets a "
                       "suffix). The user confirms on a card in Luling first, and can undo it there.",
                std::move(props), std::move(req)));
        };
        transferProps(false);
        transferProps(true);
        {
            json::Value folderProps = json::Value::Object();
            folderProps.set("parent", StringField(L"Absolute path of the folder to create the new folder in."));
            folderProps.set("name", StringField(L"New folder name: one segment, no slashes or colons."));
            json::Value req = json::Value::Array();
            req.push("parent");
            req.push("name");
            list.push(ToolDef(
                "create_folder",
                "Creates one folder. The user confirms on a card in Luling and can undo it there.",
                std::move(folderProps), std::move(req)));
        }
        list.push(ToolDef(
            "undo_last",
            "Undoes the newest file action Luling did for AI apps (a copy, move, rename, delete or folder "
            "creation), when it is still the newest step. Only one step per call; say what was undone.",
            json::Value::Object(), json::Value::Array()));
        list.push(ToolDef(
            "get_settings",
            "Reads a few Luling settings: theme, row height, language, hidden and protected files, status "
            "performance. Use it before changing one, so the answer matches what the user sees.",
            json::Value::Object(), json::Value::Array()));
        {
            json::Value setProps = json::Value::Object();
            json::Value key = json::Value::Object();
            key.set("type", "string");
            json::Value keys = json::Value::Array();
            for (const char* name : {"theme", "row_height", "language", "show_hidden", "show_protected",
                                     "show_performance", "list_smart_date", "list_zebra_rows",
                                     "list_size_bar"})
                keys.push(name);
            key.set("enum", std::move(keys));
            setProps.set("key", std::move(key));
            setProps.set("value", StringField(L"New value: theme system/light/dark; row_height compact/standard/roomy; "
                                           L"language system/zh-CN/zh-TW/en-US; booleans true/false."));
            json::Value req = json::Value::Array();
            req.push("key");
            req.push("value");
            list.push(ToolDef(
                "set_setting",
                "Changes one Luling setting. The user confirms on a card first. Supported keys: theme, "
                "row_height, language, show_hidden, show_protected, show_performance, list_smart_date, "
                "list_zebra_rows, list_size_bar.",
                std::move(setProps), std::move(req)));
        }
        return list;
    }();
    return tools;
}

} // namespace

const json::Value& Tools() { return BuildTools(); }

std::wstring ToolTitle(const std::string& name) {
    if (name == "get_state") return L"查看当前窗口";
    if (name == "list_folder") return L"读取文件夹";
    if (name == "organize_files") return L"整理文件";
    if (name == "rename_files") return L"重命名文件";
    if (name == "delete_files") return L"删除文件";
    return L"执行工具";
}

bool ToolNeedsUiThread(const std::string& name) { return name == "get_state"; }

bool ResolveFolderPath(const std::wstring& raw, std::wstring& out) {
    std::wstring path = raw;
    while (!path.empty() && (path.back() == L'/' || path.back() == L'\\')) path.pop_back();
    if (path.empty()) return false;
    if (path.size() == 2 && path[1] == L':') {
        out = path + L"\\";
        return true;
    }
    if (KnownFolder(path, out)) return true;
    if (path.find(L":\\") != std::wstring::npos) {
        out = path;
        return true;
    }
    return false;
}

ToolOutcome RunToolOnWorker(const std::string& name, const json::Value& args,
                            const std::atomic<bool>& cancelled) {
    ToolOutcome out;
    if (name != "list_folder") {
        out.code = "unknown_tool";
        out.message = "This version does not have that tool.";
        return out;
    }
    const std::wstring raw = json::Wide(args.get_string("path"));
    if (raw.empty()) {
        out.code = "no_folder";
        out.message = "No folder is open and no path was given.";
        return out;
    }
    std::wstring path;
    if (!ResolveFolderPath(raw, path)) {
        out.code = "bad_path";
        out.message = "That is not an absolute path or a known folder.";
        return out;
    }
    if (fs::IsVirtualPath(path)) {
        out.code = "virtual_path";
        out.message = "That is a search or tag view, not a folder.";
        return out;
    }

    size_t limit = 200;
    {
        int64_t n = 0;
        if (const json::Value* v = args.find("limit"); v && v->get_int(n, 1, static_cast<int64_t>(kMaxListItems)))
            limit = static_cast<size_t>(n);
    }

    std::vector<fs::DirEntry> entries;
    try {
        fs::EnumerationOptions options;
        options.cancelled = [&cancelled] { return cancelled.load(); };
        fs::EnumerateDirectory(path, entries, options);
    } catch (const std::exception& e) {
        out.code = "read_failed";
        out.message = e.what();
        return out;
    }
    if (cancelled.load()) {
        out.code = "cancelled";
        out.message = "Cancelled.";
        return out;
    }
    std::stable_sort(entries.begin(), entries.end(), [](const fs::DirEntry& a, const fs::DirEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return CompareStringOrdinal(a.name.c_str(), static_cast<int>(a.name.size()), b.name.c_str(),
                                    static_cast<int>(b.name.size()), TRUE) == CSTR_LESS_THAN;
    });

    json::Value items = json::Value::Array();
    size_t folders = 0, files = 0, listed = 0;
    uint64_t total = 0;
    for (const fs::DirEntry& e : entries) {
        if (e.is_dir) ++folders;
        else {
            ++files;
            total += e.size;
        }
        if (listed >= limit) continue;
        json::Value item = json::Value::Object();
        item.set("name", json::Utf8(e.name));
        item.set("type", e.is_dir ? "folder" : "file");
        if (!e.is_dir) item.set("size", static_cast<int64_t>(e.size));
        const std::string modified = DateText(e.mtime);
        if (!modified.empty()) item.set("modified", modified);
        items.push(std::move(item));
        ++listed;
    }
    out.data = json::Value::Object();
    out.data.set("path", json::Utf8(path));
    out.data.set("folders", static_cast<int64_t>(folders));
    out.data.set("files", static_cast<int64_t>(files));
    out.data.set("total_size", static_cast<int64_t>(total));
    out.data.set("truncated", listed < entries.size());
    out.data.set("items", std::move(items));
    out.ok = true;
    return out;
}

} // namespace luling::app::ai
