// temp_files.h — writing and cleaning up the scratch files a preview creates.
//
// The rule this exists to enforce: code that runs "just to show a preview" must
// never be able to delete a user's file. Even if a path is computed wrong — as
// happened once, when a preview helper derived its scratch path from the source
// file and then deleted it — DeleteTempFile refuses anything outside the
// temporary directory, so the mistake cannot reach user data.
#pragma once
#include <windows.h>
#include <string>

namespace luling::common {

// %TEMP%, with its trailing separator. Empty if it cannot be read.
const std::wstring& TempDirectory();

// A unique path under %TEMP% for this process: "<prefix>-<pid>-<counter><ext>".
std::wstring MakeTempPath(const std::wstring& prefix, const std::wstring& extension);

// Deletes `path` only when it really lives in the temporary directory. Returns
// true when the file was removed. Anything outside %TEMP% is refused, whatever
// the caller believed it was deleting.
bool DeleteTempFile(const std::wstring& path);

} // namespace luling::common
