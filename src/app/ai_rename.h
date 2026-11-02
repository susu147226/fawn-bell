// ai_rename.h - rename_files: turn the model's rule into new names on this
// PC, show them, and let the user confirm before anything changes.
#pragma once

#include "../common/json_value.h"

#include <string>
#include <utility>
#include <vector>

namespace luling::app::ai {

struct RenamePlan {
    std::wstring folder;    // the folder most files sit in (the card's label)
    std::wstring label;
    std::wstring rule;      // the pattern as the card shows it
    std::vector<std::wstring> old_paths;
    std::vector<std::wstring> new_names;   // parallel to old_paths
    std::vector<std::pair<std::wstring, std::wstring>> rows;   // old name, new name
    size_t skipped = 0;     // unchanged, invalid or clashing
    size_t clashed = 0;
};

// Reads {paths?, steps:[...]} with the rule steps this version understands
// (replace, template) and previews the names for `selection` when paths are
// left out. Nothing is written.
bool ParseRenamePlan(const json::Value& args, const std::vector<std::wstring>& selection,
                     RenamePlan& plan, std::string& code, std::string& message);

} // namespace luling::app::ai
