// Copyright (C) 2026-present, the Celestia Development Team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>

#include <Eigen/Core>

#include <celutil/texhandle.h>

namespace celestia::util
{
class AssociativeArray;
}

namespace celestia::engine
{

class TexturePaths;

struct RingScattering
{
    float opticalDepth{ 0.0f };
    Eigen::Vector3f albedo{ Eigen::Vector3f::Ones() };
    float asymmetry{ 0.0f };
    util::TextureHandle opticalDepthTexture{ util::TextureHandle::Invalid };
    util::TextureHandle phaseTexture{ util::TextureHandle::Invalid };
    util::TextureHandle albedoTexture{ util::TextureHandle::Invalid };
};

// Updates only the supplied properties; invalid input leaves scattering unchanged.
bool ReadRingScattering(const util::AssociativeArray&,
                        const std::filesystem::path&,
                        TexturePaths&,
                        RingScattering&);

} // namespace celestia::engine
