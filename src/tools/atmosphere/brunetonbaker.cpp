// Copyright (c) 2017 Eric Bruneton
// Copyright (C) 2026, the Celestia Development Team
// SPDX-License-Identifier: BSD-3-Clause

#include "brunetonbaker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "brunetonprecompute.h"

namespace celestia::tools
{

namespace
{

constexpr double Pi = 3.14159265358979323846;
constexpr double MiePhaseG = 0.8;

bruneton::DensityProfileLayer
toPrecomputeLayer(const engine::BrunetonDensityProfileLayer& layer)
{
    return {
        layer.width,
        layer.expTerm,
        layer.expScale,
        layer.linearTerm,
        layer.constantTerm,
    };
}

bruneton::dvec3
toPrecomputeVector(const std::array<float, 3>& value)
{
    return { value[0], value[1], value[2] };
}

bruneton::AtmosphereParameters
toPrecomputeParameters(const engine::BrunetonAtmosphereParameters& source)
{
    bruneton::AtmosphereParameters result;
    result.solar_irradiance = toPrecomputeVector(source.solarIrradiance);
    result.sun_angular_radius = source.sunAngularRadius;
    result.bottom_radius = source.bottomRadius;
    result.top_radius = source.topRadius;
    for (std::size_t i = 0; i < 2; ++i)
    {
        result.rayleigh_density.layers[i] = toPrecomputeLayer(source.rayleighDensity[i]);
        result.mie_density.layers[i] = toPrecomputeLayer(source.mieDensity[i]);
        result.absorption_density.layers[i] = toPrecomputeLayer(source.absorptionDensity[i]);
    }
    result.rayleigh_scattering = toPrecomputeVector(source.rayleighScattering);
    result.rayleigh_extinction = result.rayleigh_scattering;
    result.mie_scattering = toPrecomputeVector(source.mieScattering);
    result.mie_extinction = toPrecomputeVector(source.mieExtinction);
    result.mie_phase_function_g = source.miePhaseFunctionG;
    result.absorption_extinction = toPrecomputeVector(source.absorptionExtinction);
    result.ground_albedo = toPrecomputeVector(source.groundAlbedo);
    result.mu_s_min = source.muSMin;
    return result;
}

bruneton::dvec3
toRgbSpectrum(const TabulatedSpectrum& spectrum)
{
    constexpr double MetersPerKilometer = 1000.0;
    return {
        spectrum.sample(680.0) * MetersPerKilometer,
        spectrum.sample(550.0) * MetersPerKilometer,
        spectrum.sample(440.0) * MetersPerKilometer,
    };
}

void
setTabulatedDensity(const TabulatedDensity& source,
                    bruneton::DensityProfile& destination)
{
    constexpr double MetersPerKilometer = 1000.0;
    destination.altitudes.reserve(source.altitudesM.size());
    for (double altitude : source.altitudesM)
        destination.altitudes.push_back(altitude / MetersPerKilometer);
    destination.densities = source.relativeDensity;
}

void
setTabulatedPhase(const TabulatedPhase& source,
                  bruneton::PhaseFunction& destination)
{
    destination.angles = source.anglesRad;
    destination.values.reserve(source.anglesRad.size());
    for (double angle : source.anglesRad)
    {
        destination.values.emplace_back(
            source.sample(680.0, angle),
            source.sample(550.0, angle),
            source.sample(440.0, angle));
    }
}

bruneton::AtmosphereParameters
makeTabulatedPrecomputeParameters(
    const engine::BrunetonAtmosphereParameters& runtime,
    const TabulatedAtmosphereInput& input)
{
    auto result = toPrecomputeParameters(runtime);
    setTabulatedDensity(input.molecules.density, result.rayleigh_density);
    setTabulatedDensity(input.aerosols.density, result.mie_density);
    result.rayleigh_scattering = toRgbSpectrum(input.molecules.scattering);
    result.rayleigh_extinction =
        result.rayleigh_scattering + toRgbSpectrum(input.molecules.absorption);
    result.mie_scattering = toRgbSpectrum(input.aerosols.scattering);
    result.mie_extinction =
        result.mie_scattering + toRgbSpectrum(input.aerosols.absorption);
    setTabulatedPhase(input.molecules.phase, result.rayleigh_phase);
    setTabulatedPhase(input.aerosols.phase, result.mie_phase);
    if (input.absorber)
    {
        setTabulatedDensity(input.absorber->density, result.absorption_density);
        result.absorption_extinction =
            toRgbSpectrum(input.absorber->absorption);
    }
    else
    {
        result.absorption_density = {};
        result.absorption_extinction = {};
    }
    return result;
}

engine::BrunetonTextureData
packTexture(const bruneton::Tex2& source)
{
    engine::BrunetonTextureData result;
    result.width = static_cast<std::uint32_t>(source.w);
    result.height = static_cast<std::uint32_t>(source.h);
    result.depth = 1;
    result.texels.resize(source.data.size() * 4);
    for (std::size_t i = 0; i < source.data.size(); ++i)
    {
        result.texels[i * 4] = static_cast<float>(source.data[i].x);
        result.texels[i * 4 + 1] = static_cast<float>(source.data[i].y);
        result.texels[i * 4 + 2] = static_cast<float>(source.data[i].z);
        result.texels[i * 4 + 3] = 0.0f;
    }
    return result;
}

engine::BrunetonTextureData
packTexture(const bruneton::Tex3& source)
{
    engine::BrunetonTextureData result;
    result.width = static_cast<std::uint32_t>(source.w);
    result.height = static_cast<std::uint32_t>(source.h);
    result.depth = static_cast<std::uint32_t>(source.d);
    result.texels.resize(source.data.size() * 4);
    for (std::size_t i = 0; i < source.data.size(); ++i)
    {
        result.texels[i * 4] = static_cast<float>(source.data[i].x);
        result.texels[i * 4 + 1] = static_cast<float>(source.data[i].y);
        result.texels[i * 4 + 2] = static_cast<float>(source.data[i].z);
        result.texels[i * 4 + 3] = 0.0f;
    }
    return result;
}

} // namespace

bool
ValidateBrunetonBakeSettings(const BrunetonBakeSettings& settings, std::string& error)
{
    error.clear();
    const float bottomRadius = static_cast<float>(settings.bottomRadiusKm);
    const float topRadius = static_cast<float>(settings.topRadiusKm);
    if (!std::isfinite(settings.bottomRadiusKm) ||
        !std::isfinite(settings.topRadiusKm) ||
        settings.bottomRadiusKm <= 0.0 ||
        settings.topRadiusKm <= settings.bottomRadiusKm ||
        !std::isfinite(bottomRadius) ||
        !std::isfinite(topRadius) ||
        bottomRadius <= 0.0f ||
        topRadius <= bottomRadius)
    {
        error = "invalid atmosphere radii";
        return false;
    }
    if (settings.scatteringOrders < 1 || settings.scatteringOrders > 16)
    {
        error = "scattering orders must be between 1 and 16";
        return false;
    }
    if (settings.threadCount < 0 ||
        settings.threadCount > MaxBrunetonBakeThreads)
    {
        error = "thread count must be between 0 and 64";
        return false;
    }
    if (settings.phaseSampleCount < 64 || settings.phaseSampleCount > 65536)
    {
        error = "phase sample count must be between 64 and 65536";
        return false;
    }
    return true;
}

engine::BrunetonAtmosphereParameters
MakePhysicalEarthParameters(const BrunetonBakeSettings& settings)
{
    engine::BrunetonAtmosphereParameters result;
    result.solarIrradiance = { 1.474f, 1.8504f, 1.91198f };
    result.sunAngularRadius = 0.00935f / 2.0f;
    result.bottomRadius = static_cast<float>(settings.bottomRadiusKm);
    result.topRadius = static_cast<float>(settings.topRadiusKm);
    result.rayleighDensity[1] = {
        0.0f, 1.0f, -1.0f / 8.0f, 0.0f, 0.0f,
    };
    result.rayleighScattering = {
        0.0058023394f, 0.0135577628f, 0.0331000052f,
    };
    result.mieDensity[1] = {
        0.0f, 1.0f, -1.0f / 1.2f, 0.0f, 0.0f,
    };
    result.mieScattering = { 0.003996f, 0.003996f, 0.003996f };
    result.mieExtinction = { 0.00444f, 0.00444f, 0.00444f };
    result.miePhaseFunctionG = static_cast<float>(MiePhaseG);
    result.absorptionDensity[0] = {
        25.0f, 0.0f, 0.0f, 1.0f / 15.0f, -2.0f / 3.0f,
    };
    result.absorptionDensity[1] = {
        0.0f, 0.0f, 0.0f, -1.0f / 15.0f, 8.0f / 3.0f,
    };
    result.absorptionExtinction = {
        0.0006497166f, 0.0018809f, 0.0000850167f,
    };
    result.groundAlbedo = { 0.1f, 0.1f, 0.1f };
    result.muSMin = static_cast<float>(std::cos(102.0 / 180.0 * Pi));
    result.skySpectralRadianceToLuminance = {
        114974.9140625f, 71305.953125f, 65310.546875f,
    };
    result.sunSpectralRadianceToLuminance = {
        98242.7890625f, 69954.3984375f, 66475.015625f,
    };
    result.scatteringNuSize = bruneton::SCATTERING_TEXTURE_NU_SIZE;
    result.scatteringMuSSize = bruneton::SCATTERING_TEXTURE_MU_S_SIZE;
    result.valueMode = engine::BrunetonLutValueMode::Radiance;
    return result;
}

engine::BrunetonTextureData
MakeAnalyticEarthPhaseTexture(std::uint32_t sampleCount)
{
    engine::BrunetonTextureData result;
    result.width = sampleCount;
    result.height = 2;
    result.depth = 1;
    result.texels.resize(static_cast<std::size_t>(sampleCount) * 2 * 4);
    for (std::uint32_t row = 0; row < 2; ++row)
    {
        for (std::uint32_t i = 0; i < sampleCount; ++i)
        {
            const double theta = Pi * i / (sampleCount - 1);
            const double nu = std::cos(theta);
            double phase;
            if (row == 0)
            {
                phase = 3.0 / (16.0 * Pi) * (1.0 + nu * nu);
            }
            else
            {
                const double g = MiePhaseG;
                const double k =
                    3.0 / (8.0 * Pi) * (1.0 - g * g) / (2.0 + g * g);
                phase = k * (1.0 + nu * nu) /
                        std::pow(1.0 + g * g - 2.0 * g * nu, 1.5);
            }

            const std::size_t offset =
                (static_cast<std::size_t>(row) * sampleCount + i) * 4;
            std::fill_n(result.texels.begin() + offset, 3, static_cast<float>(phase));
            result.texels[offset + 3] = 1.0f;
        }
    }
    return result;
}

engine::BrunetonTextureData
MakeTabulatedPhaseTexture(const TabulatedAtmosphereInput& input)
{
    const auto sampleCount =
        static_cast<std::uint32_t>(input.molecules.phase.anglesRad.size());
    engine::BrunetonTextureData result;
    result.width = sampleCount;
    result.height = 2;
    result.depth = 1;
    result.texels.resize(static_cast<std::size_t>(sampleCount) * 2 * 4);
    const TabulatedPhase* phases[] = {
        &input.molecules.phase,
        &input.aerosols.phase,
    };
    for (std::uint32_t row = 0; row < 2; ++row)
    {
        for (std::uint32_t i = 0; i < sampleCount; ++i)
        {
            const double theta = input.molecules.phase.anglesRad[i];
            const std::size_t offset =
                (static_cast<std::size_t>(row) * sampleCount + i) * 4;
            result.texels[offset] =
                static_cast<float>(phases[row]->sample(680.0, theta));
            result.texels[offset + 1] =
                static_cast<float>(phases[row]->sample(550.0, theta));
            result.texels[offset + 2] =
                static_cast<float>(phases[row]->sample(440.0, theta));
            result.texels[offset + 3] = 1.0f;
        }

    }
    return result;
}

bool
validateScatteringRange(const bruneton::PrecomputedTextures& textures,
                        std::string& error)
{
    constexpr double MaxBinary16 = 65504.0;
    const auto valid = [=](const bruneton::dvec3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z) &&
               value.x >= 0.0 && value.y >= 0.0 && value.z >= 0.0 &&
               value.x <= MaxBinary16 && value.y <= MaxBinary16 &&
               value.z <= MaxBinary16;
    };
    if (!std::all_of(textures.scattering.data.begin(),
                     textures.scattering.data.end(), valid) ||
        !std::all_of(textures.single_mie.data.begin(),
                     textures.single_mie.data.end(), valid))
    {
        error = "tabulated phase produces scattering outside the finite "
                "binary16 range";
        return false;
    }
    return true;
}

bool
BakePhysicalEarthAtmosphere(const BrunetonBakeSettings& settings,
                            engine::BrunetonAtmosphereData& data,
                            std::string& error)
{
    if (!ValidateBrunetonBakeSettings(settings, error))
        return false;

    engine::BrunetonAtmosphereData result;
    result.parameters = MakePhysicalEarthParameters(settings);
    result.phase = MakeAnalyticEarthPhaseTexture(settings.phaseSampleCount);

    bruneton::PrecomputeSettings precomputeSettings;
    precomputeSettings.scattering_orders = settings.scatteringOrders;
    precomputeSettings.thread_count = settings.threadCount;
    precomputeSettings.emulate_half_precision = settings.emulateHalfPrecision;
    const auto textures =
        bruneton::Precompute(toPrecomputeParameters(result.parameters), precomputeSettings);

    result.transmittance = packTexture(textures.transmittance);
    result.scattering = packTexture(textures.scattering);
    result.singleMie = packTexture(textures.single_mie);
    result.irradiance = packTexture(textures.irradiance);
    data = std::move(result);
    return true;
}

bool
BakeTabulatedAtmosphere(const TabulatedAtmosphereInput& input,
                        const BrunetonBakeSettings& settings,
                        engine::BrunetonAtmosphereData& data,
                        std::string& error)
{
    if (!ValidateBrunetonBakeSettings(settings, error) ||
        !ValidateTabulatedAtmosphereInput(input, error))
    {
        return false;
    }
    BrunetonBakeSettings radii = settings;
    radii.bottomRadiusKm = input.bottomRadiusM / 1000.0;
    radii.topRadiusKm = input.topRadiusM / 1000.0;
    if (!ValidateBrunetonBakeSettings(radii, error))
        return false;
    engine::BrunetonAtmosphereData result;
    result.parameters = MakePhysicalEarthParameters(radii);
    result.parameters.rayleighScattering = {
        static_cast<float>(input.molecules.scattering.sample(680.0) * 1000.0),
        static_cast<float>(input.molecules.scattering.sample(550.0) * 1000.0),
        static_cast<float>(input.molecules.scattering.sample(440.0) * 1000.0),
    };
    result.parameters.mieScattering = {
        static_cast<float>(input.aerosols.scattering.sample(680.0) * 1000.0),
        static_cast<float>(input.aerosols.scattering.sample(550.0) * 1000.0),
        static_cast<float>(input.aerosols.scattering.sample(440.0) * 1000.0),
    };
    result.parameters.mieExtinction = {
        static_cast<float>(
            (input.aerosols.scattering.sample(680.0) +
             input.aerosols.absorption.sample(680.0)) * 1000.0),
        static_cast<float>(
            (input.aerosols.scattering.sample(550.0) +
             input.aerosols.absorption.sample(550.0)) * 1000.0),
        static_cast<float>(
            (input.aerosols.scattering.sample(440.0) +
             input.aerosols.absorption.sample(440.0)) * 1000.0),
    };
    if (input.absorber)
    {
        result.parameters.absorptionExtinction = {
            static_cast<float>(
                input.absorber->absorption.sample(680.0) * 1000.0),
            static_cast<float>(
                input.absorber->absorption.sample(550.0) * 1000.0),
            static_cast<float>(
                input.absorber->absorption.sample(440.0) * 1000.0),
        };
    }
    else
    {
        result.parameters.absorptionExtinction = {};
    }
    result.phase = MakeTabulatedPhaseTexture(input);

    bruneton::PrecomputeSettings precomputeSettings;
    precomputeSettings.scattering_orders = settings.scatteringOrders;
    precomputeSettings.thread_count = settings.threadCount;
    precomputeSettings.emulate_half_precision = settings.emulateHalfPrecision;
    const auto textures = bruneton::Precompute(
        makeTabulatedPrecomputeParameters(result.parameters, input),
        precomputeSettings);
    if (!validateScatteringRange(textures, error))
        return false;
    result.transmittance = packTexture(textures.transmittance);
    result.scattering = packTexture(textures.scattering);
    result.singleMie = packTexture(textures.single_mie);
    result.irradiance = packTexture(textures.irradiance);
    data = std::move(result);
    return true;
}

} // namespace celestia::tools
