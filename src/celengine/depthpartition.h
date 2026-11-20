// Copyright (C) 2026-present, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include "renderlistentry.h"

#include <optional>
#include <span>
#include <vector>

namespace celestia::engine
{

struct DepthBufferPartition
{
    int index;
    float nearZ;
    float farZ;
};

namespace detail
{

std::vector<DepthBufferPartition> buildDepthPartitions(
    std::span<const RenderListEntry> entries,
    std::optional<float> nearestAnnotationZ,
    std::optional<float> farthestOrbitZ);

std::vector<std::vector<const RenderListEntry*>> partitionRenderList(
    std::span<const RenderListEntry> entries,
    std::span<const DepthBufferPartition> partitions);

} // namespace detail
} // namespace celestia::engine
