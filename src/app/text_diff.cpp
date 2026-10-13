#include "text_diff.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <unordered_map>

#include "../common/text_decode.h"

namespace pulse::diff {
namespace {

constexpr size_t kProbeBytes = 8192;
constexpr int kTabWidth = 4;

std::wstring EncodingLabel(const std::vector<uint8_t>& bytes) {
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) return L"UTF-16 LE";
    if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) return L"UTF-16 BE";
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        return L"UTF-8 BOM";
    if (bytes.empty()) return L"UTF-8";
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          reinterpret_cast<const char*>(bytes.data()),
                                          static_cast<int>(bytes.size()), nullptr, 0);
    if (chars > 0) return L"UTF-8";
    const UINT acp = GetACP();
    if (acp == 936) return L"GBK";
    return L"ANSI " + std::to_wstring(acp);
}

void SplitLines(const std::wstring& text, DiffSide& out) {
    out.lines.clear();
    size_t crlf = 0, lf = 0, cr = 0;
    std::wstring line;
    int column = 0;
    bool pending = false; // characters seen since the last line break
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\r' || ch == L'\n') {
            if (ch == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') {
                ++crlf;
                ++i;
            } else if (ch == L'\r') {
                ++cr;
            } else {
                ++lf;
            }
            out.lines.push_back(std::move(line));
            line.clear();
            column = 0;
            pending = false;
            continue;
        }
        pending = true;
        if (ch == L'\t') {
            const int pad = kTabWidth - (column % kTabWidth);
            line.append(static_cast<size_t>(pad), L' ');
            column += pad;
        } else {
            line.push_back(ch);
            ++column;
        }
    }
    if (pending) out.lines.push_back(std::move(line));
    const int kinds = (crlf ? 1 : 0) + (lf ? 1 : 0) + (cr ? 1 : 0);
    if (kinds > 1) out.eol = L"Mixed";
    else if (crlf) out.eol = L"CRLF";
    else if (lf) out.eol = L"LF";
    else if (cr) out.eol = L"CR";
    else out.eol.clear();
}

bool ReadAllBytes(const std::wstring& path, uint64_t limit, std::vector<uint8_t>& bytes,
                  bool& too_large) {
    too_large = false;
    bytes.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        return false;
    }
    if (static_cast<uint64_t>(size.QuadPart) > limit) {
        too_large = true;
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>((std::min)(bytes.size() - done, size_t{1} << 20));
        if (!::ReadFile(file, bytes.data() + done, want, &got, nullptr)) {
            CloseHandle(file);
            return false;
        }
        if (got == 0) break;
        done += got;
    }
    bytes.resize(done);
    CloseHandle(file);
    return true;
}

std::wstring NormalizeKey(const std::wstring& line, const DiffOptions& options) {
    if (!options.ignore_whitespace && !options.ignore_case) return line;
    std::wstring key;
    key.reserve(line.size());
    for (const wchar_t ch : line) {
        if (options.ignore_whitespace && std::iswspace(ch)) continue;
        key.push_back(options.ignore_case ? static_cast<wchar_t>(std::towlower(ch)) : ch);
    }
    return key;
}

enum class Op : uint8_t { Equal, Del, Ins };
struct EditOp {
    Op op;
    int a;
    int b;
};

// Myers O(ND) on the trimmed middle. Returns false when the budget is exceeded or cancelled
// (cancelled is reported separately).
bool Myers(const int* a, int n, const int* b, int m, int budget, std::vector<EditOp>& ops,
           int a_base, int b_base, const std::atomic<bool>* cancel, bool& cancelled) {
    cancelled = false;
    const int max = n + m;
    if (max == 0) return true;
    const int offset = max + 1;
    std::vector<int> v(static_cast<size_t>(2 * max + 3), 0);
    std::vector<std::vector<int>> trace;
    int found = -1;
    const int limit = (std::min)(max, budget);
    for (int d = 0; d <= limit && found < 0; ++d) {
        if (cancel && (d & 31) == 0 && cancel->load(std::memory_order_relaxed)) {
            cancelled = true;
            return false;
        }
        trace.emplace_back(v.begin() + (offset - d), v.begin() + (offset + d + 1));
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1]))
                x = v[offset + k + 1];
            else
                x = v[offset + k - 1] + 1;
            int y = x - k;
            while (x < n && y < m && a[x] == b[y]) {
                ++x;
                ++y;
            }
            v[offset + k] = x;
            if (x >= n && y >= m) {
                found = d;
                break;
            }
        }
    }
    if (found < 0) return false;
    std::vector<EditOp> reversed;
    int x = n, y = m;
    for (int d = found; d > 0; --d) {
        const std::vector<int>& vd = trace[static_cast<size_t>(d)];
        auto at = [&](int k) { return vd[static_cast<size_t>(k + d)]; };
        const int k = x - y;
        const int prev_k = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? k + 1 : k - 1;
        const int prev_x = at(prev_k);
        const int prev_y = prev_x - prev_k;
        while (x > prev_x && y > prev_y) {
            --x;
            --y;
            reversed.push_back({Op::Equal, a_base + x, b_base + y});
        }
        if (prev_k == k + 1)
            reversed.push_back({Op::Ins, -1, b_base + prev_y});
        else
            reversed.push_back({Op::Del, a_base + prev_x, -1});
        x = prev_x;
        y = prev_y;
    }
    while (x > 0 && y > 0) {
        --x;
        --y;
        reversed.push_back({Op::Equal, a_base + x, b_base + y});
    }
    ops.insert(ops.end(), reversed.rbegin(), reversed.rend());
    return true;
}

void MergeSpans(std::vector<CharSpan>& spans, int gap) {
    if (spans.empty()) return;
    std::vector<CharSpan> merged;
    merged.push_back(spans.front());
    for (size_t i = 1; i < spans.size(); ++i) {
        if (spans[i].begin - merged.back().end <= gap)
            merged.back().end = spans[i].end;
        else
            merged.push_back(spans[i]);
    }
    spans.swap(merged);
}

void DropWhitespaceSpans(std::wstring_view text, std::vector<CharSpan>& spans) {
    spans.erase(std::remove_if(spans.begin(), spans.end(), [&](const CharSpan& s) {
        for (int i = s.begin; i < s.end; ++i)
            if (!std::iswspace(text[static_cast<size_t>(i)])) return false;
        return true;
    }), spans.end());
}

} // namespace

void LoadDiffSide(const std::wstring& path, DiffSide& out) {
    out.path = path;
    out.lines.clear();
    out.encoding.clear();
    out.eol.clear();
    std::vector<uint8_t> bytes;
    bool too_large = false;
    if (!ReadAllBytes(path, kMaxDiffFileBytes, bytes, too_large)) {
        out.status = too_large ? LoadStatus::TooLarge : LoadStatus::ReadFailed;
        return;
    }
    {
        std::vector<uint8_t> head(bytes.begin(),
                                  bytes.begin() + static_cast<std::ptrdiff_t>(
                                      (std::min)(bytes.size(), kProbeBytes)));
        if (text::LooksBinary(head)) {
            out.status = LoadStatus::Binary;
            return;
        }
    }
    out.encoding = EncodingLabel(bytes);
    std::wstring decoded;
    if (!text::Decode(bytes, decoded, text::Encoding::Auto)) {
        out.status = LoadStatus::ReadFailed;
        return;
    }
    SplitLines(decoded, out);
    out.status = LoadStatus::Ok;
}

bool ProbeLooksText(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> head(kProbeBytes);
    DWORD got = 0;
    const BOOL ok = ::ReadFile(file, head.data(), static_cast<DWORD>(head.size()), &got, nullptr);
    CloseHandle(file);
    if (!ok) return false;
    head.resize(got);
    return !text::LooksBinary(head);
}

bool ComputeLineDiff(const std::vector<std::wstring>& left,
                     const std::vector<std::wstring>& right, const DiffOptions& options,
                     DiffResult& out, const std::atomic<bool>* cancel) {
    out = DiffResult{};
    std::unordered_map<std::wstring, int> ids;
    ids.reserve(left.size() + right.size());
    auto intern = [&](const std::vector<std::wstring>& lines, std::vector<int>& seq) {
        seq.reserve(lines.size());
        for (const auto& line : lines) {
            auto [it, inserted] = ids.try_emplace(NormalizeKey(line, options),
                                                  static_cast<int>(ids.size()));
            (void)inserted;
            seq.push_back(it->second);
        }
    };
    std::vector<int> a, b;
    intern(left, a);
    intern(right, b);
    if (cancel && cancel->load()) return false;

    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    int prefix = 0;
    while (prefix < n && prefix < m && a[static_cast<size_t>(prefix)] == b[static_cast<size_t>(prefix)])
        ++prefix;
    int suffix = 0;
    while (suffix < n - prefix && suffix < m - prefix &&
           a[static_cast<size_t>(n - 1 - suffix)] == b[static_cast<size_t>(m - 1 - suffix)])
        ++suffix;

    std::vector<EditOp> ops;
    ops.reserve(static_cast<size_t>((std::max)(n, m)) + 16);
    for (int i = 0; i < prefix; ++i) ops.push_back({Op::Equal, i, i});
    const int mid_n = n - prefix - suffix;
    const int mid_m = m - prefix - suffix;
    const int total = mid_n + mid_m;
    const int budget = std::clamp(40'000'000 / (total + 1), 200, 2000);
    bool cancelled = false;
    if (!Myers(a.data() + prefix, mid_n, b.data() + prefix, mid_m, budget, ops, prefix, prefix,
               cancel, cancelled)) {
        if (cancelled) return false;
        out.capped = true;
        for (int i = 0; i < mid_n; ++i) ops.push_back({Op::Del, prefix + i, -1});
        for (int j = 0; j < mid_m; ++j) ops.push_back({Op::Ins, -1, prefix + j});
    }
    for (int i = 0; i < suffix; ++i) ops.push_back({Op::Equal, n - suffix + i, m - suffix + i});

    std::vector<int> dels, ins;
    auto flush = [&] {
        const size_t pairs = (std::min)(dels.size(), ins.size());
        for (size_t i = 0; i < pairs; ++i) {
            out.rows.push_back({RowKind::Mod, dels[i], ins[i]});
            ++out.modified;
        }
        for (size_t i = pairs; i < dels.size(); ++i) {
            out.rows.push_back({RowKind::Del, dels[i], -1});
            ++out.deleted;
        }
        for (size_t i = pairs; i < ins.size(); ++i) {
            out.rows.push_back({RowKind::Add, -1, ins[i]});
            ++out.added;
        }
        dels.clear();
        ins.clear();
    };
    out.rows.reserve(ops.size());
    for (const EditOp& op : ops) {
        if (op.op == Op::Equal) {
            flush();
            out.rows.push_back({RowKind::Same, op.a, op.b});
        } else if (op.op == Op::Del) {
            dels.push_back(op.a);
        } else {
            ins.push_back(op.b);
        }
    }
    flush();

    for (int r = 0; r < static_cast<int>(out.rows.size()); ++r) {
        if (out.rows[static_cast<size_t>(r)].kind == RowKind::Same) continue;
        if (!out.hunks.empty() && out.hunks.back().last == r - 1)
            out.hunks.back().last = r;
        else
            out.hunks.push_back({r, r});
    }
    return true;
}

void ComputeCharDiff(std::wstring_view left, std::wstring_view right, const DiffOptions& options,
                     std::vector<CharSpan>& left_spans, std::vector<CharSpan>& right_spans) {
    left_spans.clear();
    right_spans.clear();
    auto fold = [&](wchar_t ch) {
        return options.ignore_case ? static_cast<wchar_t>(std::towlower(ch)) : ch;
    };
    const int n = static_cast<int>(left.size());
    const int m = static_cast<int>(right.size());
    int prefix = 0;
    while (prefix < n && prefix < m &&
           fold(left[static_cast<size_t>(prefix)]) == fold(right[static_cast<size_t>(prefix)]))
        ++prefix;
    int suffix = 0;
    while (suffix < n - prefix && suffix < m - prefix &&
           fold(left[static_cast<size_t>(n - 1 - suffix)]) ==
               fold(right[static_cast<size_t>(m - 1 - suffix)]))
        ++suffix;
    const int a = n - prefix - suffix;
    const int b = m - prefix - suffix;
    auto whole = [&] {
        if (a > 0) left_spans.push_back({prefix, prefix + a});
        if (b > 0) right_spans.push_back({prefix, prefix + b});
    };
    if (a == 0 || b == 0 || static_cast<int64_t>(a) * b > 250'000) {
        whole();
    } else {
        // Suffix LCS table: len[i][j] = LCS(A[i..], B[j..]). min(a, b) <= 500 so uint16 fits.
        const size_t stride = static_cast<size_t>(b) + 1;
        std::vector<uint16_t> len((static_cast<size_t>(a) + 1) * stride, 0);
        for (int i = a - 1; i >= 0; --i) {
            for (int j = b - 1; j >= 0; --j) {
                uint16_t& cell = len[static_cast<size_t>(i) * stride + static_cast<size_t>(j)];
                if (fold(left[static_cast<size_t>(prefix + i)]) ==
                    fold(right[static_cast<size_t>(prefix + j)]))
                    cell = static_cast<uint16_t>(
                        len[static_cast<size_t>(i + 1) * stride + static_cast<size_t>(j + 1)] + 1);
                else
                    cell = (std::max)(
                        len[static_cast<size_t>(i + 1) * stride + static_cast<size_t>(j)],
                        len[static_cast<size_t>(i) * stride + static_cast<size_t>(j + 1)]);
            }
        }
        const int common = len[0];
        if (common * 10 < (std::max)(a, b) * 3) {
            whole(); // mostly rewritten: one block reads better than confetti
        } else {
            auto mark = [](std::vector<CharSpan>& spans, int pos) {
                if (!spans.empty() && spans.back().end == pos)
                    spans.back().end = pos + 1;
                else
                    spans.push_back({pos, pos + 1});
            };
            int i = 0, j = 0;
            while (i < a && j < b) {
                if (fold(left[static_cast<size_t>(prefix + i)]) ==
                    fold(right[static_cast<size_t>(prefix + j)])) {
                    ++i;
                    ++j;
                } else if (len[static_cast<size_t>(i + 1) * stride + static_cast<size_t>(j)] >=
                           len[static_cast<size_t>(i) * stride + static_cast<size_t>(j + 1)]) {
                    mark(left_spans, prefix + i);
                    ++i;
                } else {
                    mark(right_spans, prefix + j);
                    ++j;
                }
            }
            while (i < a) mark(left_spans, prefix + i++);
            while (j < b) mark(right_spans, prefix + j++);
            MergeSpans(left_spans, 2);
            MergeSpans(right_spans, 2);
        }
    }
    if (options.ignore_whitespace) {
        DropWhitespaceSpans(left, left_spans);
        DropWhitespaceSpans(right, right_spans);
    }
}

} // namespace pulse::diff
