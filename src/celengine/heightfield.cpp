// heightfield.cpp
//
// Copyright (C) 2001-present, Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "heightfield.h"

#include <algorithm>
#include <cmath>

#include <celimage/image.h>
#include <celutil/logger.h>

using celestia::util::GetLogger;

namespace celestia::engine
{

HeightField::HeightField(int w, int h, std::vector<float>&& data) :
    m_width(w),
    m_height(h),
    m_data(std::move(data))
{
    double north = 0.0;
    double south = 0.0;
    for (int x = 0; x < m_width; ++x)
    {
        north += at(x, 0);
        south += at(x, m_height - 1);
    }
    m_poleNorth = static_cast<float>(north / m_width);
    m_poleSouth = static_cast<float>(south / m_width);
}

std::unique_ptr<HeightField>
HeightField::load(const std::filesystem::path& path)
{
    auto image = Image::load(path);
    if (image == nullptr || !image->isValid() || image->isCompressed())
    {
        GetLogger()->error("Failed to load height map {}\n", path);
        return nullptr;
    }

    const int w = image->getWidth();
    const int h = image->getHeight();
    const int components = image->getComponents();
    const int pitch = image->getPitch();
    const std::uint8_t* pixels = image->getPixels();
    if (w <= 0 || h <= 0 || components <= 0 || pixels == nullptr)
        return nullptr;

    // Elevation lives in the first channel; convert 8-bit to [0, 1].
    std::vector<float> data(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y)
    {
        const std::uint8_t* row = pixels + static_cast<std::size_t>(y) * pitch;
        for (int x = 0; x < w; ++x)
            data[static_cast<std::size_t>(y) * w + x] = row[x * components] * (1.0f / 255.0f);
    }

    return std::unique_ptr<HeightField>(new HeightField(w, h, std::move(data)));
}

float
HeightField::sampleUV(float u, float v) const
{
    float fx = u * static_cast<float>(m_width) - 0.5f;
    float fy = v * static_cast<float>(m_height) - 0.5f;

    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    float tx = fx - static_cast<float>(x0);
    float ty = fy - static_cast<float>(y0);

    auto wrapX = [this](int x) noexcept
    {
        x %= m_width;
        return x < 0 ? x + m_width : x;
    };
    auto clampY = [this](int y) noexcept
    {
        return std::clamp(y, 0, m_height - 1);
    };

    int xa = wrapX(x0);
    int xb = wrapX(x0 + 1);
    int ya = clampY(y0);
    int yb = clampY(y0 + 1);

    float top = at(xa, ya) + (at(xb, ya) - at(xa, ya)) * tx;
    float bot = at(xa, yb) + (at(xb, yb) - at(xa, yb)) * tx;
    return top + (bot - top) * ty;
}

} // namespace celestia::engine
