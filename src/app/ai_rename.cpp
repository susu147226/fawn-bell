// ai_rename.cpp - rename_files: the rule, the preview, nothing written.
#include "ai_rename.h"

#include "batch_rename.h"

#include <windows.h>

#include <string>
#include <vector>

namespace luling::app::ai {
namespace {

std::wstring BaseName(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? path : path.substr(cut + 1);
}

std::wstring ParentOf(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
}

// "{n}" becomes "{n:2}" when the caller asked for padded numbers.
std::wstring WithDigits(std::wstring pattern, int digits) {
    if (digits <= 0) return pattern;
    const std::wstring pad = std::to_wstring(digits);
    std::wstring out;
    out.reserve(pattern.size() + 8);
    for (size_t i = 0; i < pattern.size();) {
        if (pattern.compare(i, 3, L"{n}") == 0) {
            out += L"{n:" + pad + L"}";
            i += 3;
            continue;
        }
        out.push_back(pattern[i++]);
    }
    return out;
}

} // namespace

bool ParseRenamePlan(const json::Value& args, const std::vector<std::wstring>& selection,
                     RenamePlan& plan, std::string& code, std::string& message) {
    std::vector<std::wstring> paths;
    if (const json::Value* given = args.find("paths"); given && given->is_array()) {
        for (const json::Value& v : given->items()) {
            const std::wstring path = json::Wide(v.as_string());
            if (!path.empty()) paths.push_back(path);
        }
    }
    if (paths.empty()) paths = selection;
    if (paths.empty()) {
        code = "no_files";
        message = "No files were given and nothing is selected in the window.";
        return false;
    }
    if (paths.size() > 500) {
        code = "too_many_files";
        message = "At most 500 files in one rename.";
        return false;
    }

    BatchRenameRule rule;
    bool has_rule = false;
    const json::Value* steps = args.find("steps");
    if (!steps || !steps->is_array() || steps->size() == 0) {
        code = "no_rule";
        message = "steps must hold at least one rule step.";
        return false;
    }
    for (const json::Value& step : steps->items()) {
        const std::string type = step.get_string("type");
        if (type == "replace") {
            rule.find = json::Wide(step.get_string("find"));
            rule.replace = json::Wide(step.get_string("with"));
            if (rule.find.empty()) {
                code = "bad_step";
                message = "A replace step needs find.";
                return false;
            }
            has_rule = true;
        } else if (type == "template") {
            rule.pattern = json::Wide(step.get_string("pattern"));
            if (rule.pattern.empty()) {
                code = "bad_step";
                message = "A template step needs pattern.";
                return false;
            }
            int64_t n = 0;
            if (const json::Value* v = step.find("start"); v && v->get_int(n, 0, 1000000))
                rule.start = static_cast<int>(n);
            int64_t d = 0;
            if (const json::Value* v = step.find("digits"); v && v->get_int(d, 0, 9))
                rule.digits = static_cast<int>(d);
            rule.pattern = WithDigits(rule.pattern, rule.digits);
            has_rule = true;
        } else {
            code = "unsupported_step";
            message = "This version renames with replace and template steps only; got '" + type + "'.";
            return false;
        }
    }
    if (!has_rule) {
        code = "no_rule";
        message = "steps must hold at least one rule step.";
        return false;
    }

    const std::vector<BatchRenameItem> items = PreviewBatchRename(paths, rule);
    // A pattern replaces the whole name; the extension stays unless it names
    // {ext} itself (the rule's own {name} already leaves it out).
    const bool keep_ext = !rule.pattern.empty() && rule.pattern.find(L"{ext}") == std::wstring::npos;
    for (const BatchRenameItem& item : items) {
        if (item.status == BatchRenameStatus::Ok && item.new_name != item.original_name) {
            std::wstring new_name = item.new_name;
            if (keep_ext) {
                std::wstring stem, ext;
                SplitFileName(item.original_name, stem, ext);
                new_name += ext;
            }
            plan.old_paths.push_back(item.source_path);
            plan.new_names.push_back(new_name);
            plan.rows.emplace_back(item.original_name, new_name);
            continue;
        }
        ++plan.skipped;
        if (item.status == BatchRenameStatus::Collision) ++plan.clashed;
    }
    if (plan.old_paths.empty()) {
        code = "nothing_to_rename";
        message = "With this rule every name stays as it is; change the rule.";
        return false;
    }
    plan.folder = ParentOf(plan.old_paths.front());
    plan.label = BaseName(plan.folder);
    plan.rule = rule.pattern.empty() ? (rule.find + L" → " + rule.replace) : rule.pattern;
    return true;
}

} // namespace luling::app::ai
