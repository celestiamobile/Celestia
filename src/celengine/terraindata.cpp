// terraindata.cpp
//
// Copyright (C) 2026-present, Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "terraindata.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <celcompat/numbers.h>
#include <celimage/image.h>
#include <celutil/logger.h>

using celestia::engine::Image;
using celestia::util::GetLogger;

TerrainData::TerrainData(const std::filesystem::path& path)
    : basePath(path)
{
}

TerrainData::~TerrainData()
{
    clearCache();
}

void TerrainData::clearCache()
{
    heightmapImage.reset();
    attemptedLoad = false;
}

void TerrainData::preloadData()
{
    (void) ensureHeightmapLoaded();
}

float TerrainData::getHeightAt(float u, float v) const
{
    u = u - std::floor(u);
    v = v - std::floor(v);

    float height = sampleHeightPixel(u, v);
    height *= heightScale;
    height += heightOffset;

    return height;
}

Eigen::Vector3f TerrainData::getNormalAt(float u, float v) const
{
    return computeNormalFromGradients(u, v);
}

Eigen::Vector3f TerrainData::computeNormalFromGradients(float u, float v) const
{
    constexpr float du = 1.0f / 4096.0f;
    constexpr float dv = 1.0f / 2048.0f;

    float hL = sampleHeightPixel(u - du, v);
    float hR = sampleHeightPixel(u + du, v);
    float hD = sampleHeightPixel(u, v - dv);
    float hU = sampleHeightPixel(u, v + dv);

    float dU = hR - hL;
    float dV = hU - hD;

    Eigen::Vector3f n(-dU, 1.0f, -dV);
    return n.normalized();
}

float TerrainData::sampleHeightPixel(float u, float v) const
{
    u = u - std::floor(u);
    v = v - std::floor(v);

    if (!ensureHeightmapLoaded() || !heightmapImage)
    {
        float h = std::sin(u * static_cast<float>(celestia::numbers::pi * 2.0)) * 0.1f;
        h += std::sin(v * static_cast<float>(celestia::numbers::pi)) * 0.05f;
        return h;
    }

    const int width = heightmapImage->getWidth();
    const int height = heightmapImage->getHeight();
    const int components = heightmapImage->getComponents();

    const float x = u * static_cast<float>(width - 1);
    // Heightmap rows are stored top-to-bottom (row 0 = north pole), matching
    // the equirectangular base-diffuse texture sampled directly with v in
    // the fragment shader. Do NOT flip v here, or the heightmap is mirrored
    // N/S relative to the diffuse (e.g. Himalayas would read from ocean
    // south of Madagascar).
    const float y = v * static_cast<float>(height - 1);

    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    int x1 = std::min(x0 + 1, width - 1);
    int y1 = std::min(y0 + 1, height - 1);

    float tx = x - static_cast<float>(x0);
    float ty = y - static_cast<float>(y0);

    const std::uint8_t* pixels = heightmapImage->getPixels();
    auto sample = [pixels, width, components](int sx, int sy) -> float
    {
        const int idx = (sy * width + sx) * components;
        const float r = static_cast<float>(pixels[idx]) / 255.0f;
        const float g = components > 1 ? static_cast<float>(pixels[idx + 1]) / 255.0f : r;
        const float b = components > 2 ? static_cast<float>(pixels[idx + 2]) / 255.0f : r;
        return (r + g + b) / 3.0f;
    };

    float h00 = sample(x0, y0);
    float h10 = sample(x1, y0);
    float h01 = sample(x0, y1);
    float h11 = sample(x1, y1);

    float h0 = h00 + (h10 - h00) * tx;
    float h1 = h01 + (h11 - h01) * tx;
    // Return raw normalized pixel value in [0, 1]. Callers map this to
    // physical elevation via heightScale + heightOffset (configured per-body
    // in the .ssc file). Heightmaps normally encode "lowest point" (ocean
    // bottom / sea level) at 0 and "highest point" at 1; recentering here
    // would push 70% of Earth's surface (oceans) deep below the base
    // sphere and hide the terrain mesh under it.
    return h0 + (h1 - h0) * ty;
}

bool TerrainData::ensureHeightmapLoaded() const
{
    if (heightmapImage)
        return true;
    if (attemptedLoad)
        return false;

    attemptedLoad = true;
    if (basePath.empty())
    {
        fmt::print(stderr, "[TERRAIN] ensureHeightmapLoaded: basePath EMPTY\n");
        return false;
    }

    // For relative paths (the common case — .ssc files specify heightmaps
    // as e.g. "../textures/medres/earth-height.jpg" relative to the data
    // directory), the literal path almost never resolves against the
    // process CWD. Search the standard Celestia texture resolution
    // subdirectories under known data roots first; this is what succeeds
    // in practice. Fall back to the path-as-given only if the search
    // misses (e.g. a custom basePath the user put outside the standard
    // texture tree).
    if (!basePath.is_absolute())
    {
        const std::string filename = basePath.filename().string();
        const std::array<const char*, 3> resDirs = {
            "textures/hires", "textures/medres", "textures/lores"
        };
        const std::array<std::filesystem::path, 3> roots = {
            std::filesystem::path{"."},
            std::filesystem::path{"output/share/celestia"},
            std::filesystem::path{"share/celestia"}
        };
        for (const auto& root : roots)
        {
            for (const char* dir : resDirs)
            {
                std::filesystem::path candidate = root / dir / filename;
                if (std::filesystem::exists(candidate))
                {
                    heightmapImage = Image::load(candidate);
                    if (heightmapImage)
                        break;
                }
            }
            if (heightmapImage)
                break;
        }
    }

    // Last-resort: try the path verbatim (handles absolute paths and any
    // unusual relative path the search above didn't cover).
    if (!heightmapImage)
        heightmapImage = Image::load(basePath);

    if (heightmapImage)
    {
        fmt::print(stderr, "[TERRAIN] ensureHeightmapLoaded: SUCCESS w={} h={} comp={}\n",
                   heightmapImage->getWidth(), heightmapImage->getHeight(),
                   heightmapImage->getComponents());
        return true;
    }

    fmt::print(stderr, "[TERRAIN] ensureHeightmapLoaded: FAILED to load '{}'\n", basePath.string());
    GetLogger()->warn("Failed to load terrain heightmap '{}', using procedural fallback.\n", basePath);
    return false;
}
