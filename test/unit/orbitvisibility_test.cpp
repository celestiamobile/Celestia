#include <array>
#include <celengine/orbitvisibility.h>
#include <cmath>
#include <doctest.h>

using celestia::engine::OrbitVisibilityLayer;
using celestia::engine::OrbitVisibilityScratch;
using celestia::engine::splitOrbitSegment;

namespace
{

OrbitVisibilityLayer
shell(double radius, bool volume)
{
    return { Eigen::Scaling(1.0 / radius) * Eigen::Translation3d(0.0, 0.0, 10.0), volume };
}

} // namespace

TEST_CASE("Orbit visibility separates foreground from background cloud segments")
{
    std::array layers{ shell(2.0, false) };
    OrbitVisibilityScratch scratch;
    auto segments = splitOrbitSegment({ 0.0, 0.0, -6.0 }, { 0.0, 0.0, -14.0 }, layers, scratch);
    REQUIRE(segments.size() == 2);
    CHECK(segments[0].stage == 1);
    CHECK(segments[0].end == doctest::Approx(0.25));
    CHECK(segments[1].stage == 0);
    CHECK(segments[1].begin == segments[0].end);
}

TEST_CASE("Orbit visibility finds atmosphere crossings between exterior endpoints")
{
    std::array layers{ shell(2.0, true) };
    OrbitVisibilityScratch scratch;
    auto segments = splitOrbitSegment({ -3.0, 0.0, -10.0 }, { 3.0, 0.0, -10.0 }, layers, scratch);
    REQUIRE(segments.size() == 3);
    CHECK(segments[0].stage == 0);
    CHECK(segments[0].end == doctest::Approx(1.0 / 6.0));
    CHECK(segments[1].stage == 1);
    CHECK(segments[1].end == doctest::Approx(5.0 / 6.0));
    CHECK(segments[2].stage == 0);
}

TEST_CASE("Orbit visibility splits at the projected limb as well as the shell")
{
    std::array layers{ shell(2.0, false) };
    OrbitVisibilityScratch scratch;
    auto segments = splitOrbitSegment({ 0.0, 0.0, -7.0 }, { 4.0, 0.0, -7.0 }, layers, scratch);
    REQUIRE(segments.size() == 2);
    CHECK(segments[0].stage == 1);
    CHECK(segments[0].end == doctest::Approx(7.0 / (4.0 * std::sqrt(24.0))));
    CHECK(segments[1].stage == 0);
}

TEST_CASE("Orbits inside an atmosphere retain foreground cloud occlusion")
{
    std::array layers{ shell(3.0, true), shell(2.0, false) };
    OrbitVisibilityScratch scratch;
    auto aboveClouds = splitOrbitSegment({ 0.0, 0.0, -7.1 }, { 0.0, 0.0, -7.9 }, layers, scratch);
    REQUIRE(aboveClouds.size() == 1);
    CHECK(aboveClouds[0].stage == 2);

    auto belowClouds = splitOrbitSegment({ 0.0, 0.0, -8.1 }, { 0.0, 0.0, -9.0 }, layers, scratch);
    REQUIRE(belowClouds.size() == 1);
    CHECK(belowClouds[0].stage == 1);
}

TEST_CASE("Orbit visibility handles an observer inside the cloud shell")
{
    std::array layers{ OrbitVisibilityLayer{ Eigen::Affine3d::Identity(), false } };
    OrbitVisibilityScratch scratch;
    auto segments = splitOrbitSegment({ 0.0, 0.0, 0.5 }, { 0.0, 0.0, 1.5 }, layers, scratch);
    REQUIRE(segments.size() == 2);
    CHECK(segments[0].stage == 1);
    CHECK(segments[0].end == doctest::Approx(0.5));
    CHECK(segments[1].stage == 0);
}

TEST_CASE("Orbit visibility respects ellipsoids and ignores unrelated shells")
{
    auto layer = shell(2.0, false);
    layer.toUnitSphere = Eigen::Scaling(1.0, 2.0, 1.0) * layer.toUnitSphere;
    std::array layers{ layer,
                       OrbitVisibilityLayer{ Eigen::Translation3d(100.0, 0.0, 0.0)
                                                 * Eigen::Affine3d::Identity(),
                                             false } };
    OrbitVisibilityScratch scratch;
    auto segments = splitOrbitSegment({ 0.0, 0.0, -7.0 }, { 0.0, 2.0, -7.0 }, layers, scratch);
    REQUIRE(segments.size() == 2);
    CHECK(segments[0].stage == 1);
    CHECK(segments[1].stage == 0);
    CHECK(segments[0].end == doctest::Approx(7.0 / (4.0 * std::sqrt(24.0))));
}

TEST_CASE("Orbit visibility reuses scratch storage across crossing and unrelated segments")
{
    std::array layers{ shell(3.0, true), shell(2.0, false) };
    OrbitVisibilityScratch scratch;
    auto crossing = splitOrbitSegment({ -4.0, 0.0, -10.0 }, { 4.0, 0.0, -10.0 }, layers, scratch);
    REQUIRE(crossing.size() > 1);
    const auto* cuts = scratch.cuts.data();
    const auto* segments = scratch.segments.data();

    for (int i = 0; i < 10; ++i)
    {
        auto unrelated = splitOrbitSegment({ 10.0, 0.0, -10.0 }, { 11.0, 0.0, -10.0 }, layers, scratch);
        REQUIRE(unrelated.size() == 1);
        CHECK(unrelated[0].begin == 0.0);
        CHECK(unrelated[0].end == 1.0);
        CHECK(unrelated[0].stage == 0);

        crossing = splitOrbitSegment({ -4.0, 0.0, -10.0 }, { 4.0, 0.0, -10.0 }, layers, scratch);
        REQUIRE(crossing.size() > 1);
        CHECK(scratch.cuts.data() == cuts);
        CHECK(scratch.segments.data() == segments);
    }

    auto empty = splitOrbitSegment({ -4.0, 0.0, -10.0 }, { 4.0, 0.0, -10.0 }, {}, scratch);
    REQUIRE(empty.size() == 1);
    CHECK(empty[0].begin == 0.0);
    CHECK(empty[0].end == 1.0);
    CHECK(empty[0].stage == 0);
}
