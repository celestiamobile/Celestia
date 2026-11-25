// radiometry.h
//
// Copyright (C) 2026-present, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <limits>
#include <optional>

#include <celastro/astro.h>

namespace celestia::engine::detail
{

inline constexpr std::array<float, 3> StarLimbDarkening{ 0.56f, 0.61f, 0.72f };

// The PSF integrates Vega-normalized irradiance over 96-DPI logical pixels.
// Resolved reference surfaces use the same units, not SI radiance. Textures
// modulate the reference surface without automatic average normalization.
inline float
discMeanRadiance(float irradiance, float radiusInLogicalPixels)
{
    return irradiance / (std::numbers::pi_v<float> * radiusInLogicalPixels * radiusInLogicalPixels);
}

inline float
limbDarkeningNormalization(float coefficient)
{
    return 1.0f / (1.0f - coefficient / 3.0f);
}

inline float
reflectedLuminosity(float luminosity, float distance, float radius, float geometricAlbedo)
{
    double ratio = static_cast<double>(radius) / distance;
    return static_cast<float>(luminosity * geometricAlbedo * ratio * ratio);
}

inline float
geometricAlbedoScale(float geometricAlbedo, float lunarLambert)
{
    return geometricAlbedo * 3.0f / (2.0f + lunarLambert);
}

// Disc-integrated phase law for the shader's Lambert/Lommel-Seeliger mixture.
inline float
reflectedPhase(double cosPhase, float lunarLambert)
{
    if (cosPhase >= 1.0)
        return 1.0f;
    if (cosPhase <= -1.0)
        return 0.0f;

    double phase = std::acos(cosPhase);
    double degrees = phase * 180.0 / std::numbers::pi;
    double weight = lunarLambert * std::max(0.0, 1.0 + degrees *
        (-0.019 + degrees * (0.242e-3 - degrees * 1.46e-6)));
    double lambert = (std::sin(phase) + (std::numbers::pi - phase) * cosPhase) / std::numbers::pi;
    double lommelSeeliger = 1.0 - std::sin(phase / 2.0) * std::tan(phase / 2.0)
                                  * std::log(1.0 / std::tan(phase / 4.0));
    return static_cast<float>(std::max(0.0, (2.0 * lambert * (1.0 - weight)
                                           + 3.0 * lommelSeeliger * weight) / (2.0 + lunarLambert)));
}

// Incident solar-at-1-AU units to scene radiance for a unit Lambert reflector.
inline float
reflectedRadianceScale(float logicalPixelAngularWidth)
{
    static const float solarIrradiance = astro::lumToIrradiance(1.0f, astro::AUtoKilometers(1.0f));
    return solarIrradiance * logicalPixelAngularWidth * logicalPixelAngularWidth / std::numbers::pi_v<float>;
}

// Conventional photopic approximation to the V-band Vega zero point.
// Exact conversion depends on the source spectrum, which RGB assets do not encode.
inline float
emittedRadiance(float luminance, float logicalPixelAngularWidth)
{
    constexpr double zeroMagnitudeIlluminance = 2.54e-6; // lux
    double angle = logicalPixelAngularWidth;
    return static_cast<float>(luminance * angle * angle / zeroMagnitudeIlluminance);
}

inline float
emittedDiscIrradiance(float luminance, float radius, double distance)
{
    float angularRadius = static_cast<float>(radius / std::max(distance, static_cast<double>(radius)));
    return std::numbers::pi_v<float> * emittedRadiance(luminance, angularRadius);
}

inline float
dustScatteringPhase(float cosScatteringAngle, float asymmetry)
{
    float denominator = 1.0f + asymmetry * asymmetry -
                        2.0f * asymmetry * std::clamp(cosScatteringAngle, -1.0f, 1.0f);
    return (1.0f - asymmetry * asymmetry) /
           (4.0f * std::numbers::pi_v<float> * denominator * std::sqrt(denominator));
}

inline double
cometDustActivity(double distanceAU, float exponent)
{
    return std::pow(distanceAU, -exponent);
}

inline std::optional<float>
cometDustColumnScale(float crossSection, double distanceAU, float exponent,
                    float radius, double angularWidth)
{
    if (crossSection == 0.0f)
        return 0.0f;
    double scale = 36.0 * crossSection * cometDustActivity(distanceAU, exponent) *
                   angularWidth * angularWidth / (std::numbers::pi * radius * radius);
    if (!std::isfinite(scale) || scale < 0.0 || scale > std::numeric_limits<float>::max())
        return std::nullopt;
    return static_cast<float>(scale);
}

} // namespace celestia::engine::detail
