// cloud_status.h — the sidebar's cloud row status, read from the synced
// folder itself. OneDrive is the first consumer; any placeholder-style sync
// folder (Files On-Demand) reports the same way.
#pragma once

#include <string>

namespace luling::app {
// "已同步" / "按需下载" / "位置不可用", or empty when there is nothing to say.
// Samples the folder's direct children; results are cached briefly so a
// sidebar refresh does not stat the account root every frame.
std::wstring CloudFolderSyncStatus(const std::wstring& root);
}
