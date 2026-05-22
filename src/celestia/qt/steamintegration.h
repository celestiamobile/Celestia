// steamintegration.h
//
// Copyright (C) 2026-present, the Celestia Development Team
//
// Optional Steamworks SDK integration for the Qt6 front-end. When the build
// is configured with -DENABLE_STEAM=ON, these functions wrap SteamAPI_Init,
// SteamAPI_Shutdown, and the Workshop content directory query. When the
// build is configured without USE_STEAM, every function is a no-op and the
// Steam SDK is not linked.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <filesystem>
#include <vector>

namespace celestia::qt
{

bool initializeSteam();
void shutdownSteam();

// Returns the addon content directories from subscribed Workshop items
// (description.json type == "addon"), sorted ascending by addon ID.
std::vector<std::filesystem::path> getSteamWorkshopAddonDirs();

// Returns the script content directories from subscribed Workshop items
// (description.json type == "script"), sorted ascending by script ID.
std::vector<std::filesystem::path> getSteamWorkshopScriptDirs();

} // namespace celestia::qt
