// brunetonatmosphere.h
//
// Copyright (C) 2026, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace celestia::render
{

enum class BrunetonTextureKind : std::uint32_t
{
    Phase = 1,
    Transmittance = 2,
    IndirectIlluminance = 3,
    MultipleScattering = 4,
    SingleAerosolsScattering = 5,
    ThetaDeviation = 6,
};

struct BrunetonTextureData
{
    BrunetonTextureKind kind;
    std::uint32_t dimensions;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
    std::vector<float> pixels;
};

class BrunetonAtmosphereData
{
public:
    bool load(const std::filesystem::path& path);

    const std::vector<BrunetonTextureData>& textures() const noexcept { return m_textures; }
    const BrunetonTextureData* find(BrunetonTextureKind kind) const noexcept;

private:
    std::vector<BrunetonTextureData> m_textures;
};

} // namespace celestia::render
