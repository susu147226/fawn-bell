// asset_refile.h — refile.py built into the menu.
//
// For every selected .png: make a folder named after the file's prefix, move
// the png into it, copy the folder's .xml files there with screenWidth /
// screenHeight and the Image src rewritten from the png, then remove the
// originals from the parent folder.
#pragma once
#include <string>
#include <vector>

namespace luling {

struct AppState;

// Returns the number of PNG files that were filed.
int RefileAssets(AppState& s, const std::vector<std::wstring>& paths);

} // namespace luling
