// Copyright (C) 2026-present, the Celestia Development Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ringscattering.h"

#include <cmath>
#include <string_view>

#include <celutil/associativearray.h>
#include <celutil/logger.h>
#include "texmanager.h"

namespace celestia::engine
{

bool ReadRingScattering(const util::AssociativeArray& data,
                        const std::filesystem::path& path,
                        TexturePaths& texturePaths,
                        RingScattering& scattering)
{
    RingScattering candidate = scattering;
    if (data.getValue("OpticalDepth") != nullptr)
    {
        auto value = data.getNumber<float>("OpticalDepth");
        if (!value.has_value() || !std::isfinite(*value) || *value < 0.0f)
        {
            util::GetLogger()->error("Ring OpticalDepth must be finite and nonnegative.\n");
            return false;
        }
        candidate.opticalDepth = *value;
    }

    if (data.getValue("SingleScatteringAlbedo") != nullptr)
    {
        auto value = data.getVector3<float>("SingleScatteringAlbedo");
        if (!value.has_value() || !value->allFinite() ||
            (value->array() < 0.0f).any() || (value->array() > 1.0f).any())
        {
            util::GetLogger()->error("Ring SingleScatteringAlbedo must have three components in [0, 1].\n");
            return false;
        }
        candidate.albedo = *value;
    }

    if (data.getValue("PhaseAsymmetry") != nullptr)
    {
        auto value = data.getNumber<float>("PhaseAsymmetry");
        if (!value.has_value() || !std::isfinite(*value) || std::abs(*value) >= 1.0f)
        {
            util::GetLogger()->error("Ring PhaseAsymmetry must be finite and strictly between -1 and 1.\n");
            return false;
        }
        candidate.asymmetry = *value;
    }

    auto readTexture = [&](std::string_view key, util::TextureHandle& handle)
    {
        if (data.getValue(key) == nullptr)
            return true;
        auto filename = data.getPath(key);
        if (!filename.has_value())
        {
            util::GetLogger()->error("Invalid filename in ring Scattering {}.\n", key);
            return false;
        }
        if (filename->empty())
        {
            handle = util::TextureHandle::Invalid;
            return true;
        }
        handle = texturePaths.getHandle(*filename, path,
            TextureFlags::LinearColorspace | TextureFlags::NoMipMaps | TextureFlags::SingleTexture);
        TextureInfo info;
        if (!texturePaths.getInfo(handle, TextureResolution::lores, info))
        {
            util::GetLogger()->error("Cannot resolve ring Scattering {}: {}\n", key, *filename);
            return false;
        }
        return true;
    };

    if (!readTexture("OpticalDepthTexture", candidate.opticalDepthTexture) ||
        !readTexture("PhaseFunctionTexture", candidate.phaseTexture) ||
        !readTexture("SingleScatteringAlbedoTexture", candidate.albedoTexture))
        return false;

    scattering = candidate;
    return true;
}

} // namespace celestia::engine
