// Copyright (C) 2026, Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <Eigen/Geometry>
#include <cstddef>
#include <span>
#include <vector>

namespace celestia::engine
{

struct OrbitVisibilityLayer
{
    Eigen::Affine3d toUnitSphere;
    // Interior endpoints are shaded separately for volumes, but occluded by surfaces.
    bool volume;
};

struct OrbitVisibilitySegment
{
    double begin;
    double end;
    // Zero precedes every layer; i + 1 follows layer i.
    std::size_t stage;
};

struct OrbitVisibilityScratch
{
    std::vector<double> cuts;
    std::vector<OrbitVisibilitySegment> segments;
};

// The returned segments remain valid until scratch is reused.
std::span<const OrbitVisibilitySegment> splitOrbitSegment(
    const Eigen::Vector3d& begin,
    const Eigen::Vector3d& end,
    std::span<const OrbitVisibilityLayer> layers,
    OrbitVisibilityScratch& scratch);

} // namespace celestia::engine
