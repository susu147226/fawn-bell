// tags_io.cpp — tag catalog import / export (tags_io.h).
#include "tags_io.h"

#include "places.h"
#include "../common/json_value.h"

#include <cstdio>

namespace luling {
namespace {

bool ReadFile(const std::wstring& path, std::string& out) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return false;
    char buffer[16384];
    size_t n = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, n);
    fclose(file);
    return true;
}

bool WriteFile(const std::wstring& path, const std::string& text) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
    fclose(file);
    return ok;
}

bool EqualNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

} // namespace

bool ExportTagsFile(const app::PlacesCatalog& places, const std::wstring& path) {
    json::Value root = json::Value::Object();
    root.set("version", json::Value(1));
    json::Value list = json::Value::Array();
    for (const app::ColorTag& tag : places.tags) {
        json::Value item = json::Value::Object();
        item.set("id", json::Value(json::Utf8(tag.id)));
        item.set("name", json::Value(json::Utf8(tag.name)));
        item.set("rgb", json::Value(static_cast<int64_t>(tag.rgb & 0xFFFFFFu)));
        item.set("group", json::Value(json::Utf8(tag.group)));
        json::Value paths = json::Value::Array();
        for (const std::wstring& entry : tag.paths)
            paths.push(json::Value(json::Utf8(entry)));
        item.set("paths", std::move(paths));
        list.push(std::move(item));
    }
    root.set("tags", std::move(list));
    return WriteFile(path, json::Serialize(root));
}

int ImportTagsFile(app::PlacesCatalog& places, const std::wstring& path, std::wstring& error) {
    std::string body;
    if (!ReadFile(path, body)) {
        error = L"无法读取文件";
        return 0;
    }
    json::Value root;
    if (!json::Parse(body, root, nullptr) || !root.is_object()) {
        error = L"不是有效的标签文件";
        return 0;
    }
    const json::Value* list = root.find("tags");
    if (!list || !list->is_array()) {
        error = L"标签文件里没有标签";
        return 0;
    }
    int applied = 0;
    for (const json::Value& item : list->items()) {
        if (!item.is_object()) continue;
        const std::wstring name = json::Wide(item.get_string("name"));
        if (name.empty()) continue;
        const uint32_t rgb = static_cast<uint32_t>(
            static_cast<int64_t>(item.as_number(0)));
        std::vector<std::wstring> paths;
        if (const json::Value* entries = item.find("paths"); entries && entries->is_array()) {
            for (const json::Value& entry : entries->items())
                if (entry.is_string() && !entry.as_string().empty())
                    paths.push_back(json::Wide(entry.as_string()));
        }
        app::TagId id;
        for (const app::ColorTag& tag : places.tags) {
            if (!EqualNoCase(tag.name, name)) continue;
            id = tag.id;
            break;
        }
        if (id.empty()) id = places.CreateTag(name, rgb);
        if (id.empty()) continue;
        if (!paths.empty()) places.SetTagsBatch(id, paths, true);
        const std::wstring group = json::Wide(item.get_string("group"));
        if (!group.empty()) places.SetTagGroup(id, group);
        ++applied;
    }
    return applied;
}

} // namespace luling
