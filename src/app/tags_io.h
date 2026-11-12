// tags_io.h — import / export the tag catalog (names, colors, groups, paths).
#pragma once
#include <string>

namespace luling::app { class PlacesCatalog; }

namespace luling {

// Writes every tag as one JSON document. False when the file cannot be written.
bool ExportTagsFile(const app::PlacesCatalog& places, const std::wstring& path);

// Merges a previously exported document back in: tags match by name, and their
// paths are added to the existing tag. Returns how many tags were applied.
int ImportTagsFile(app::PlacesCatalog& places, const std::wstring& path, std::wstring& error);

} // namespace luling
