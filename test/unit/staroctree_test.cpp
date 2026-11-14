// Copyright (C) 2026 Celestia Development Team. All rights reserved.
// Licensed under the GNU General Public License, version 2 or later.

#include <array>
#include <limits>

#include <doctest.h>

#include <celastro/astro.h>
#include <celengine/staroctree.h>

namespace
{

struct Handler : celestia::engine::StarHandler
{
    unsigned int count{};
    float irradiance{};
    void process(const Star&, float, float brightness) override
    {
        ++count;
        irradiance = brightness;
    }
};

struct Fixture
{
    using Processor = celestia::engine::StarOctreeVisibleObjectsProcessor;
    std::array<Processor::PlaneType, 5> planes{
        Processor::PlaneType(Eigen::Vector3f::UnitZ(), 1000000.0f),
        Processor::PlaneType(Eigen::Vector3f::UnitZ(), 1000000.0f),
        Processor::PlaneType(Eigen::Vector3f::UnitZ(), 1000000.0f),
        Processor::PlaneType(Eigen::Vector3f::UnitZ(), 1000000.0f),
        Processor::PlaneType(Eigen::Vector3f::UnitZ(), 1000000.0f),
    };
    Handler handler;
    const float minimumIrradiance{ celestia::astro::magToIrradiance(4.0f) };
    Processor processor{ &handler, Eigen::Vector3f::Zero(), planes, minimumIrradiance };
};

} // namespace

TEST_SUITE_BEGIN("Star culling");

TEST_CASE("Magnitude prefilter retains stars rounded onto the visibility boundary")
{
    Fixture fixture;
    Star star;
    star.setPosition(Eigen::Vector3f(celestia::astro::LY_PER_PARSEC<float>, 0, 0));
    // At one parsec, this positive magnitude rounds onto -5 even with excess precision.
    star.setAbsoluteMagnitude(std::numeric_limits<float>::min());
    float distance = star.getPosition().norm();
    float minimumIrradiance = celestia::astro::absMagToIrradiance(
        0.0f, celestia::astro::lightYearsToKilometers(distance));
    Fixture::Processor processor{
        &fixture.handler, Eigen::Vector3f::Zero(), fixture.planes, minimumIrradiance };
    REQUIRE(star.getApparentMagnitude(star.getPosition().norm()) == -5.0f);
    REQUIRE(star.getApparentIrradiance(distance) == minimumIrradiance);
    REQUIRE(processor.checkNode(star.getPosition(), 0.0f, 0.0f));
    processor.process(star);
    CHECK(fixture.handler.count == 1);
    CHECK(fixture.handler.irradiance == minimumIrradiance);
    star.setAbsoluteMagnitude(100.0f);
    processor.process(star);
    CHECK(fixture.handler.count == 1);
}

TEST_CASE("Magnitude prefilter does not discard extinction-brightened stars")
{
    for (float distance : { 0.5f, 10.0f })
    {
        CAPTURE(distance);
        Fixture fixture;
        Star star;
        star.setPosition(Eigen::Vector3f(distance, 0, 0));
        star.setAbsoluteMagnitude(20.0f);
        star.setExtinction(-100.0f);
        REQUIRE(fixture.processor.checkNode(star.getPosition(), 0.01f, 0.0f));
        float irradiance = star.getApparentIrradiance(distance);
        REQUIRE(irradiance >= fixture.minimumIrradiance);
        fixture.processor.process(star);
        CHECK(fixture.handler.count == 1);
        CHECK(fixture.handler.irradiance == irradiance);
    }
}

TEST_SUITE_END();
