// Copyright (C) 2026, the Celestia Development Team
//
// Portable on-disk container for precomputed Bruneton atmosphere data.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace celestia::engine
{

inline constexpr std::uint32_t BrunetonTransmittanceWidth = 256;
inline constexpr std::uint32_t BrunetonTransmittanceHeight = 64;
inline constexpr std::uint32_t BrunetonScatteringRSize = 32;
inline constexpr std::uint32_t BrunetonScatteringMuSize = 128;
inline constexpr std::uint32_t BrunetonScatteringMuSSize = 32;
inline constexpr std::uint32_t BrunetonScatteringNuSize = 8;
inline constexpr std::uint32_t BrunetonScatteringWidth =
    BrunetonScatteringMuSSize * BrunetonScatteringNuSize;
inline constexpr std::uint32_t BrunetonScatteringHeight = BrunetonScatteringMuSize;
inline constexpr std::uint32_t BrunetonScatteringDepth = BrunetonScatteringRSize;
inline constexpr std::uint32_t BrunetonIrradianceWidth = 64;
inline constexpr std::uint32_t BrunetonIrradianceHeight = 16;

struct BrunetonDensityProfileLayer
{
    float width{ 0.0f };
    float expTerm{ 0.0f };
    float expScale{ 0.0f };
    float linearTerm{ 0.0f };
    float constantTerm{ 0.0f };
};

enum class BrunetonLutValueMode
{
    Radiance,
    PrecomputedLuminance,
};

struct BrunetonAtmosphereParameters
{
    std::array<float, 3> solarIrradiance{};
    float sunAngularRadius{ 0.0f };
    float bottomRadius{ 0.0f };
    float topRadius{ 0.0f };
    std::array<BrunetonDensityProfileLayer, 2> rayleighDensity{};
    std::array<float, 3> rayleighScattering{};
    std::array<BrunetonDensityProfileLayer, 2> mieDensity{};
    std::array<float, 3> mieScattering{};
    std::array<float, 3> mieExtinction{};
    float miePhaseFunctionG{ 0.0f };
    std::array<BrunetonDensityProfileLayer, 2> absorptionDensity{};
    std::array<float, 3> absorptionExtinction{};
    std::array<float, 3> groundAlbedo{};
    float muSMin{ 0.0f };
    std::array<float, 3> skySpectralRadianceToLuminance{};
    std::array<float, 3> sunSpectralRadianceToLuminance{};
    bool combinedScattering{ true };
    BrunetonLutValueMode valueMode{ BrunetonLutValueMode::Radiance };
};

struct BrunetonTextureData
{
    std::uint32_t width{ 0 };
    std::uint32_t height{ 0 };
    std::uint32_t depth{ 0 };
    std::vector<float> texels;
};

struct BrunetonAtmosphereData
{
    BrunetonAtmosphereParameters parameters;
    BrunetonTextureData transmittance;
    BrunetonTextureData scattering;
    BrunetonTextureData singleMie;
    BrunetonTextureData irradiance;
};

// The format is intentionally unversioned while unreleased. Incompatible
// changes must update every writer and loader together.
bool SaveBrunetonAtmosphere(std::ostream& output,
                            const BrunetonAtmosphereData& data,
                            std::string& error);
bool LoadBrunetonAtmosphere(std::istream& input,
                            BrunetonAtmosphereData& data,
                            std::string& error);
bool LoadBrunetonAtmosphere(const std::filesystem::path& path,
                            BrunetonAtmosphereData& data,
                            std::string& error);

} // namespace celestia::engine
