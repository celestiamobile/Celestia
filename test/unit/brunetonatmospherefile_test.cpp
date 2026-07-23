#include <cstdint>
#include <sstream>
#include <string>

#include <celengine/brunetonatmospherefile.h>

#include <doctest.h>

using celestia::engine::BrunetonAtmosphereData;
using celestia::engine::BrunetonAtmosphereParameters;
using celestia::engine::BrunetonDensityProfileLayer;
using celestia::engine::BrunetonIrradianceHeight;
using celestia::engine::BrunetonIrradianceWidth;
using celestia::engine::BrunetonLutValueMode;
using celestia::engine::BrunetonScatteringDepth;
using celestia::engine::BrunetonScatteringHeight;
using celestia::engine::BrunetonScatteringWidth;
using celestia::engine::BrunetonTextureData;
using celestia::engine::BrunetonTransmittanceHeight;
using celestia::engine::BrunetonTransmittanceWidth;
using celestia::engine::LoadBrunetonAtmosphere;
using celestia::engine::SaveBrunetonAtmosphere;

namespace
{

BrunetonTextureData
makeTexture(std::uint32_t width,
            std::uint32_t height,
            std::uint32_t depth,
            float value)
{
    BrunetonTextureData texture;
    texture.width = width;
    texture.height = height;
    texture.depth = depth;
    texture.texels.assign(static_cast<std::size_t>(width) * height * depth * 4, value);
    return texture;
}

BrunetonAtmosphereData
makeData(bool combined = true)
{
    BrunetonAtmosphereData data;
    auto& p = data.parameters;
    p.solarIrradiance = { 1.0f, 2.0f, 3.0f };
    p.sunAngularRadius = 0.004675f;
    p.bottomRadius = 6360.0f;
    p.topRadius = 6420.0f;
    p.rayleighDensity[1] = BrunetonDensityProfileLayer{
        0.0f, 1.0f, -0.125f, 0.0f, 0.0f,
    };
    p.rayleighScattering = { 0.0058f, 0.0135f, 0.0331f };
    p.mieDensity[1] = BrunetonDensityProfileLayer{
        0.0f, 1.0f, -0.8333333f, 0.0f, 0.0f,
    };
    p.mieScattering = { 0.003f, 0.003f, 0.003f };
    p.mieExtinction = { 0.003333f, 0.003333f, 0.003333f };
    p.miePhaseFunctionG = 0.8f;
    p.absorptionDensity[0] = BrunetonDensityProfileLayer{
        25.0f, 0.0f, 0.0f, 0.0666667f, -0.666667f,
    };
    p.absorptionDensity[1] = BrunetonDensityProfileLayer{
        0.0f, 0.0f, 0.0f, -0.0666667f, 2.66667f,
    };
    p.absorptionExtinction = { 0.00065f, 0.00188f, 0.000085f };
    p.groundAlbedo = { 0.1f, 0.1f, 0.1f };
    p.muSMin = -0.207912f;
    p.skySpectralRadianceToLuminance = { 114974.0f, 71305.0f, 65310.0f };
    p.sunSpectralRadianceToLuminance = { 98242.0f, 69954.0f, 66475.0f };
    p.combinedScattering = combined;

    data.transmittance = makeTexture(
        BrunetonTransmittanceWidth, BrunetonTransmittanceHeight, 1, 0.25f);
    data.scattering = makeTexture(
        BrunetonScatteringWidth, BrunetonScatteringHeight, BrunetonScatteringDepth, 0.5f);
    data.irradiance = makeTexture(
        BrunetonIrradianceWidth, BrunetonIrradianceHeight, 1, 0.75f);
    if (!combined)
    {
        data.singleMie = makeTexture(
            BrunetonScatteringWidth, BrunetonScatteringHeight,
            BrunetonScatteringDepth, 1.0f);
    }
    return data;
}

std::string
save(const BrunetonAtmosphereData& data)
{
    std::ostringstream output(std::ios::binary);
    std::string error;
    REQUIRE(SaveBrunetonAtmosphere(output, data, error));
    REQUIRE(error.empty());
    return output.str();
}

void
putU32(std::string& bytes, std::size_t offset, std::uint32_t value)
{
    REQUIRE(offset + 4 <= bytes.size());
    bytes[offset] = static_cast<char>(value);
    bytes[offset + 1] = static_cast<char>(value >> 8);
    bytes[offset + 2] = static_cast<char>(value >> 16);
    bytes[offset + 3] = static_cast<char>(value >> 24);
}

std::uint32_t
getU32(const std::string& bytes, std::size_t offset)
{
    REQUIRE(offset + 4 <= bytes.size());
    return static_cast<std::uint8_t>(bytes[offset]) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 1])) << 8 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 2])) << 16 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 3])) << 24;
}

} // namespace

TEST_CASE("Bruneton atmosphere file round trips combined data")
{
    const auto source = makeData();
    const std::string bytes = save(source);

    CHECK(bytes.substr(0, 8) == std::string("CELATM\r\n", 8));
    CHECK(bytes.size() == 8667616);
    CHECK(getU32(bytes, 24 + 48 + 4) == 2); // transmittance: RGBA32F
    CHECK(getU32(bytes, 24 + 2 * 48 + 4) == 3); // scattering: RGBA16F

    std::istringstream input(bytes, std::ios::binary);
    BrunetonAtmosphereData loaded;
    std::string error;
    REQUIRE(LoadBrunetonAtmosphere(input, loaded, error));
    CHECK(error.empty());
    CHECK(loaded.parameters.solarIrradiance[0] == doctest::Approx(1.0f));
    CHECK(loaded.parameters.rayleighDensity[1].expScale == doctest::Approx(-0.125f));
    CHECK(loaded.parameters.absorptionDensity[0].width == doctest::Approx(25.0f));
    CHECK(loaded.parameters.combinedScattering);
    CHECK(loaded.singleMie.texels.empty());
    CHECK(loaded.transmittance.texels.front() == doctest::Approx(0.25f));
    CHECK(loaded.scattering.texels.back() == doctest::Approx(0.5f));
    CHECK(loaded.irradiance.texels.front() == doctest::Approx(0.75f));
}

TEST_CASE("Bruneton atmosphere file quantizes scattering to binary16")
{
    auto source = makeData();
    source.scattering.texels.front() = 0.1f;
    const std::string bytes = save(source);

    std::istringstream input(bytes, std::ios::binary);
    BrunetonAtmosphereData loaded;
    std::string error;
    REQUIRE(LoadBrunetonAtmosphere(input, loaded, error));
    CHECK(loaded.scattering.texels.front() == doctest::Approx(0.0999755859375f));
}

TEST_CASE("Bruneton atmosphere file round trips separate Mie data")
{
    const std::string bytes = save(makeData(false));
    CHECK(bytes.size() == 17056272);

    std::istringstream input(bytes, std::ios::binary);
    BrunetonAtmosphereData loaded;
    std::string error;
    REQUIRE(LoadBrunetonAtmosphere(input, loaded, error));
    CHECK_FALSE(loaded.parameters.combinedScattering);
    CHECK(loaded.singleMie.texels.front() == doctest::Approx(1.0f));
}

TEST_CASE("Bruneton atmosphere file preserves precomputed luminance mode")
{
    auto source = makeData();
    source.parameters.valueMode = BrunetonLutValueMode::PrecomputedLuminance;
    const std::string bytes = save(source);

    // Combined scattering and precomputed luminance are both declared.
    CHECK(getU32(bytes, 224) == 3);

    std::istringstream input(bytes, std::ios::binary);
    BrunetonAtmosphereData loaded;
    std::string error;
    REQUIRE(LoadBrunetonAtmosphere(input, loaded, error));
    CHECK(loaded.parameters.valueMode == BrunetonLutValueMode::PrecomputedLuminance);
}

TEST_CASE("Bruneton atmosphere file rejects duplicate sections")
{
    std::string bytes = save(makeData());
    putU32(bytes, 24 + 48, 1);
    putU32(bytes, 24 + 48 + 4, 1);
    putU32(bytes, 24 + 48 + 8, 0);
    putU32(bytes, 24 + 48 + 12, 0);
    putU32(bytes, 24 + 48 + 16, 0);
    putU32(bytes, 24 + 48 + 20, 0);
    putU32(bytes, 24 + 48 + 32, 256);

    std::istringstream input(bytes, std::ios::binary);
    BrunetonAtmosphereData loaded;
    std::string error;
    CHECK_FALSE(LoadBrunetonAtmosphere(input, loaded, error));
    CHECK(error == "duplicate atmosphere section");
}

TEST_CASE("Bruneton atmosphere file rejects truncated payloads")
{
    std::string bytes = save(makeData());
    bytes.resize(bytes.size() - 1);

    std::istringstream input(bytes, std::ios::binary);
    BrunetonAtmosphereData loaded;
    std::string error;
    CHECK_FALSE(LoadBrunetonAtmosphere(input, loaded, error));
    CHECK(error == "atmosphere section is outside the file");
}

TEST_CASE("Bruneton atmosphere file rejects inconsistent combined scattering")
{
    std::string bytes = save(makeData());
    // Parameter payload starts at 224 for a four-entry directory.
    putU32(bytes, 224, 0);

    std::istringstream input(bytes, std::ios::binary);
    BrunetonAtmosphereData loaded;
    std::string error;
    CHECK_FALSE(LoadBrunetonAtmosphere(input, loaded, error));
    CHECK(error == "single-Mie section is inconsistent with combined scattering");
}
