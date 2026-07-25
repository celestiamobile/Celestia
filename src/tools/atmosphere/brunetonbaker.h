// Copyright (c) 2017 Eric Bruneton
// Copyright (C) 2026, the Celestia Development Team
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <string>

#include <celengine/brunetonatmospherefile.h>

namespace celestia::tools
{

constexpr int MaxBrunetonBakeThreads = 64;

struct BrunetonBakeSettings
{
    double bottomRadiusKm{ 6378.1366 };
    double topRadiusKm{ 6478.1366 };
    int scatteringOrders{ 4 };
    int threadCount{ 0 };
    std::uint32_t phaseSampleCount{ 1024 };
    bool emulateHalfPrecision{ true };
};

bool ValidateBrunetonBakeSettings(const BrunetonBakeSettings&, std::string& error);
engine::BrunetonAtmosphereParameters MakePhysicalEarthParameters(
    const BrunetonBakeSettings&);
engine::BrunetonTextureData MakeAnalyticEarthPhaseTexture(std::uint32_t sampleCount);
bool BakePhysicalEarthAtmosphere(const BrunetonBakeSettings&,
                                 engine::BrunetonAtmosphereData&,
                                 std::string& error);

} // namespace celestia::tools
