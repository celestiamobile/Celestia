// Copyright (C) 2026, the Celestia Development Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <limits>
#include <string>

#include <doctest.h>

#include "../../src/tools/atmosphere/brunetonbaker.h"

using celestia::tools::BrunetonBakeSettings;
using celestia::tools::MakeAnalyticEarthPhaseTexture;
using celestia::tools::MakePhysicalEarthParameters;
using celestia::tools::ValidateBrunetonBakeSettings;

TEST_CASE("Bruneton baker validates configuration")
{
    BrunetonBakeSettings settings;
    std::string error;
    CHECK(ValidateBrunetonBakeSettings(settings, error));

    settings.scatteringOrders = 0;
    CHECK_FALSE(ValidateBrunetonBakeSettings(settings, error));
    CHECK(error == "scattering orders must be between 1 and 16");

    settings = {};
    settings.topRadiusKm = settings.bottomRadiusKm;
    CHECK_FALSE(ValidateBrunetonBakeSettings(settings, error));
    CHECK(error == "invalid atmosphere radii");

    settings = {};
    settings.phaseSampleCount = 1;
    CHECK_FALSE(ValidateBrunetonBakeSettings(settings, error));
    CHECK(error == "phase sample count must be between 2 and 65536");

    settings = {};
    settings.threadCount = 65;
    CHECK_FALSE(ValidateBrunetonBakeSettings(settings, error));
    CHECK(error == "thread count must be between 0 and 64");

    settings = {};
    settings.bottomRadiusKm = 1.0e39;
    settings.topRadiusKm = 2.0e39;
    CHECK_FALSE(ValidateBrunetonBakeSettings(settings, error));
    CHECK(error == "invalid atmosphere radii");

    settings = {};
    settings.topRadiusKm = std::nextafter(
        settings.bottomRadiusKm, std::numeric_limits<double>::infinity());
    CHECK_FALSE(ValidateBrunetonBakeSettings(settings, error));
    CHECK(error == "invalid atmosphere radii");
}

TEST_CASE("Bruneton baker physical Earth preset matches reference")
{
    const auto parameters = MakePhysicalEarthParameters({});
    CHECK(parameters.bottomRadius == doctest::Approx(6378.1366f));
    CHECK(parameters.topRadius == doctest::Approx(6478.1366f));
    CHECK(parameters.rayleighScattering[0] == doctest::Approx(0.0058023394f));
    CHECK(parameters.mieScattering[0] == doctest::Approx(0.003996f));
    CHECK(parameters.miePhaseFunctionG == doctest::Approx(0.8f));
    CHECK(parameters.muSMin == doctest::Approx(-0.20791169f));
    CHECK(parameters.scatteringNuSize == 8);
    CHECK(parameters.scatteringMuSSize == 32);
}

TEST_CASE("Bruneton baker phase rows are normalized")
{
    constexpr std::uint32_t SampleCount = 4096;
    constexpr double Pi = 3.14159265358979323846;
    const auto texture = MakeAnalyticEarthPhaseTexture(SampleCount);
    REQUIRE(texture.width == SampleCount);
    REQUIRE(texture.height == 2);
    REQUIRE(texture.depth == 1);

    for (std::uint32_t row = 0; row < 2; ++row)
    {
        double integral = 0.0;
        const double step = Pi / (SampleCount - 1);
        for (std::uint32_t i = 0; i < SampleCount; ++i)
        {
            const double theta = i * step;
            const double weight = i == 0 || i + 1 == SampleCount ? 0.5 : 1.0;
            const auto offset =
                (static_cast<std::size_t>(row) * SampleCount + i) * 4;
            CHECK(texture.texels[offset] == texture.texels[offset + 1]);
            CHECK(texture.texels[offset] == texture.texels[offset + 2]);
            CHECK(texture.texels[offset + 3] == 1.0f);
            integral += texture.texels[offset] * std::sin(theta) * weight * step;
        }
        integral *= 2.0 * Pi;
        CHECK(integral == doctest::Approx(1.0).epsilon(row == 0 ? 1.0e-5 : 2.0e-4));
    }
}
