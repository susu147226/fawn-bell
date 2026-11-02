// ai_write.h - The file-changing tool this version ships: organize_files
// (move a folder's items into subfolders). The model plans it, the user
// confirms it on a card, and the moves can be undone.
#pragma once

#include "../common/json_value.h"

#include <string>
#include <vector>

namespace luling::app::ai {

struct OrganizeGroup {
    std::wstring name;
    std::vector<std::wstring> items;
    bool exists = false;   // the subfolder is already there
};

struct OrganizePlan {
    std::wstring folder;   // absolute source folder
    std::wstring label;    // its display name
    std::vector<OrganizeGroup> groups;
    size_t total = 0;
    size_t created = 0;    // groups whose folder must be made
};

// Reads {folder, groups:[{name, items[]}]} and checks it against the disk:
// the folder is a real folder, every item is directly inside it, and no name
// is a path. On failure `code`/`message` say why in the model's terms.
bool ParseOrganizePlan(const json::Value& args, OrganizePlan& plan, std::string& code,
                       std::string& message);

} // namespace luling::app::ai
