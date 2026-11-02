// ai_delete.h - delete_files: move items to the Recycle Bin (undoable), or
// delete them for good when the user asks for exactly that.
#pragma once

#include "../common/json_value.h"

#include <string>
#include <vector>

namespace luling::app::ai {

struct DeletePlan {
    std::wstring folder, label;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> names;
    bool permanent = false;
};

// Reads {paths?, permanent?} (selection when paths are left out) and checks
// every item: it exists, it is not a drive root, and a permanent delete never
// names Windows or Program Files. Nothing is touched here.
bool ParseDeletePlan(const json::Value& args, const std::vector<std::wstring>& selection,
                     DeletePlan& plan, std::string& code, std::string& message);

} // namespace luling::app::ai
