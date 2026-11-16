#include "store_trust_ui.h"

#include "store_trust.h"
#include "../common/localization.h"

namespace luling::app {

bool ConfirmStorePublisherTrust(HWND owner, const StorePublisher& publisher) {
    if (IsPublisherTrusted(publisher.id)) return true;
    const std::wstring fingerprint = publisher.id.substr(0, 16);
    std::wstring text = l10n::Pick(L"发布者：", L"Publisher: ");
    text += publisher.name.empty() ? l10n::Pick(L"（未命名）", L"(unnamed)") : publisher.name;
    if (publisher.official) text += l10n::Pick(L"（官方）", L" (official)");
    text += L"\n";
    text += l10n::Pick(L"指纹：", L"Fingerprint: ");
    text += fingerprint;
    text += l10n::Pick(L"…\n\n安装后该发布者的扩展能够运行代码。只有在你信任这个发布者时才继续。",
                       L"…\n\nExtensions from this publisher can run code once installed. "
                       L"Continue only if you trust this publisher.");
    const int answer = MessageBoxW(
        owner, text.c_str(),
        l10n::Pick(L"信任此发布者？", L"Trust this publisher?"),
        MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    if (answer != IDYES) return false;
    std::wstring error;
    if (!TrustPublisher({publisher.id, publisher.name, publisher.public_key}, error)) {
        MessageBoxW(owner, error.c_str(),
                    l10n::Pick(L"无法记录信任", L"Could not record trust"),
                    MB_ICONERROR | MB_OK);
        return false;
    }
    return true;
}

} // namespace luling::app
