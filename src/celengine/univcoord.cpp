// univcoord.cpp
//
// Copyright (C) 2025-present, the Celestia Development Team
// Original version by Chris Laurel <claurel@gmail.com>
//
// Universal coordinate is a high-precision fixed point coordinate for
// locating objects in 3D space on scales ranging from millimeters to
// thousands of light years.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include <celengine/univcoord.h>

#include <celastro/astro.h>

UniversalCoord UniversalCoord::offsetKm(const Eigen::Vector3d& v) const
{
    Eigen::Vector3d vUly = v * celestia::astro::kilometersToMicroLightYears(1.0);
    return *this + UniversalCoord(vUly);
}

Eigen::Vector3d UniversalCoord::offsetFromKm(const UniversalCoord& uc) const
{
    return Eigen::Vector3d(static_cast<double>(x - uc.x),
                           static_cast<double>(y - uc.y),
                           static_cast<double>(z - uc.z)) * celestia::astro::microLightYearsToKilometers(1.0);
}

double UniversalCoord::distanceFromLy(const UniversalCoord& uc) const
{
    return celestia::astro::kilometersToLightYears(offsetFromKm(uc).norm());
}

UniversalCoord UniversalCoord::CreateKm(const Eigen::Vector3d& v)
{
    Eigen::Vector3d vUly = v * celestia::astro::microLightYearsToKilometers(1.0);
    return UniversalCoord(vUly.x(), vUly.y(), vUly.z());
}
