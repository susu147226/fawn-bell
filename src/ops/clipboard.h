// clipboard.h — System clipboard interop with Explorer (CF_HDROP + drop effect).
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace luling::ops {

struct ClipboardData {
    std::vector<std::wstring> paths;
    bool cut = false; // CFSTR_PREFERREDDROPEFFECT == DROPEFFECT_MOVE
    uint32_t sequence = 0;
};

// CFSTR_PREFERREDDROPEFFECT -> "is this a cut?". Only an exact MOVE counts:
// many applications publish COPY|MOVE (3) to mean "either is fine", and a
// bitwise test reads that as a cut, so pasting would move - and delete - the
// user's originals instead of copying them.
bool PreferredEffectIsCut(uint32_t preferred_effect);

// Writes paths as CF_HDROP + CFSTR_PREFERREDDROPEFFECT so Explorer can paste them.
// Must be called on the UI thread (OpenClipboard affinity).
bool WriteClipboard(const std::vector<std::wstring>& paths, bool cut);

// Writes plain text (Ctrl+Shift+C "copy path"). UI thread only.
bool WriteClipboardText(const std::wstring& text);

// Reads an Explorer copy/cut (CF_HDROP). Returns false if no files present.
bool ReadClipboard(ClipboardData& out);

// Virtual files (an archive's contents, a phone's items) that Explorer can
// paste but ReadClipboard cannot list as paths.
inline bool ClipboardHasShellFiles() {
    return IsClipboardFormatAvailable(RegisterClipboardFormatW(L"Shell IDList Array")) != FALSE ||
        IsClipboardFormatAvailable(RegisterClipboardFormatW(L"FileGroupDescriptorW")) != FALSE;
}

// Anything the Paste command can act on.
inline bool ClipboardHasPasteableFiles() {
    return IsClipboardFormatAvailable(CF_HDROP) || ClipboardHasShellFiles();
}

// Clears a completed cut only if the clipboard still contains the exact cut
// captured by ReadClipboard. This never clears a newer clipboard value.
bool CompleteCutClipboard(uint32_t sequence, const std::vector<std::wstring>& expected_paths);

} // namespace luling::ops
