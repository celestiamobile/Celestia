// Copyright (C) 2026-present, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "depthpartition.h"

#include <algorithm>

namespace celestia::engine::detail
{

std::vector<DepthBufferPartition>
buildDepthPartitions(
    std::span<const RenderListEntry> entries,
    std::optional<float> nearestAnnotationZ,
    std::optional<float> farthestOrbitZ)
{
    std::vector<const RenderListEntry*> depthEntries;
    float farthest = -1.0e12f; // ~ 1 light year
    if (!entries.empty())
    {
        farthest = entries.front().farZ * 1.01f;
        for (const auto& entry : entries)
        {
            farthest = std::min(farthest, entry.farZ * 1.01f);
            // Rings are clipped across intervals rather than forcing nearby
            // moons and distant planets into a single low-precision interval.
            if (entry.discSizeInPixels > 1.0f && !entry.isRingSystem())
                depthEntries.push_back(&entry);
        }
    }

    // Transparent compositing order can differ from physical depth bounds.
    std::sort(
        depthEntries.begin(),
        depthEntries.end(),
        [](const auto* a, const auto* b) { return a->farZ < b->farZ; });

    std::vector<DepthBufferPartition> partitions;
    float prevNear = farthest;
    auto append = [&](float nearZ, float farZ)
    { partitions.push_back({ static_cast<int>(partitions.size()), nearZ, farZ }); };
    for (const auto* entry : depthEntries)
    {
        if (partitions.empty() || entry->farZ >= partitions.back().nearZ)
        {
            if (entry->farZ != prevNear)
                append(entry->farZ, prevNear);
            append(entry->nearZ, entry->farZ);
        }
        else
        {
            partitions.back().nearZ = std::max(partitions.back().nearZ, entry->nearZ);
            partitions.back().farZ = std::min(partitions.back().farZ, entry->farZ);
        }
        prevNear = partitions.back().nearZ;
    }

    float closest = nearestAnnotationZ.value_or(prevNear);
    float nearestPoint = prevNear;
    for (const auto& entry : entries)
    {
        if (entry.isRingSystem())
            closest = std::max(closest, entry.nearZ);
        else if (entry.discSizeInPixels <= 1.0f && entry.nearZ < 0.0f)
            nearestPoint = std::max(nearestPoint, entry.nearZ);
    }
    // Points have zero depth extent and must lie strictly inside the front clip plane.
    if (nearestPoint > prevNear && nearestPoint >= closest)
        closest = nearestPoint * 0.01f;
    if (closest <= prevNear)
        closest = 0.0f;
    if (!entries.empty() && closest == 0.0f)
    {
        float nearest = entries.front().nearZ;
        for (const auto& entry : entries) nearest = std::max(nearest, entry.nearZ);
        closest = nearest * 0.01f;
    }
    append(closest, prevNear);

    if (farthestOrbitZ.has_value())
        partitions.front().farZ = std::min(partitions.front().farZ, *farthestOrbitZ);

    return partitions;
}

std::vector<std::vector<const RenderListEntry*>>
partitionRenderList(
    std::span<const RenderListEntry> entries,
    std::span<const DepthBufferPartition> partitions)
{
    std::vector<std::vector<const RenderListEntry*>> result(partitions.size());
    for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
    {
        if (entry->isRingSystem())
        {
            for (std::size_t i = 0; i < partitions.size(); ++i)
            {
                if (entry->nearZ > partitions[i].farZ && entry->farZ < partitions[i].nearZ)
                    result[i].push_back(&*entry);
            }
        }
        else
        {
            auto partition = std::upper_bound(
                partitions.begin(),
                partitions.end(),
                entry->farZ,
                [](float farZ, const auto& p) { return farZ < p.nearZ; });
            if (partition != partitions.end())
                result[partition - partitions.begin()].push_back(&*entry);
        }
    }
    return result;
}

} // namespace celestia::engine::detail
