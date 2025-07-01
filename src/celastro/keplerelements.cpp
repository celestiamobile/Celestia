// keplerelements.cpp
//
// Copyright (C) 2025-present, the Celestia Development Team
// Original version by Chris Laurel <claurel@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include <celastro/keplerelements.h>
#include <celmath/mathlib.h>

namespace celestia::astro
{

namespace
{

inline void
negateIf(double& d, bool condition)
{
    if (condition)
        d = -d;
}

} // end unnamed namespace

KeplerElements
StateVectorToElements(const Eigen::Vector3d& r,
                      const Eigen::Vector3d& v,
                      double mu)
{
    constexpr double tolerance = 1e-9;

    Eigen::Vector3d h = r.cross(v);
    double rNorm = r.norm();

    KeplerElements result;

    // Compute eccentricity
    Eigen::Vector3d evec = v.cross(h) / mu - r / rNorm;
    result.eccentricity = evec.norm();

    // Compute inclination
    result.inclination = std::acos(std::clamp(h.y() / h.norm(), -1.0, 1.0));

    // Normal vector (UnitY x h)
    Eigen::Vector3d nvec(h[2], 0, -h[0]);
    double nNorm = nvec.norm();

    // compute longAscendingNode and argPericenter
    if (result.inclination < tolerance)
    {
        // handle face-on orbit: by convention Omega = 0.0
        if (result.eccentricity >= tolerance)
        {
            result.argPericenter = std::acos(evec.x() / result.eccentricity);
            negateIf(result.argPericenter, evec.z() >= 0.0);
        }
    }
    else
    {
        result.longAscendingNode = std::acos(nvec.x() / nNorm);
        negateIf(result.longAscendingNode, nvec.z() >= 0.0);
        if (result.eccentricity >= tolerance)
        {
            result.argPericenter = std::acos(std::clamp(nvec.dot(evec) / (nNorm * result.eccentricity), -1.0, 1.0));
            negateIf(result.argPericenter, evec.y() < 0.0);
        }
    }

    // compute true anomaly
    double nu;
    if (result.eccentricity >= tolerance)
    {
        nu = std::acos(std::clamp(evec.dot(r) / (result.eccentricity * rNorm), -1.0, 1.0));
        negateIf(nu, r.dot(v) < 0.0);
    }
    else
    {
        if (result.inclination < tolerance)
        {
            // circular face-on orbit
            nu = std::acos(r.x() / rNorm);
            negateIf(nu, v.x() > 0.0);
        }
        else
        {
            nu = std::acos(std::clamp(nvec.dot(r) / (nNorm * rNorm), -1.0, 1.0));
            negateIf(nu, nvec.dot(v) > 0.0);
        }
    }

    double s_nu;
    double c_nu;
    math::sincos(nu, s_nu, c_nu);

    // compute mean anomaly
    double e2 = math::square(result.eccentricity);
    if (result.eccentricity < 1.0)
    {
        double E = std::atan2(std::sqrt(1.0 - e2) * s_nu,
                              result.eccentricity + c_nu);
        result.meanAnomaly = E - result.eccentricity * std::sin(E);
    }
    else
    {
        double sinhE = std::sqrt(e2 - 1.0) * s_nu / (1.0 + result.eccentricity * c_nu);
        double E = std::asinh(sinhE);
        result.meanAnomaly = result.eccentricity * sinhE - E;
    }

    // compute semimajor axis
    result.semimajorAxis = 1.0 / (2.0 / rNorm - v.squaredNorm() / mu);
    result.period = 2.0 * celestia::numbers::pi * std::sqrt(math::cube(std::abs(result.semimajorAxis)) / mu);

    return result;
}

} // end namespace celestia::astro
