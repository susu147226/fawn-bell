#pragma once

// Line + character diff for the staging-tray text compare window.
// Pure computation (no UI); LoadDiffSide touches the file system.

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::diff {

enum class LoadStatus : uint8_t { Ok, Binary, TooLarge, ReadFailed };

struct DiffSide {
    std::wstring path;
    std::vector<std::wstring> lines; // tabs expanded to 4 columns, no EOL
    std::wstring encoding;           // "UTF-8", "UTF-8 BOM", "UTF-16 LE", "GBK", ...
    std::wstring eol;                // "CRLF", "LF", "CR", "Mixed" or empty (single line)
    LoadStatus status = LoadStatus::ReadFailed;
};

inline constexpr uint64_t kMaxDiffFileBytes = 8ull * 1024 * 1024;

// Reads, decodes and splits one file. Never throws.
void LoadDiffSide(const std::wstring& path, DiffSide& out);

// First 8 KB probe used by the tray: true when the file looks like text.
bool ProbeLooksText(const std::wstring& path);

enum class RowKind : uint8_t { Same, Mod, Del, Add };

struct DiffRow {
    RowKind kind = RowKind::Same;
    int left = -1;  // line index on the left, -1 when the row only exists on the right
    int right = -1; // line index on the right, -1 when the row only exists on the left
};

struct DiffHunk {
    int first = 0; // first row index
    int last = 0;  // last row index (inclusive)
};

struct DiffOptions {
    bool ignore_whitespace = false;
    bool ignore_case = false;
};

struct DiffResult {
    std::vector<DiffRow> rows;
    std::vector<DiffHunk> hunks;
    int added = 0;
    int deleted = 0;
    int modified = 0;
    bool capped = false; // edit distance budget exceeded; middle shown as blocks
};

// Returns false when cancelled.
bool ComputeLineDiff(const std::vector<std::wstring>& left,
                     const std::vector<std::wstring>& right, const DiffOptions& options,
                     DiffResult& out, const std::atomic<bool>* cancel = nullptr);

struct CharSpan {
    int begin = 0;
    int end = 0; // exclusive
};

// Character ranges that differ inside a modified row.
void ComputeCharDiff(std::wstring_view left, std::wstring_view right, const DiffOptions& options,
                     std::vector<CharSpan>& left_spans, std::vector<CharSpan>& right_spans);

} // namespace pulse::diff
