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
using celestia::engine::BrunetonScatteringMuSSize;
using celestia::engine::BrunetonScatteringNuSize;
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
makeData()
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
    data.phase = makeTexture(181, 2, 1, 0.125f);
    data.transmittance = makeTexture(
        BrunetonTransmittanceWidth, BrunetonTransmittanceHeight, 1, 0.25f);
    data.scattering = makeTexture(
        BrunetonScatteringWidth, BrunetonScatteringHeight, BrunetonScatteringDepth, 0.5f);
    data.irradiance = makeTexture(
        BrunetonIrradianceWidth, BrunetonIrradianceHeight, 1, 0.75f);
    data.singleMie = makeTexture(
        BrunetonScatteringWidth, BrunetonScatteringHeight,
        BrunetonScatteringDepth, 1.0f);
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

TEST_CASE("Bruneton atmosphere file round trips extended data")
{
    const auto source = makeData();
    const std::string bytes = save(source);

    CHECK(bytes.substr(0, 8) == std::string("CELATM\r\n", 8));
    CHECK(getU32(bytes, 24 + 48 + 4) == 2); // phase: RGBA32F
    CHECK(getU32(bytes, 24 + 2 * 48 + 4) == 2); // transmittance: RGBA32F
    CHECK(getU32(bytes, 24 + 3 * 48 + 4) == 3); // scattering: RGBA16F
    CHECK(getU32(bytes, 24 + 4 * 48 + 4) == 3); // single Mie: RGBA16F

    std::istringstream input(bytes, std::ios::binary);
    BrunetonAtmosphereData loaded;
    std::string error;
    REQUIRE(LoadBrunetonAtmosphere(input, loaded, error));
    CHECK(error.empty());
    CHECK(loaded.parameters.solarIrradiance[0] == doctest::Approx(1.0f));
    CHECK(loaded.parameters.rayleighDensity[1].expScale == doctest::Approx(-0.125f));
    CHECK(loaded.parameters.absorptionDensity[0].width == doctest::Approx(25.0f));
    CHECK(loaded.parameters.scatteringNuSize == BrunetonScatteringNuSize);
    CHECK(loaded.parameters.scatteringMuSSize == BrunetonScatteringMuSSize);
    CHECK(loaded.phase.texels.front() == doctest::Approx(0.125f));
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

TEST_CASE("Bruneton atmosphere file supports variable LUT dimensions")
{
    auto source = makeData();
    source.parameters.scatteringNuSize = 16;
    source.parameters.scatteringMuSSize = 4;
    source.phase = makeTexture(1024, 2, 1, 0.125f);
    source.transmittance = makeTexture(1024, 512, 1, 0.25f);
    source.scattering = makeTexture(64, 96, 16, 0.5f);
    source.singleMie = makeTexture(64, 96, 16, 1.0f);
    source.irradiance = makeTexture(128, 32, 1, 0.75f);
    const std::string bytes = save(source);

    std::istringstream input(bytes, std::ios::binary);
    BrunetonAtmosphereData loaded;
    std::string error;
    REQUIRE(LoadBrunetonAtmosphere(input, loaded, error));
    CHECK(loaded.parameters.scatteringNuSize == 16);
    CHECK(loaded.parameters.scatteringMuSSize == 4);
    CHECK(loaded.phase.width == 1024);
    CHECK(loaded.transmittance.width == 1024);
    CHECK(loaded.scattering.height == 96);
    CHECK(loaded.scattering.depth == 16);
    CHECK(loaded.singleMie.texels.front() == doctest::Approx(1.0f));
}

TEST_CASE("Bruneton atmosphere file preserves precomputed luminance mode")
{
    auto source = makeData();
    source.parameters.valueMode = BrunetonLutValueMode::PrecomputedLuminance;
    const std::string bytes = save(source);

    CHECK(getU32(bytes, 320) == 1);

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

TEST_CASE("Bruneton atmosphere file rejects invalid scattering packing")
{
    auto source = makeData();
    source.parameters.scatteringNuSize = 7;
    std::string error;
    std::ostringstream output(std::ios::binary);
    CHECK_FALSE(SaveBrunetonAtmosphere(output, source, error));
    CHECK(error == "scattering texture packing is invalid");
}

TEST_CASE("Bruneton atmosphere file requires tabulated phase functions")
{
    auto source = makeData();
    source.phase.height = 1;
    source.phase.texels.resize(source.phase.width * 4);
    std::string error;
    std::ostringstream output(std::ios::binary);
    CHECK_FALSE(SaveBrunetonAtmosphere(output, source, error));
    CHECK(error == "phase texture must contain two angle-sampled rows");
}

TEST_CASE("Bruneton atmosphere file requires separate aerosol scattering")
{
    auto source = makeData();
    source.singleMie = {};
    std::string error;
    std::ostringstream output(std::ios::binary);
    CHECK_FALSE(SaveBrunetonAtmosphere(output, source, error));
    CHECK(error == "texture dimensions must be nonzero");
}

TEST_CASE("Bruneton atmosphere file rejects overflowing texture dimensions")
{
    auto source = makeData();
    source.parameters.scatteringNuSize = 2;
    source.parameters.scatteringMuSSize = 1u << 30;
    source.scattering.width = 1u << 31;
    source.scattering.height = 1u << 31;
    source.scattering.depth = 4;
    source.scattering.texels.clear();
    source.singleMie = source.scattering;
    std::string error;
    std::ostringstream output(std::ios::binary);
    CHECK_FALSE(SaveBrunetonAtmosphere(output, source, error));
    CHECK(error == "texture dimensions are too large");
}

TEST_CASE("Bruneton atmosphere file rejects excessive texture allocations")
{
    auto source = makeData();
    source.transmittance.width = 16384;
    source.transmittance.height = 8192;
    source.transmittance.texels.clear();
    std::string error;
    std::ostringstream output(std::ios::binary);
    CHECK_FALSE(SaveBrunetonAtmosphere(output, source, error));
    CHECK(error == "texture dimensions are too large");
}

TEST_CASE("Bruneton atmosphere file reports missing files")
{
    BrunetonAtmosphereData loaded;
    std::string error;
    CHECK_FALSE(LoadBrunetonAtmosphere(
        std::filesystem::path("definitely-missing-bruneton-atmosphere.atm"),
        loaded,
        error));
    CHECK(error == "could not open atmosphere file");
}
