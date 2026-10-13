#pragma once
namespace luling {
struct AppState;
void ApplyGlobalSearchSettings(AppState& s);
void ToggleGlobalSearch(AppState& s);
void ShutdownGlobalSearch(AppState& s);
bool HandleGlobalSearchHotkeyCapture(AppState& s, unsigned key);
}
