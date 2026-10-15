#pragma once
#include "network_locations.h"
#include <functional>

namespace luling::app {

// Machines visible on the local network, as \\NAME entries to browse.
//
// The Network root is walked down through its workgroup / domain containers,
// which is where Windows keeps the machines. A machine's *shares* are left for
// the folder itself to list: walking every machine would turn a file manager's
// sidebar into a crawl nobody asked for.
//
// Worker-only, like ReadNetworkLocations: it talks to the network, so it is
// bounded by a deadline and an entry cap, checks `cancel` between calls, and
// never resolves a name or opens a share. An unreachable network simply yields
// an empty list.
std::vector<NetworkLocation> EnumerateNetworkComputers(const std::function<bool()>& cancel = {});

} // namespace luling::app
