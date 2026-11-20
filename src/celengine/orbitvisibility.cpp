// Copyright (C) 2026, Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "orbitvisibility.h"

#include <algorithm>
#include <cmath>

namespace celestia::engine
{

namespace
{

void
addRoots(double a, double b, double c, std::vector<double>& cuts)
{
    auto add = [&cuts](double root)
    {
        if (root > 0.0 && root < 1.0)
            cuts.push_back(root);
    };

    if (a == 0.0)
    {
        if (b != 0.0)
            add(-c / (2.0 * b));
        return;
    }

    double discriminant = b * b - a * c;
    if (discriminant < 0.0)
        return;

    double q = -b - std::copysign(std::sqrt(discriminant), b);
    add(q / a);
    if (q != 0.0)
        add(c / q);
}

bool
inFrontOfLayer(const Eigen::Vector3d& point, const OrbitVisibilityLayer& layer)
{
    Eigen::Vector3d eye = layer.toUnitSphere.translation();
    Eigen::Vector3d position = layer.toUnitSphere * point;
    if (layer.volume && position.squaredNorm() < 1.0)
        return true;

    Eigen::Vector3d direction = position - eye;
    double a = direction.squaredNorm();
    double b = eye.dot(direction);
    double c = eye.squaredNorm() - 1.0;
    double discriminant = b * b - a * c;
    if (a == 0.0 || discriminant < 0.0)
        return false;

    double root = (-b + (c < 0.0 ? 1.0 : -1.0) * std::sqrt(discriminant)) / a;
    return root >= 1.0;
}

} // namespace

std::vector<OrbitVisibilitySegment>
splitOrbitSegment(
    const Eigen::Vector3d& begin,
    const Eigen::Vector3d& end,
    std::span<const OrbitVisibilityLayer> layers)
{
    std::vector<double> cuts{ 0.0, 1.0 };
    for (const auto& layer : layers)
    {
        Eigen::Vector3d eye = layer.toUnitSphere.translation();
        Eigen::Vector3d p = layer.toUnitSphere * begin;
        Eigen::Vector3d delta = layer.toUnitSphere.linear() * (end - begin);
        addRoots(delta.squaredNorm(), p.dot(delta), p.squaredNorm() - 1.0, cuts);

        // Visibility can also change where the eye-to-orbit ray grazes the shell.
        double c = eye.squaredNorm() - 1.0;
        if (c > 0.0)
        {
            Eigen::Vector3d ray = p - eye;
            double b = eye.dot(ray);
            double d = eye.dot(delta);
            addRoots(
                d * d - c * delta.squaredNorm(),
                b * d - c * ray.dot(delta),
                b * b - c * ray.squaredNorm(),
                cuts);
        }
    }

    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    std::vector<OrbitVisibilitySegment> segments;
    for (std::size_t i = 1; i < cuts.size(); ++i)
    {
        Eigen::Vector3d midpoint = begin + ((cuts[i - 1] + cuts[i]) * 0.5) * (end - begin);
        std::size_t stage = 0;
        for (std::size_t j = 0; j < layers.size(); ++j)
        {
            if (inFrontOfLayer(midpoint, layers[j]))
                stage = j + 1;
        }

        if (!segments.empty() && segments.back().stage == stage)
            segments.back().end = cuts[i];
        else
            segments.push_back({ cuts[i - 1], cuts[i], stage });
    }
    return segments;
}

} // namespace celestia::engine
