#pragma once
#include <string>

namespace luling::index {

// Runs the per-user UNC indexing mode of Luling.Index.exe. This must remain a
// separate process from the SYSTEM service so SMB uses the interactive user's
// credentials, but it shares the same executable and release artifact.
int RunNetworkAgent();
int RunNetworkAgentTest(const std::wstring& token);

} // namespace luling::index
