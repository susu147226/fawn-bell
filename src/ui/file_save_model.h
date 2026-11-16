#pragma once

// Pure save-target resolution for the picker's Save mode: filename rules,
// extension completion and overwrite detection. No windows, no file creation.

#include "folder_picker_model.h"

#include <windows.h>

#include <string>
#include <vector>

namespace luling::ui {

enum class SaveTargetState {
    Resolved, Ready, ConfirmOverwrite, InvalidName, InvalidDirectory, InvalidFilter,
    Unsupported, ParentMissing, ParentNotDirectory, TargetDirectory, IoError
};

struct SaveTargetRequest {
    std::wstring directory;         // absolute directory
    std::wstring filename;          // leaf name typed in the picker
    std::vector<PickerFilter> filters;
    size_t filter_index = 0;
    std::wstring default_extension; // without the dot
    bool filter_changed = false;    // user changed the type after opening
};

struct SaveTargetResult {
    SaveTargetState state = SaveTargetState::InvalidName;
    std::wstring path;
    size_t filter_index = 0;
    DWORD error = ERROR_SUCCESS;
    bool target_exists = false;
    bool read_only = false;
};

// Resolves the typed leaf against the directory; no filesystem access.
SaveTargetResult ResolveSaveTarget(const SaveTargetRequest& request);

// Adds attribute reads (never creates or truncates) to tell Ready from
// ConfirmOverwrite. Run off the UI thread.
SaveTargetResult ValidateSaveTarget(const SaveTargetRequest& request);

} // namespace luling::ui
