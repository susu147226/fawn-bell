#include "plugin_manifest.h"

#include "../common/localization.h"

namespace luling::plugin {

std::vector<std::wstring> CapabilityLines(const std::vector<std::wstring>& capabilities) {
    std::vector<std::wstring> lines;
    for (const std::wstring& capability : capabilities) {
        if (capability == L"ui.command")
            lines.push_back(l10n::Pick(L"在右键菜单和命令面板中提供命令",
                                       L"Add commands to the context menu and the command palette"));
        else if (capability == L"ui.preview")
            lines.push_back(l10n::Pick(L"为选定类型的文件绘制详情预览",
                                       L"Draw details-pane previews for chosen file types"));
        else if (capability == L"ui.column")
            lines.push_back(l10n::Pick(L"在文件列表中添加一列",
                                       L"Add a column to the file list"));
        else
            lines.push_back(capability);
    }
    return lines;
}

} // namespace luling::plugin
