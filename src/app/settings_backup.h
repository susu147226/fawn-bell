// settings_backup.h - 备份与迁移: write app.json, ai.json and
// context_menu.json into one file, and put them back after keeping the
// current copies as .bak-<time>.
#pragma once

namespace luling {
struct AppState;

void ExportSettingsBackup(AppState& s);
void ImportSettingsBackup(AppState& s);

} // namespace luling
