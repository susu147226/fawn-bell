#include "../app/global_search_result_state.h"
#include <iostream>
#include <vector>
struct Row { std::wstring path; };
int main() {
    using namespace pulse::app;
    bool ok = true;
    auto check = [&](bool value, const char* label) { std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= value; };
    for (const bool network_first : {false, true}) {
        const std::wstring chosen = network_first ? L"\\\\server\\share\\same.txt" : L"C:\\local\\same.txt";
        std::vector<Row> first{{L"C:\\a.txt"}, {chosen}, {L"C:\\z.txt"}};
        const auto position = GlobalSearchPosition::Capture(first, 1, 1);
        std::vector<Row> merged{{L"C:\\best-exact.txt"}, {L"D:\\other\\same.txt"}, {L"C:\\a.txt"}, {chosen}, {L"C:\\z.txt"}};
        const auto selected = position.Selection(merged);
        check(selected == 3 && merged[selected].path == chosen && position.First(merged) == 3,
              "provider arrival keeps selected and scroll-anchor paths despite reranking and same names");
        merged.erase(merged.begin() + 3);
        check(position.Selection(merged) == -1, "top-N eviction clears selection instead of opening another path");
        const auto cleared = GlobalSearchPosition::Capture(merged, -1, 0);
        check(cleared.Selection(first) == -1, "late provider cannot silently revive an explicitly cleared selection");
    }
    std::vector<Row> normalized{{L"C:\\Fixture\\A.txt"}};
    check(GlobalSearchPosition::Find(normalized, L"\\\\?\\c:\\fixture\\a.TXT") == 0,
          "selection uses Windows path identity including extended prefix and case");
    for (DWORD error : {DWORD{ERROR_CONNECTION_ABORTED}, DWORD{ERROR_INVALID_DATA}}) {
        for (bool network_first : {false, true}) {
            GlobalProviderStatus state;
            if (network_first) { state.Accept(true, error); state.Accept(false, 0); }
            else { state.Accept(false, 0); state.Accept(true, error); }
            check(state.Error() == error && !state.Busy(), "provider failure remains visible after either completion order");
            state.Accept(true, 0);
            check(!state.Error() && !state.Busy(), "only successful retry of failed provider clears its failure");
        }
    }
    GlobalProviderStatus timeout; timeout.Timeout(); timeout.Accept(false, 0);
    check(timeout.Error() == ERROR_TIMEOUT && !timeout.Busy(), "late local success preserves pending network timeout");
    timeout.Accept(true, 0);
    check(!timeout.Error() && !timeout.Busy(), "late network success resolves its own timeout");
    GlobalProviderStatus empty; empty.Accept(false, 0); empty.Accept(true, 0);
    check(!empty.Error() && !empty.Busy(), "two successful empty providers remain a true empty result");
    return ok ? 0 : 1;
}
