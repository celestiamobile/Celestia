// terraindata.h
//
// Copyright (C) 2026-present, Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <cmath>
#include <filesystem>
#include <memory>

#include <Eigen/Core>

namespace celestia::engine
{
class Image;
}

/// Manages heightmap data for terrain rendering
class TerrainData
{
public:
    /// Create terrain data with heightmap source
    /// @param basePath Path to heightmap tiles directory
    explicit TerrainData(const std::filesystem::path& basePath);
    ~TerrainData();
    
    TerrainData(const TerrainData&) = delete;
    TerrainData& operator=(const TerrainData&) = delete;
    TerrainData(TerrainData&&) = delete;
    TerrainData& operator=(TerrainData&&) = delete;
    
    /// Query height at specific UV coordinate
    /// @param u Normalized U coordinate [0, 1]
    /// @param v Normalized V coordinate [0, 1]
    /// @return Height value (typically in meters)
    float getHeightAt(float u, float v) const;
    
    /// Get normal vector at specific UV coordinate
    /// @param u Normalized U coordinate [0, 1]
    /// @param v Normalized V coordinate [0, 1]
    /// @return Normal vector computed from heightmap gradients
    Eigen::Vector3f getNormalAt(float u, float v) const;
    
    /// Configure height scaling
    /// @param scale Multiply heightmap values by this amount
    void setHeightScale(float scale) { heightScale = scale; }
    
    /// Configure height offset
    /// @param offset Add this to all height values
    void setHeightOffset(float offset) { heightOffset = offset; }
    
    /// Get current height scale
    float getHeightScale() const { return heightScale; }
    
    /// Get current height offset
    float getHeightOffset() const { return heightOffset; }

    /// Returns the largest negative excursion that `getHeightAt` can produce
    /// (always >= 0). Callers can add this as a baseline so the terrain mesh
    /// never dips below `sphereRadius`, preventing z-fight with the base
    /// ellipsoid. The heightmap pixel range is normalized to [-0.5, 0.5] in
    /// sampleHeightPixel(), so worst-case negative is `0.5 * |heightScale|`
    /// (less any positive offset, clamped to 0).
    float getMaxNegativeExcursion() const
    {
        float maxNeg = 0.5f * std::abs(heightScale) - heightOffset;
        return maxNeg > 0.0f ? maxNeg : 0.0f;
    }
    
    /// Clear cached heightmap tiles
    void clearCache();
    
    /// Load all available heightmap data
    void preloadData();
    
private:
    bool ensureHeightmapLoaded() const;

    /// Compute normal from heightmap gradients using Sobel filter
    Eigen::Vector3f computeNormalFromGradients(float u, float v) const;

    /// Sample a single pixel from heightmap
    float sampleHeightPixel(float u, float v) const;
    
    std::filesystem::path basePath;
    float heightScale = 1.0f;
    float heightOffset = 0.0f;

    mutable std::unique_ptr<celestia::engine::Image> heightmapImage;
    mutable bool attemptedLoad = false;
};
