// Copyright (C) 2026-present, the Celestia Development Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>

#include <doctest.h>

#include <celengine/body.h>
#include <celengine/ringscattering.h>
#include <celengine/shadermanager.h>
#include <celengine/texmanager.h>
#include <celutil/associativearray.h>
#include <celutil/parser.h>
#include <celutil/tokenizer.h>

using celestia::engine::ReadRingScattering;
using celestia::engine::RingScattering;
using celestia::engine::TexturePaths;
using celestia::util::AssociativeArray;
using celestia::util::Parser;
using celestia::util::Tokenizer;
using celestia::util::Value;

namespace
{

bool readScattering(const std::string& text, RingScattering& scattering)
{
    std::istringstream stream(text);
    Tokenizer tokenizer(stream);
    Parser parser(&tokenizer);
    auto value = parser.readValue();
    const auto* data = value.getHash();
    REQUIRE(data != nullptr);
    TexturePaths paths;
    return ReadRingScattering(*data, {}, paths, scattering);
}

} // namespace

TEST_SUITE_BEGIN("Ring scattering");

TEST_CASE("Physical ring optics are opt-in and empty slabs are transparent")
{
    RingSystem rings(100.0f, 200.0f);
    CHECK_FALSE(rings.scattering.has_value());
    CHECK(rings.opacityTexture() == rings.texture);
    rings.scattering.emplace();
    CHECK(rings.scattering->opticalDepth == 0.0f);
    CHECK(rings.scattering->albedo == Eigen::Vector3f::Ones());
    CHECK(rings.scattering->asymmetry == 0.0f);
    CHECK(rings.opacityTexture() == rings.scattering->opticalDepthTexture);
}

TEST_CASE("Physical optical depth is not limited to an opacity")
{
    RingScattering scattering;
    REQUIRE(readScattering(R"({
        OpticalDepth 3.5
        SingleScatteringAlbedo [0.0001 0.6 1]
        PhaseAsymmetry 0.8
    })", scattering));
    CHECK(scattering.opticalDepth == doctest::Approx(3.5));
    CHECK(scattering.albedo.x() == doctest::Approx(0.0001));
    CHECK(scattering.albedo.y() == doctest::Approx(0.6));
    CHECK(scattering.albedo.z() == 1.0f);
    CHECK(scattering.asymmetry == doctest::Approx(0.8));
}

TEST_CASE("Partial modifications preserve other ring optical properties")
{
    RingScattering scattering;
    scattering.opticalDepth = 0.2f;
    scattering.albedo = Eigen::Vector3f::Constant(0.7f);
    scattering.albedoTexture = celestia::util::TextureHandle{ 0 };
    REQUIRE(readScattering("{ PhaseAsymmetry -0.3 }", scattering));
    CHECK(scattering.opticalDepth == doctest::Approx(0.2));
    CHECK(scattering.albedo == Eigen::Vector3f::Constant(0.7f));
    CHECK(scattering.asymmetry == doctest::Approx(-0.3));
    CHECK(scattering.albedoTexture == celestia::util::TextureHandle{ 0 });
}

TEST_CASE("Invalid physical properties reject the entire update")
{
    for (const auto* text : {
        "{ OpticalDepth -1 }",
        "{ OpticalDepth \"0.1\" }",
        "{ OpticalDepth 1e100 }",
        "{ OpticalDepth 0.8 PhaseAsymmetry 1 }",
        "{ PhaseAsymmetry -1 }",
        "{ PhaseAsymmetry \"forward\" }",
        "{ SingleScatteringAlbedo [1.1 0.5 0.5] }",
        "{ SingleScatteringAlbedo [-0.1 0.5 0.5] }",
        "{ SingleScatteringAlbedo [0.5 0.5] }",
        "{ SingleScatteringAlbedo [0.5 1e100 0.5] }",
        "{ OpticalDepthTexture 42 }",
        "{ PhaseFunctionTexture false }",
        "{ SingleScatteringAlbedoTexture 42 }",
        "{ SingleScatteringAlbedoTexture \"nonexistent-ring-albedo.png\" }",
        "{ OpticalDepthTexture \"nonexistent-ring-optical-depth.png\" }",
    })
    {
        CAPTURE(text);
        RingScattering scattering;
        scattering.opticalDepth = 0.2f;
        scattering.albedoTexture = celestia::util::TextureHandle{ 0 };
        CHECK_FALSE(readScattering(text, scattering));
        CHECK(scattering.opticalDepth == doctest::Approx(0.2));
        CHECK(scattering.albedo == Eigen::Vector3f::Ones());
        CHECK(scattering.asymmetry == 0.0f);
        CHECK(scattering.albedoTexture == celestia::util::TextureHandle{ 0 });
    }
}

TEST_CASE("Nonfinite optical depth and asymmetry are rejected")
{
    TexturePaths paths;
    for (const auto* key : { "OpticalDepth", "PhaseAsymmetry" })
    {
        for (double value : { std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN() })
        {
            AssociativeArray data;
            data.addValue(std::string(key), Value(value));
            RingScattering scattering;
            CHECK_FALSE(ReadRingScattering(data, {}, paths, scattering));
        }
    }
}

TEST_CASE("Empty filenames remove optical maps")
{
    RingScattering scattering;
    scattering.albedoTexture = celestia::util::TextureHandle{ 0 };
    REQUIRE(readScattering(R"({ OpticalDepthTexture "" PhaseFunctionTexture ""
                               SingleScatteringAlbedoTexture "" })", scattering));
    CHECK(scattering.opticalDepthTexture == celestia::util::TextureHandle::Invalid);
    CHECK(scattering.phaseTexture == celestia::util::TextureHandle::Invalid);
    CHECK(scattering.albedoTexture == celestia::util::TextureHandle::Invalid);
}

TEST_CASE("Legacy and physical shaders have distinct cache keys")
{
    ShaderProperties legacy;
    legacy.lightModel = LightingModel::RingIllumModel;
    ShaderProperties physical = legacy;
    physical.physicalRings = true;
    ShaderProperties textured = physical;
    textured.texUsage |= TexUsage::RingOpticalDepthTexture;
    CHECK(physical != legacy);
    CHECK(textured != physical);
    ShaderProperties albedoTextured = physical;
    albedoTextured.texUsage |= TexUsage::RingAlbedoTexture;
    CHECK(albedoTextured != physical);
    CHECK(albedoTextured != textured);
    std::unordered_set<ShaderProperties> keys{ legacy, physical, textured, albedoTextured };
    CHECK(keys.size() == 4);
}

TEST_SUITE_END();
