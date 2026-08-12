#include <celastro/date.h>

#include <doctest.h>

namespace astro = celestia::astro;

TEST_SUITE_BEGIN("Astronomical time");

TEST_CASE("Historical Delta T estimates")
{
    CHECK(astro::estimateDeltaT(-479.0) == doctest::Approx(16830.4).epsilon(0.0001));
    CHECK(astro::estimateDeltaT(479.0) == doctest::Approx(5915.88).epsilon(0.0001));
    CHECK(astro::estimateDeltaT(2000.0) == doctest::Approx(63.86));
}

TEST_SUITE_END();
