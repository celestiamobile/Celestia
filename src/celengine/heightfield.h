// heightfield.h
//
// Copyright (C) 2001-present, Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace celestia::engine
{

// CPU-resident grayscale elevation map, sampled while building terrain chunk
// meshes. Texel values are normalized to [0, 1]; the caller maps them to a
// physical displacement.
class HeightField
{
public:
    static std::unique_ptr<HeightField> load(const std::filesystem::path& path);

    // u, v in [0, 1]. u (longitude) wraps; v (latitude) clamps. Bilinear.
    float sampleUV(float u, float v) const;

    // Single elevation for a pole, where the equirectangular map degenerates to a
    // point: the mean of the top (north, v=0) or bottom (south, v=1) image row.
    float poleValue(bool north) const noexcept { return north ? m_poleNorth : m_poleSouth; }

    int width() const noexcept { return m_width; }
    int height() const noexcept { return m_height; }

private:
    HeightField(int w, int h, std::vector<float>&& data);

    float at(int x, int y) const noexcept { return m_data[static_cast<std::size_t>(y) * m_width + x]; }

    int m_width;
    int m_height;
    float m_poleNorth{ 0.0f };
    float m_poleSouth{ 0.0f };
    std::vector<float> m_data; // row-major, row 0 = top of the image
};

} // namespace celestia::engine
