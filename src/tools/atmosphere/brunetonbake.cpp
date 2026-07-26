// Copyright (C) 2026, the Celestia Development Team

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <celengine/brunetonatmospherefile.h>

#include "brunetonprecompute.h"
#include "cie_constants.h"

namespace
{

using bruneton::AtmosphereParameters;
using bruneton::DensityProfileLayer;
using bruneton::PrecomputedTextures;
using celestia::engine::BrunetonAtmosphereData;
using celestia::engine::BrunetonAtmosphereParameters;
using celestia::engine::BrunetonDensityProfileLayer;
using celestia::engine::BrunetonTextureData;
using celestia::engine::BrunetonLutValueMode;

constexpr double Pi = 3.14159265358979323846;
constexpr double LengthUnitInMeters = 1000.0;
constexpr double LambdaR = 680.0;
constexpr double LambdaG = 550.0;
constexpr double LambdaB = 440.0;
constexpr int LambdaMin = 360;
constexpr int LambdaMax = 830;

// ASTM G-173 extraterrestrial irradiance, averaged over 10 nm bins.
constexpr std::array<double, 48> SolarIrradianceAtOneAu{
    1.11776, 1.14259, 1.01249, 1.14716, 1.72765, 1.73054, 1.6887, 1.61253,
    1.91198, 2.03474, 2.02042, 2.02212, 1.93377, 1.95809, 1.91686, 1.8298,
    1.8685, 1.8931, 1.85149, 1.8504, 1.8341, 1.8345, 1.8147, 1.78158,
    1.7533, 1.6965, 1.68194, 1.64654, 1.6048, 1.52143, 1.55622, 1.5113,
    1.474, 1.4482, 1.41018, 1.36775, 1.34188, 1.31429, 1.28303, 1.26758,
    1.2367, 1.2082, 1.18737, 1.14683, 1.12362, 1.1058, 1.07124, 1.04992,
};

struct Spectrum
{
    std::vector<double> wavelengths;
    std::vector<double> solarIrradiance;
    std::vector<double> rayleighScattering;
    std::vector<double> mieScattering;
    std::vector<double> mieExtinction;
    std::vector<double> absorptionExtinction;
    std::vector<double> groundAlbedo;
};

struct OpticalSample
{
    double wavelength;
    double rayleighScattering;
    double mieScattering;
    double mieExtinction;
    double absorptionExtinction;
    double groundAlbedo;
};

struct TitanConfig
{
    double bottomRadiusMeters{ 0.0 };
    double topAltitudeMeters{ 0.0 };
    double sunDistanceAu{ 0.0 };
    double sunAngularRadiusAtOneAu{ 0.0 };
    double rayleighScaleHeightMeters{ 0.0 };
    double mieScaleHeightMeters{ 0.0 };
    double absorptionScaleHeightMeters{ 0.0 };
    bool useOzoneAbsorptionProfile{ false };
    bool emulateHalfPrecision{ true };
    double miePhaseFunctionG{ 0.0 };
    double minimumSunZenithDegrees{ 0.0 };
    std::filesystem::path spectrumFile;
    std::vector<OpticalSample> opticalSamples;
};

bool
parseFiniteDouble(std::string_view text, double& value)
{
    const std::string input(text);
    char* end = nullptr;
    value = std::strtod(input.c_str(), &end);
    return end != nullptr && *end == '\0' && std::isfinite(value);
}

bool
loadOpticalSamples(const std::filesystem::path& path,
                   std::vector<OpticalSample>& samples,
                   std::string& error)
{
    std::ifstream input(path);
    if (!input)
    {
        error = "could not open spectrum file: " + path.string();
        return false;
    }

    std::string line;
    std::size_t lineNumber = 0;
    bool headerSeen = false;
    while (std::getline(input, line))
    {
        ++lineNumber;
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.find_first_not_of(" \t\r\n") == std::string::npos)
            continue;
        if (!headerSeen)
        {
            constexpr std::string_view ExpectedHeader =
                "wavelength_nm,rayleigh_scattering_per_m,"
                "mie_scattering_per_m,mie_extinction_per_m,"
                "absorption_extinction_per_m,ground_albedo";
            if (line != ExpectedHeader)
            {
                error = "invalid spectrum header";
                return false;
            }
            headerSeen = true;
            continue;
        }

        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream row(line);
        OpticalSample sample{};
        std::string extra;
        if (!(row >> sample.wavelength >>
              sample.rayleighScattering >>
              sample.mieScattering >>
              sample.mieExtinction >>
              sample.absorptionExtinction >>
              sample.groundAlbedo) ||
            (row >> extra))
        {
            error = "invalid spectrum row " + std::to_string(lineNumber);
            return false;
        }

        const std::array<double, 6> values{
            sample.wavelength,
            sample.rayleighScattering,
            sample.mieScattering,
            sample.mieExtinction,
            sample.absorptionExtinction,
            sample.groundAlbedo,
        };
        if (!std::all_of(values.begin(),
                         values.end(),
                         [](double value) { return std::isfinite(value); }) ||
            sample.wavelength <= 0.0 ||
            sample.rayleighScattering < 0.0 ||
            sample.mieScattering < 0.0 ||
            sample.mieExtinction < sample.mieScattering ||
            sample.absorptionExtinction < 0.0 ||
            sample.groundAlbedo < 0.0 ||
            sample.groundAlbedo > 1.0 ||
            (!samples.empty() &&
             sample.wavelength <= samples.back().wavelength))
        {
            error = "invalid spectrum values on row " +
                    std::to_string(lineNumber);
            return false;
        }
        samples.push_back(sample);
    }

    if (!headerSeen || samples.size() < 2 ||
        samples.front().wavelength > LambdaMin ||
        samples.back().wavelength < LambdaMax)
    {
        error = "spectrum must cover 360 through 830 nm";
        return false;
    }
    return true;
}

bool
loadTitanConfig(const std::filesystem::path& path,
                TitanConfig& config,
                std::string& error)
{
    std::ifstream input(path);
    if (!input)
    {
        error = "could not open config file: " + path.string();
        return false;
    }

    std::set<std::string> seen;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line))
    {
        ++lineNumber;
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);
        std::istringstream fields(line);
        std::string key;
        std::string value;
        std::string extra;
        if (!(fields >> key))
            continue;
        if (!(fields >> value) || (fields >> extra))
        {
            error = "invalid config row " + std::to_string(lineNumber);
            return false;
        }
        if (!seen.insert(key).second)
        {
            error = "duplicate config key: " + key;
            return false;
        }

        double parsed = 0.0;
        if (key == "spectrum_file")
        {
            config.spectrumFile = value;
            continue;
        }
        if (key == "absorption_profile")
        {
            if (value == "exponential")
                config.useOzoneAbsorptionProfile = false;
            else if (value == "ozone")
                config.useOzoneAbsorptionProfile = true;
            else
            {
                error = "invalid absorption profile";
                return false;
            }
            continue;
        }
        if (key == "emulate_half_precision")
        {
            if (value == "true")
                config.emulateHalfPrecision = true;
            else if (value == "false")
                config.emulateHalfPrecision = false;
            else
            {
                error = "invalid emulate_half_precision value";
                return false;
            }
            continue;
        }
        if (!parseFiniteDouble(value, parsed))
        {
            error = "invalid numeric value for " + key;
            return false;
        }

        if (key == "bottom_radius_m")
            config.bottomRadiusMeters = parsed;
        else if (key == "top_altitude_m")
            config.topAltitudeMeters = parsed;
        else if (key == "sun_distance_au")
            config.sunDistanceAu = parsed;
        else if (key == "sun_angular_radius_at_1au_rad")
            config.sunAngularRadiusAtOneAu = parsed;
        else if (key == "rayleigh_scale_height_m")
            config.rayleighScaleHeightMeters = parsed;
        else if (key == "mie_scale_height_m")
            config.mieScaleHeightMeters = parsed;
        else if (key == "absorption_scale_height_m")
            config.absorptionScaleHeightMeters = parsed;
        else if (key == "mie_phase_g")
            config.miePhaseFunctionG = parsed;
        else if (key == "minimum_sun_zenith_degrees")
            config.minimumSunZenithDegrees = parsed;
        else
        {
            error = "unknown config key: " + key;
            return false;
        }
    }

    constexpr std::array<std::string_view, 10> RequiredKeys{
        "bottom_radius_m",
        "top_altitude_m",
        "sun_distance_au",
        "sun_angular_radius_at_1au_rad",
        "rayleigh_scale_height_m",
        "mie_scale_height_m",
        "absorption_scale_height_m",
        "mie_phase_g",
        "minimum_sun_zenith_degrees",
        "spectrum_file",
    };
    for (std::string_view key : RequiredKeys)
    {
        if (seen.find(std::string(key)) == seen.end())
        {
            error = "missing config key: " + std::string(key);
            return false;
        }
    }

    if (config.bottomRadiusMeters <= 0.0 ||
        config.topAltitudeMeters <= 0.0 ||
        config.sunDistanceAu <= 0.0 ||
        config.sunAngularRadiusAtOneAu <= 0.0 ||
        config.rayleighScaleHeightMeters <= 0.0 ||
        config.mieScaleHeightMeters <= 0.0 ||
        config.absorptionScaleHeightMeters <= 0.0 ||
        config.miePhaseFunctionG <= -1.0 ||
        config.miePhaseFunctionG >= 1.0 ||
        config.minimumSunZenithDegrees <= 0.0 ||
        config.minimumSunZenithDegrees >= 180.0)
    {
        error = "config contains an out-of-range value";
        return false;
    }

    const std::filesystem::path spectrumPath =
        path.parent_path() / config.spectrumFile;
    return loadOpticalSamples(spectrumPath, config.opticalSamples, error);
}

double
interpolate(const std::vector<double>& wavelengths,
            const std::vector<double>& values,
            double wavelength)
{
    if (wavelength <= wavelengths.front())
        return values.front();

    for (std::size_t i = 0; i + 1 < wavelengths.size(); ++i)
    {
        if (wavelength < wavelengths[i + 1])
        {
            const double t = (wavelength - wavelengths[i]) /
                             (wavelengths[i + 1] - wavelengths[i]);
            return values[i] * (1.0 - t) + values[i + 1] * t;
        }
    }

    return values.back();
}

double
cieValue(double wavelength, int column)
{
    if (wavelength <= LambdaMin || wavelength >= LambdaMax)
        return 0.0;

    double rowPosition = (wavelength - LambdaMin) / 5.0;
    const int row = static_cast<int>(std::floor(rowPosition));
    rowPosition -= row;
    return bruneton::CIE_2_DEG_COLOR_MATCHING_FUNCTIONS[4 * row + column] *
               (1.0 - rowPosition) +
           bruneton::CIE_2_DEG_COLOR_MATCHING_FUNCTIONS[
               4 * (row + 1) + column] *
               rowPosition;
}

std::array<float, 3>
radianceToLuminance(const Spectrum& spectrum, double lambdaPower)
{
    std::array<double, 3> result{};
    const std::array<double, 3> referenceWavelengths{ LambdaR, LambdaG, LambdaB };
    const std::array<double, 3> referenceSolar{
        interpolate(spectrum.wavelengths, spectrum.solarIrradiance, LambdaR),
        interpolate(spectrum.wavelengths, spectrum.solarIrradiance, LambdaG),
        interpolate(spectrum.wavelengths, spectrum.solarIrradiance, LambdaB),
    };

    for (int wavelength = LambdaMin; wavelength < LambdaMax; ++wavelength)
    {
        const double x = cieValue(wavelength, 1);
        const double y = cieValue(wavelength, 2);
        const double z = cieValue(wavelength, 3);
        const double solar = interpolate(spectrum.wavelengths,
                                         spectrum.solarIrradiance,
                                         wavelength);
        for (std::size_t channel = 0; channel < result.size(); ++channel)
        {
            const double* matrix = bruneton::XYZ_TO_SRGB + channel * 3;
            const double colorMatching = matrix[0] * x + matrix[1] * y +
                                         matrix[2] * z;
            result[channel] +=
                colorMatching * solar / referenceSolar[channel] *
                std::pow(wavelength / referenceWavelengths[channel],
                         lambdaPower);
        }
    }

    return {
        static_cast<float>(result[0] * bruneton::MAX_LUMINOUS_EFFICACY),
        static_cast<float>(result[1] * bruneton::MAX_LUMINOUS_EFFICACY),
        static_cast<float>(result[2] * bruneton::MAX_LUMINOUS_EFFICACY),
    };
}

Spectrum
buildTitanSpectrum(const TitanConfig& config)
{
    Spectrum result;
    std::vector<double> sampleWavelengths;
    std::vector<double> rayleighScattering;
    std::vector<double> mieScattering;
    std::vector<double> mieExtinction;
    std::vector<double> absorptionExtinction;
    std::vector<double> groundAlbedo;
    sampleWavelengths.reserve(config.opticalSamples.size());
    rayleighScattering.reserve(config.opticalSamples.size());
    mieScattering.reserve(config.opticalSamples.size());
    mieExtinction.reserve(config.opticalSamples.size());
    absorptionExtinction.reserve(config.opticalSamples.size());
    groundAlbedo.reserve(config.opticalSamples.size());
    for (const OpticalSample& sample : config.opticalSamples)
    {
        sampleWavelengths.push_back(sample.wavelength);
        rayleighScattering.push_back(sample.rayleighScattering);
        mieScattering.push_back(sample.mieScattering);
        mieExtinction.push_back(sample.mieExtinction);
        absorptionExtinction.push_back(sample.absorptionExtinction);
        groundAlbedo.push_back(sample.groundAlbedo);
    }

    const double distanceScale =
        1.0 / (config.sunDistanceAu * config.sunDistanceAu);
    for (int wavelength = LambdaMin; wavelength <= LambdaMax; wavelength += 10)
    {
        result.wavelengths.push_back(wavelength);
        result.solarIrradiance.push_back(
            SolarIrradianceAtOneAu[(wavelength - LambdaMin) / 10] *
            distanceScale);
        result.rayleighScattering.push_back(
            interpolate(sampleWavelengths, rayleighScattering, wavelength));
        result.mieScattering.push_back(
            interpolate(sampleWavelengths, mieScattering, wavelength));
        result.mieExtinction.push_back(
            interpolate(sampleWavelengths, mieExtinction, wavelength));
        result.absorptionExtinction.push_back(
            interpolate(sampleWavelengths, absorptionExtinction, wavelength));
        result.groundAlbedo.push_back(
            interpolate(sampleWavelengths, groundAlbedo, wavelength));
    }
    return result;
}

bruneton::dvec3
sampleSpectrum(const Spectrum& spectrum,
               const std::vector<double>& values,
               const std::array<double, 3>& wavelengths)
{
    return {
        interpolate(spectrum.wavelengths,
                    values,
                    wavelengths[0]),
        interpolate(spectrum.wavelengths,
                    values,
                    wavelengths[1]),
        interpolate(spectrum.wavelengths,
                    values,
                    wavelengths[2]),
    };
}

DensityProfileLayer
exponentialLayer(double scaleHeightMeters)
{
    return { 0.0, 1.0, -LengthUnitInMeters / scaleHeightMeters, 0.0, 0.0 };
}

AtmosphereParameters
buildTitanAtmosphere(const TitanConfig& config,
                     const Spectrum& spectrum,
                     const std::array<double, 3>& wavelengths)
{
    AtmosphereParameters atmosphere;
    atmosphere.solar_irradiance =
        sampleSpectrum(spectrum, spectrum.solarIrradiance, wavelengths);
    atmosphere.sun_angular_radius =
        config.sunAngularRadiusAtOneAu / config.sunDistanceAu;
    atmosphere.bottom_radius =
        config.bottomRadiusMeters / LengthUnitInMeters;
    atmosphere.top_radius =
        (config.bottomRadiusMeters + config.topAltitudeMeters) /
        LengthUnitInMeters;
    atmosphere.rayleigh_density.layers[1] =
        exponentialLayer(config.rayleighScaleHeightMeters);
    atmosphere.mie_density.layers[1] =
        exponentialLayer(config.mieScaleHeightMeters);
    atmosphere.rayleigh_scattering =
        sampleSpectrum(spectrum,
                       spectrum.rayleighScattering,
                       wavelengths) *
        LengthUnitInMeters;
    atmosphere.mie_scattering =
        sampleSpectrum(spectrum, spectrum.mieScattering, wavelengths) *
        LengthUnitInMeters;
    atmosphere.mie_extinction =
        sampleSpectrum(spectrum, spectrum.mieExtinction, wavelengths) *
        LengthUnitInMeters;
    atmosphere.mie_phase_function_g = config.miePhaseFunctionG;
    if (config.useOzoneAbsorptionProfile)
    {
        atmosphere.absorption_density.layers[0] =
            { 25.0, 0.0, 0.0, 1.0 / 15.0, -2.0 / 3.0 };
        atmosphere.absorption_density.layers[1] =
            { 0.0, 0.0, 0.0, -1.0 / 15.0, 8.0 / 3.0 };
    }
    else
    {
        atmosphere.absorption_density.layers[1] =
            exponentialLayer(config.absorptionScaleHeightMeters);
    }
    atmosphere.absorption_extinction =
        sampleSpectrum(spectrum,
                       spectrum.absorptionExtinction,
                       wavelengths) *
        LengthUnitInMeters;
    atmosphere.ground_albedo =
        sampleSpectrum(spectrum, spectrum.groundAlbedo, wavelengths);
    atmosphere.mu_s_min =
        std::cos(config.minimumSunZenithDegrees * Pi / 180.0);
    return atmosphere;
}

BrunetonDensityProfileLayer
toFileLayer(const DensityProfileLayer& source)
{
    return {
        static_cast<float>(source.width),
        static_cast<float>(source.exp_term),
        static_cast<float>(source.exp_scale),
        static_cast<float>(source.linear_term),
        static_cast<float>(source.constant_term),
    };
}

std::array<float, 3>
toArray(const bruneton::dvec3& source)
{
    return {
        static_cast<float>(source.x),
        static_cast<float>(source.y),
        static_cast<float>(source.z),
    };
}

BrunetonTextureData
makeTexture(std::uint32_t width,
            std::uint32_t height,
            std::uint32_t depth,
            std::vector<float>&& texels)
{
    return { width, height, depth, std::move(texels) };
}

BrunetonAtmosphereData
makeFileData(const AtmosphereParameters& atmosphere,
             const Spectrum& solar,
             const PrecomputedTextures& textures,
             BrunetonLutValueMode valueMode,
             bool combinedScattering)
{
    BrunetonAtmosphereData data;
    BrunetonAtmosphereParameters& parameters = data.parameters;
    parameters.solarIrradiance = toArray(atmosphere.solar_irradiance);
    parameters.sunAngularRadius =
        static_cast<float>(atmosphere.sun_angular_radius);
    parameters.bottomRadius = static_cast<float>(atmosphere.bottom_radius);
    parameters.topRadius = static_cast<float>(atmosphere.top_radius);
    for (std::size_t i = 0; i < 2; ++i)
    {
        parameters.rayleighDensity[i] =
            toFileLayer(atmosphere.rayleigh_density.layers[i]);
        parameters.mieDensity[i] =
            toFileLayer(atmosphere.mie_density.layers[i]);
        parameters.absorptionDensity[i] =
            toFileLayer(atmosphere.absorption_density.layers[i]);
    }
    parameters.rayleighScattering = toArray(atmosphere.rayleigh_scattering);
    parameters.mieScattering = toArray(atmosphere.mie_scattering);
    parameters.mieExtinction = toArray(atmosphere.mie_extinction);
    parameters.miePhaseFunctionG =
        static_cast<float>(atmosphere.mie_phase_function_g);
    parameters.absorptionExtinction =
        toArray(atmosphere.absorption_extinction);
    parameters.groundAlbedo = toArray(atmosphere.ground_albedo);
    parameters.muSMin = static_cast<float>(atmosphere.mu_s_min);
    if (valueMode == BrunetonLutValueMode::Radiance)
    {
        parameters.skySpectralRadianceToLuminance =
            radianceToLuminance(solar, -3.0);
        parameters.sunSpectralRadianceToLuminance =
            radianceToLuminance(solar, 0.0);
    }
    else
    {
        parameters.skySpectralRadianceToLuminance = { 1.0f, 1.0f, 1.0f };
        parameters.sunSpectralRadianceToLuminance = { 1.0f, 1.0f, 1.0f };
    }
    parameters.combinedScattering = combinedScattering;
    parameters.valueMode = valueMode;

    data.transmittance = makeTexture(
        celestia::engine::BrunetonTransmittanceWidth,
        celestia::engine::BrunetonTransmittanceHeight,
        1,
        textures.TransmittanceRGBA());
    data.scattering = makeTexture(
        celestia::engine::BrunetonScatteringWidth,
        celestia::engine::BrunetonScatteringHeight,
        celestia::engine::BrunetonScatteringDepth,
        textures.ScatteringRGBA(combinedScattering));
    if (!combinedScattering)
    {
        data.singleMie = makeTexture(
            celestia::engine::BrunetonScatteringWidth,
            celestia::engine::BrunetonScatteringHeight,
            celestia::engine::BrunetonScatteringDepth,
            textures.SingleMieRGBA());
    }
    data.irradiance = makeTexture(
        celestia::engine::BrunetonIrradianceWidth,
        celestia::engine::BrunetonIrradianceHeight,
        1,
        textures.IrradianceRGBA());
    return data;
}

bruneton::dvec3
toLinearSrgb(const bruneton::dvec3& samples,
             const std::array<std::array<double, 3>, 3>& matrix)
{
    return {
        matrix[0][0] * samples.x + matrix[0][1] * samples.y +
            matrix[0][2] * samples.z,
        matrix[1][0] * samples.x + matrix[1][1] * samples.y +
            matrix[1][2] * samples.z,
        matrix[2][0] * samples.x + matrix[2][1] * samples.y +
            matrix[2][2] * samples.z,
    };
}

template<typename Texture>
void
accumulateTexture(Texture& destination,
                  const Texture& source,
                  const std::array<std::array<double, 3>, 3>& matrix)
{
    if (destination.data.empty())
    {
        destination = source;
        std::fill(destination.data.begin(),
                  destination.data.end(),
                  bruneton::dvec3{});
    }

    for (std::size_t i = 0; i < source.data.size(); ++i)
        destination.data[i] += toLinearSrgb(source.data[i], matrix);
}

PrecomputedTextures
precomputeLuminance(const Spectrum& solar,
                    const TitanConfig& config,
                    int wavelengthCount,
                    int scatteringOrders,
                    PrecomputedTextures* previousOrder,
                    PrecomputedTextures* antepreviousOrder)
{
    const int iterationCount = (wavelengthCount + 2) / 3;
    const double wavelengthStep =
        static_cast<double>(LambdaMax - LambdaMin) / (3 * iterationCount);
    PrecomputedTextures accumulated;
    bruneton::g_emulate_half = false;

    for (int iteration = 0; iteration < iterationCount; ++iteration)
    {
        std::array<double, 3> wavelengths{};
        for (int sample = 0; sample < 3; ++sample)
        {
            wavelengths[sample] =
                LambdaMin +
                (3 * iteration + sample + 0.5) * wavelengthStep;
        }

        std::array<std::array<double, 3>, 3> matrix{};
        for (std::size_t output = 0; output < 3; ++output)
        {
            for (std::size_t sample = 0; sample < 3; ++sample)
            {
                const double x = cieValue(wavelengths[sample], 1);
                const double y = cieValue(wavelengths[sample], 2);
                const double z = cieValue(wavelengths[sample], 3);
                const double* row = bruneton::XYZ_TO_SRGB + output * 3;
                matrix[output][sample] =
                    bruneton::MAX_LUMINOUS_EFFICACY * wavelengthStep *
                    (row[0] * x + row[1] * y + row[2] * z);
            }
        }

        std::cout << "Spectral triplet " << iteration + 1 << '/'
                  << iterationCount << ": "
                  << wavelengths[0] << ", "
                  << wavelengths[1] << ", "
                  << wavelengths[2] << " nm\n";
        const AtmosphereParameters atmosphere =
            buildTitanAtmosphere(config, solar, wavelengths);
        PrecomputedTextures previousSpectral;
        PrecomputedTextures antepreviousSpectral;
        const PrecomputedTextures spectral =
            bruneton::Precompute(atmosphere,
                                 scatteringOrders,
                                 0,
                                 previousOrder == nullptr
                                     ? nullptr
                                     : &previousSpectral,
                                 antepreviousOrder == nullptr
                                     ? nullptr
                                     : &antepreviousSpectral);
        accumulateTexture(accumulated.scattering,
                          spectral.scattering,
                          matrix);
        accumulateTexture(accumulated.single_mie,
                          spectral.single_mie,
                          matrix);
        accumulateTexture(accumulated.irradiance,
                          spectral.irradiance,
                          matrix);
        if (previousOrder != nullptr)
        {
            accumulateTexture(previousOrder->scattering,
                              previousSpectral.scattering,
                              matrix);
            accumulateTexture(previousOrder->irradiance,
                              previousSpectral.irradiance,
                              matrix);
        }
        if (antepreviousOrder != nullptr)
        {
            accumulateTexture(antepreviousOrder->scattering,
                              antepreviousSpectral.scattering,
                              matrix);
            accumulateTexture(antepreviousOrder->irradiance,
                              antepreviousSpectral.irradiance,
                              matrix);
        }
    }

    const std::array<double, 3> rgbWavelengths{ LambdaR, LambdaG, LambdaB };
    const AtmosphereParameters rgbAtmosphere =
        buildTitanAtmosphere(config, solar, rgbWavelengths);
    // Bruneton's spectral path accumulates scattering and irradiance, then
    // recomputes transmittance at the runtime's fixed RGB wavelengths.
    const PrecomputedTextures rgb =
        bruneton::Precompute(rgbAtmosphere, 1);
    accumulated.transmittance = rgb.transmittance;
    return accumulated;
}

PrecomputedTextures
precomputeAtmosphere(const TitanConfig& config,
                     const Spectrum& spectrum,
                     BrunetonLutValueMode valueMode,
                     int wavelengthCount,
                     int scatteringOrders,
                     PrecomputedTextures* previousOrder = nullptr,
                     PrecomputedTextures* antepreviousOrder = nullptr)
{
    if (valueMode == BrunetonLutValueMode::PrecomputedLuminance)
    {
        return precomputeLuminance(spectrum,
                                   config,
                                   wavelengthCount,
                                   scatteringOrders,
                                   previousOrder,
                                   antepreviousOrder);
    }

    const std::array<double, 3> rgbWavelengths{ LambdaR, LambdaG, LambdaB };
    const AtmosphereParameters atmosphere =
        buildTitanAtmosphere(config, spectrum, rgbWavelengths);
    bruneton::g_emulate_half = config.emulateHalfPrecision;
    return bruneton::Precompute(atmosphere,
                                scatteringOrders,
                                0,
                                previousOrder,
                                antepreviousOrder);
}

struct ConvergenceChange
{
    double peakRelative;
    double rmsRelative;
    double peakAbsolute;
    double rmsAbsolute;
    double referencePeak;
    double referenceRms;
};

template<typename Texture>
ConvergenceChange
measureChange(const Texture& previous, const Texture& current)
{
    if (previous.data.size() != current.data.size() ||
        current.data.empty())
    {
        return {
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity(),
            0.0,
            0.0,
        };
    }

    double maximumReference = 0.0;
    double maximumDifference = 0.0;
    double referenceSquares = 0.0;
    double differenceSquares = 0.0;
    auto accumulate = [&](double before, double after)
    {
        const double difference = std::abs(after - before);
        maximumReference = std::max(maximumReference, std::abs(after));
        maximumDifference = std::max(maximumDifference, difference);
        referenceSquares += after * after;
        differenceSquares += difference * difference;
    };

    for (std::size_t i = 0; i < current.data.size(); ++i)
    {
        accumulate(previous.data[i].x, current.data[i].x);
        accumulate(previous.data[i].y, current.data[i].y);
        accumulate(previous.data[i].z, current.data[i].z);
    }

    const double peakRelative =
        maximumReference == 0.0
            ? (maximumDifference == 0.0 ? 0.0
                                        : std::numeric_limits<double>::infinity())
            : maximumDifference / maximumReference;
    const double rmsRelative =
        referenceSquares == 0.0
            ? (differenceSquares == 0.0
                   ? 0.0
                   : std::numeric_limits<double>::infinity())
            : std::sqrt(differenceSquares / referenceSquares);
    return {
        peakRelative,
        rmsRelative,
        maximumDifference,
        std::sqrt(differenceSquares),
        maximumReference,
        std::sqrt(referenceSquares),
    };
}

struct ConvergenceTail
{
    double peakRelative;
    double rmsRelative;
    double peakRatio;
    double rmsRatio;
};

double
estimateTail(double previousIncrement,
             double currentIncrement,
             double reference,
             double& ratio)
{
    if (currentIncrement == 0.0)
    {
        ratio = 0.0;
        return 0.0;
    }
    if (previousIncrement <= 0.0 || reference <= 0.0)
    {
        ratio = std::numeric_limits<double>::infinity();
        return std::numeric_limits<double>::infinity();
    }

    ratio = currentIncrement / previousIncrement;
    if (ratio >= 1.0)
        return std::numeric_limits<double>::infinity();

    return currentIncrement * ratio / (1.0 - ratio) / reference;
}

template<typename Texture>
ConvergenceTail
measureTail(const Texture& anteprevious,
            const Texture& previous,
            const Texture& current)
{
    const ConvergenceChange previousChange =
        measureChange(anteprevious, previous);
    const ConvergenceChange currentChange =
        measureChange(previous, current);
    ConvergenceTail tail{};
    tail.peakRelative =
        estimateTail(previousChange.peakAbsolute,
                     currentChange.peakAbsolute,
                     currentChange.referencePeak,
                     tail.peakRatio);
    tail.rmsRelative =
        estimateTail(previousChange.rmsAbsolute,
                     currentChange.rmsAbsolute,
                     currentChange.referenceRms,
                     tail.rmsRatio);
    return tail;
}

bool
validateConvergence(const PrecomputedTextures& anteprevious,
                    const PrecomputedTextures& previous,
                    const PrecomputedTextures& current,
                    double tolerance)
{
    const ConvergenceTail scattering =
        measureTail(anteprevious.scattering,
                    previous.scattering,
                    current.scattering);
    const ConvergenceTail irradiance =
        measureTail(anteprevious.irradiance,
                    previous.irradiance,
                    current.irradiance);
    std::cout << "Estimated omitted tail: scattering peak="
              << scattering.peakRelative * 100.0
              << "% rms=" << scattering.rmsRelative * 100.0
              << "% (ratios " << scattering.peakRatio
              << ", " << scattering.rmsRatio
              << "); irradiance peak="
              << irradiance.peakRelative * 100.0
              << "% rms=" << irradiance.rmsRelative * 100.0
              << "% (ratios " << irradiance.peakRatio
              << ", " << irradiance.rmsRatio << ")\n";

    if (scattering.peakRelative > tolerance ||
        scattering.rmsRelative > tolerance ||
        irradiance.peakRelative > tolerance ||
        irradiance.rmsRelative > tolerance)
    {
        std::cerr << "Atmosphere has not converged within "
                  << tolerance * 100.0 << "%\n";
        return false;
    }
    return true;
}

bool
parsePositiveInt(std::string_view text, int& value)
{
    const std::string input(text);
    char* end = nullptr;
    const long parsed = std::strtol(input.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || parsed < 1 ||
        parsed > std::numeric_limits<int>::max())
    {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool
parsePositiveDouble(std::string_view text, double& value)
{
    return parseFiniteDouble(text, value) && value > 0.0;
}

bool
validateHalfPrecisionRange(const BrunetonAtmosphereData& data)
{
    constexpr float MaximumHalf = 65504.0f;
    auto maximumAbsolute = [](const BrunetonTextureData& texture)
    {
        float result = 0.0f;
        for (float value : texture.texels)
            result = std::max(result, std::abs(value));
        return result;
    };

    const float scatteringMaximum = maximumAbsolute(data.scattering);
    const float singleMieMaximum = data.parameters.combinedScattering
                                       ? 0.0f
                                       : maximumAbsolute(data.singleMie);
    std::cout << "Peak scattering values: combined="
              << scatteringMaximum;
    if (!data.parameters.combinedScattering)
        std::cout << ", single-Mie=" << singleMieMaximum;
    std::cout << '\n';

    if (!std::isfinite(scatteringMaximum) ||
        !std::isfinite(singleMieMaximum) ||
        scatteringMaximum > MaximumHalf ||
        singleMieMaximum > MaximumHalf)
    {
        std::cerr << "Scattering exceeds the runtime RGBA16F range\n";
        return false;
    }
    return true;
}

void
printUsage(const char* executable)
{
    std::cerr << "Usage: " << executable
              << " --config FILE --output FILE --orders COUNT"
                 " [--mode rgb|luminance] [--wavelengths 15]"
                 " [--convergence-tolerance FRACTION]"
                 " [--skip-convergence-check]\n";
}

bool
validateOutput(const std::filesystem::path& path)
{
    BrunetonAtmosphereData data;
    std::string error;
    if (!celestia::engine::LoadBrunetonAtmosphere(path, data, error))
    {
        std::cerr << "Could not reload atmosphere: " << error << '\n';
        return false;
    }

    float minimumTransmittance = 1.0f;
    float maximumTransmittance = 0.0f;
    for (std::size_t i = 0; i < data.transmittance.texels.size(); i += 4)
    {
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            const float value = data.transmittance.texels[i + channel];
            if (!std::isfinite(value) || value < 0.0f || value > 1.00001f)
            {
                std::cerr << "Transmittance is outside [0, 1]\n";
                return false;
            }
            minimumTransmittance = std::min(minimumTransmittance, value);
            maximumTransmittance = std::max(maximumTransmittance, value);
        }
    }

    auto isFiniteNonnegative = [](const BrunetonTextureData& texture)
    {
        return std::all_of(texture.texels.begin(),
                           texture.texels.end(),
                           [](float value)
                           {
                               return std::isfinite(value) && value >= 0.0f;
                           });
    };
    const bool radianceMode =
        data.parameters.valueMode == BrunetonLutValueMode::Radiance;
    if ((radianceMode &&
         (!isFiniteNonnegative(data.scattering) ||
          !isFiniteNonnegative(data.irradiance))) ||
        !std::all_of(data.scattering.texels.begin(),
                     data.scattering.texels.end(),
                     [](float value) { return std::isfinite(value); }) ||
        !std::all_of(data.irradiance.texels.begin(),
                     data.irradiance.texels.end(),
                     [](float value) { return std::isfinite(value); }))
    {
        std::cerr << "Scattering output contains an invalid value\n";
        return false;
    }
    if (minimumTransmittance >= 0.5f || maximumTransmittance <= 0.99f)
    {
        std::cerr << "Transmittance does not span opaque and clear paths\n";
        return false;
    }

    std::cout << "Validated transmittance range ["
              << minimumTransmittance << ", "
              << maximumTransmittance << "]\n";
    return true;
}

} // namespace

int
main(int argc, char** argv)
{
    std::filesystem::path outputPath;
    int scatteringOrders = 0;
    int wavelengthCount = 15;
    double convergenceTolerance = 0.01;
    bool skipConvergenceCheck = false;
    std::filesystem::path configPath;
    BrunetonLutValueMode valueMode = BrunetonLutValueMode::Radiance;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument = argv[i];
        if (argument == "--config" && i + 1 < argc)
        {
            configPath = argv[++i];
        }
        else if (argument == "--output" && i + 1 < argc)
        {
            outputPath = argv[++i];
        }
        else if (argument == "--orders" && i + 1 < argc)
        {
            if (!parsePositiveInt(argv[++i], scatteringOrders))
            {
                std::cerr << "Invalid scattering order count\n";
                return 2;
            }
        }
        else if (argument == "--wavelengths" && i + 1 < argc)
        {
            if (!parsePositiveInt(argv[++i], wavelengthCount) ||
                wavelengthCount < 3)
            {
                std::cerr << "Invalid wavelength count\n";
                return 2;
            }
        }
        else if (argument == "--mode" && i + 1 < argc)
        {
            const std::string_view mode = argv[++i];
            if (mode == "rgb")
                valueMode = BrunetonLutValueMode::Radiance;
            else if (mode == "luminance")
                valueMode = BrunetonLutValueMode::PrecomputedLuminance;
            else
            {
                std::cerr << "Invalid output mode\n";
                return 2;
            }
        }
        else if (argument == "--convergence-tolerance" && i + 1 < argc)
        {
            if (!parsePositiveDouble(argv[++i], convergenceTolerance) ||
                convergenceTolerance >= 1.0)
            {
                std::cerr << "Invalid convergence tolerance\n";
                return 2;
            }
        }
        else if (argument == "--skip-convergence-check")
        {
            skipConvergenceCheck = true;
        }
        else
        {
            printUsage(argv[0]);
            return 2;
        }
    }

    if (configPath.empty() || outputPath.empty() || scatteringOrders == 0)
    {
        printUsage(argv[0]);
        return 2;
    }
    if (!skipConvergenceCheck && scatteringOrders < 3)
    {
        std::cerr << "Convergence checking requires at least three orders\n";
        return 2;
    }

    TitanConfig config;
    std::string error;
    if (!loadTitanConfig(configPath, config, error))
    {
        std::cerr << "Could not load atmosphere inputs: " << error << '\n';
        return 1;
    }
    const Spectrum solar = buildTitanSpectrum(config);
    const std::array<double, 3> rgbWavelengths{ LambdaR, LambdaG, LambdaB };
    const AtmosphereParameters atmosphere =
        buildTitanAtmosphere(config, solar, rgbWavelengths);
    const bool luminanceMode =
        valueMode == BrunetonLutValueMode::PrecomputedLuminance;
    std::cout << "Baking "
              << (luminanceMode ? "spectral luminance" : "RGB")
              << " atmosphere with " << scatteringOrders
              << " scattering orders\n";
    PrecomputedTextures antepreviousTextures;
    PrecomputedTextures previousTextures;
    const PrecomputedTextures textures =
        precomputeAtmosphere(config,
                             solar,
                             valueMode,
                             wavelengthCount,
                             scatteringOrders,
                             skipConvergenceCheck
                                 ? nullptr
                                 : &previousTextures,
                             skipConvergenceCheck
                                 ? nullptr
                                 : &antepreviousTextures);
    if (!skipConvergenceCheck &&
        !validateConvergence(antepreviousTextures,
                             previousTextures,
                             textures,
                             convergenceTolerance))
        return 1;
    if (skipConvergenceCheck)
        std::cout << "Warning: convergence check was explicitly skipped\n";
    BrunetonAtmosphereData data =
        makeFileData(atmosphere,
                     solar,
                     textures,
                     valueMode,
                     !luminanceMode);
    if (!validateHalfPrecisionRange(data))
        return 1;

    std::ofstream output(outputPath, std::ios::binary);
    if (!output)
    {
        std::cerr << "Could not open output file: " << outputPath << '\n';
        return 1;
    }

    if (!celestia::engine::SaveBrunetonAtmosphere(output, data, error))
    {
        std::cerr << "Could not write atmosphere: " << error << '\n';
        return 1;
    }
    output.close();
    if (!output)
    {
        std::cerr << "Could not finish writing atmosphere\n";
        return 1;
    }
    if (!validateOutput(outputPath))
        return 1;

    std::cout << "Wrote " << outputPath << '\n';
    return 0;
}
