// keplerelements.h
//
// Copyright (C) 2025-present, Celestia Development Team.
// Original version by Chris Laurel <claurel@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <Eigen/Core>

namespace celestia::astro
{
struct KeplerElements
{
    double semimajorAxis{ 0.0 };
    double eccentricity{ 0.0 };
    double inclination{ 0.0 };
    double longAscendingNode{ 0.0 };
    double argPericenter{ 0.0 };
    double meanAnomaly{ 0.0 };
    double period{ 0.0 };
};


KeplerElements StateVectorToElements(const Eigen::Vector3d&,
                                     const Eigen::Vector3d&,
                                     double);
} // end namespace celestia::astro
