#pragma once
#include <string>

namespace luling::index {
// Called by the elevated configuration helper, never by the UI thread.
int ConfigureServiceIndexPath(const std::wstring& path);
}
