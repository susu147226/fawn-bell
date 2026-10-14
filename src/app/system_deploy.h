// system_deploy.h — install/uninstall done by the running program.
//
// The bundled installer cannot write anything on some machines (creating even
// %LOCALAPPDATA%\Programs\Luling is refused, while the same process tree can
// write freely from here), so deployment is done in-process instead: per-user
// shortcuts plus a per-user uninstall entry, no elevation, no installer.
#pragma once
#include <string>

namespace luling {

// Creates the Start-menu and desktop shortcuts and the per-user uninstall
// entry. False with `error` set when a step fails.
bool DeployToSystem(std::wstring& error);
// Removes the shortcuts and the uninstall entry, then schedules the install
// folder for deletion after this process exits.
bool RemoveFromSystem(std::wstring& error);
// True when the shortcuts or the uninstall entry exist.
bool IsDeployedToSystem();

} // namespace luling
