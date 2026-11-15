// psf_test.cpp
//
// Copyright (C) 2026-present, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include <doctest.h>

#include <celengine/pointstarrenderer.h>

using celestia::engine::detail::psfGlowOnset;
using celestia::engine::detail::psfPointFade;
using celestia::engine::detail::psfReflectiveGlowOnset;

TEST_SUITE_BEGIN("PSF");

TEST_CASE("PSF soft clipping preserves its curve without overflowing finite peaks")
{
    using celestia::engine::detail::psfSoftClip;
    CHECK(psfSoftClip(5.0f, 3.0f) == doctest::Approx(4.0f));
    for (float peak : { 1.0e20f, 1.0e30f, std::numeric_limits<float>::max() })
    {
        CAPTURE(peak);
        float actual = psfSoftClip(peak, peak * 0.5f);
        double p = peak;
        double g = p * 0.5;
        CHECK(std::isfinite(actual));
        CHECK(actual == doctest::Approx(std::sqrt(p * p - g * g)));
    }
    float gate = 1.0e20f;
    float peak = std::nextafter(gate, std::numeric_limits<float>::infinity());
    double p = peak;
    double g = gate;
    CHECK(psfSoftClip(peak, gate) == doctest::Approx(std::sqrt((p - g) * (p + g))));
}

TEST_CASE("Calibrated point halos share the same onset for unresolved sources")
{
    using celestia::engine::detail::calibratedPointHaloOpacity;
    CHECK(calibratedPointHaloOpacity(0.5f, 1.0f) == 0.0f);
    CHECK(calibratedPointHaloOpacity(1.0f, 1.0f) == 0.0f);
    CHECK(calibratedPointHaloOpacity(1.5f, 1.0f) == doctest::Approx(0.5f));
    CHECK(calibratedPointHaloOpacity(2.0f, 1.0f) == 1.0f);
}

TEST_CASE("Reflective point halos vanish as the resolved surface takes over")
{
    using celestia::engine::detail::calibratedPointHaloOpacity;
    CHECK(calibratedPointHaloOpacity(2.0f, 0.75f) == doctest::Approx(0.375f));
    CHECK(calibratedPointHaloOpacity(2.0f, 0.5f) == 0.0f);
    CHECK(calibratedPointHaloOpacity(1000.0f, 0.25f) == doctest::Approx(0.25f));
    CHECK(calibratedPointHaloOpacity(1000.0f, 0.0f) == 0.0f);
    CHECK(calibratedPointHaloOpacity(std::numeric_limits<float>::infinity(), 0.0f) == 0.0f);
}

TEST_CASE("PSF glow fades in between peak radiance one and two")
{
    CHECK(psfGlowOnset(0.0f) == 0.0f);
    CHECK(psfGlowOnset(0.5f) == 0.0f);
    CHECK(psfGlowOnset(1.0f) == 0.0f);
    CHECK(psfGlowOnset(1.25f) == doctest::Approx(0.15625f));
    CHECK(psfGlowOnset(1.5f) == doctest::Approx(0.5f));
    CHECK(psfGlowOnset(1.75f) == doctest::Approx(0.84375f));
    CHECK(psfGlowOnset(2.0f) == 1.0f);
    CHECK(psfGlowOnset(100.0f) == 1.0f);
    CHECK(psfGlowOnset(std::numeric_limits<float>::max()) == 1.0f);
}

TEST_CASE("PSF glow onset is continuous at both thresholds")
{
    float aboveOne = psfGlowOnset(std::nextafter(1.0f, 2.0f));
    CHECK(aboveOne > 0.0f);
    CHECK(aboveOne < 1.0e-6f);
    CHECK(psfGlowOnset(std::nextafter(1.0f, 0.0f)) == 0.0f);

    CHECK(psfGlowOnset(std::nextafter(2.0f, 1.0f)) > 1.0f - 1.0e-6f);
    CHECK(psfGlowOnset(std::nextafter(2.0f, 3.0f)) == 1.0f);
}

TEST_CASE("PSF glow onset is bounded and monotonic")
{
    float previous = 0.0f;
    for (int i = 0; i <= 300; ++i)
    {
        float alpha = psfGlowOnset(static_cast<float>(i) / 100.0f);
        CHECK(alpha >= previous);
        CHECK(alpha >= 0.0f);
        CHECK(alpha <= 1.0f);
        previous = alpha;
    }
}

TEST_CASE("PSF point fades out as the disc resolves")
{
    CHECK(psfPointFade(0.0f, 1.5f, 1.0f) == 1.0f);
    CHECK(psfPointFade(0.5f, 1.5f, 1.0f) == 1.0f);
    CHECK(psfPointFade(1.0f, 1.5f, 1.0f) == 1.0f);
    CHECK(psfPointFade(1.25f, 1.5f, 1.0f) == doctest::Approx(0.84375f));
    CHECK(psfPointFade(1.5f, 1.5f, 1.0f) == doctest::Approx(0.5f));
    CHECK(psfPointFade(1.75f, 1.5f, 1.0f) == doctest::Approx(0.15625f));
    CHECK(psfPointFade(2.0f, 1.5f, 1.0f) == 0.0f);
    CHECK(psfPointFade(100.0f, 1.5f, 1.0f) == 0.0f);
}

TEST_CASE("PSF reflective glow fades smoothly at the limb overflow threshold")
{
    CHECK(psfReflectiveGlowOnset(10.0f, 0.0f) == 1.0f);
    for (float linkedPeak : { 0.1f, 1.0f, 10.0f, 1000.0f })
    {
        CAPTURE(linkedPeak);
        CHECK(psfReflectiveGlowOnset(0.0f, linkedPeak) == 0.0f);
        CHECK(psfReflectiveGlowOnset(linkedPeak, linkedPeak) == 0.0f);
        CHECK(psfReflectiveGlowOnset(1.5f * linkedPeak, linkedPeak) == doctest::Approx(0.5f));
        CHECK(psfReflectiveGlowOnset(2.0f * linkedPeak, linkedPeak) == 1.0f);
        CHECK(psfReflectiveGlowOnset(10.0f * linkedPeak, linkedPeak) == 1.0f);
        CHECK(psfReflectiveGlowOnset(std::nextafter(linkedPeak, 0.0f), linkedPeak) == 0.0f);
        CHECK(psfReflectiveGlowOnset(std::nextafter(linkedPeak, 2.0f * linkedPeak), linkedPeak) < 1.0e-6f);
        CHECK(psfReflectiveGlowOnset(std::nextafter(2.0f * linkedPeak, linkedPeak), linkedPeak) > 1.0f - 1.0e-6f);

        float previous = 0.0f;
        for (int i = 0; i <= 300; ++i)
        {
            float alpha = psfReflectiveGlowOnset(linkedPeak * (static_cast<float>(i) / 100.0f), linkedPeak);
            CHECK(alpha >= previous);
            CHECK(alpha >= 0.0f);
            CHECK(alpha <= 1.0f);
            previous = alpha;
        }
    }
}

TEST_CASE("PSF point transition follows the DPI-scaled cone radius")
{
    CHECK(psfPointFade(1.5f, 1.0f, 1.0f) == doctest::Approx(0.5f));
    CHECK(psfPointFade(1.5f, 1.0f, 0.5f) == doctest::Approx(0.5f));
    CHECK(psfPointFade(2.0f, 1.5f, 2.0f) == doctest::Approx(0.5f));
    CHECK(psfPointFade(2.0f, 3.0f, 1.0f) == doctest::Approx(0.5f));
    CHECK(psfPointFade(3.0f, 1.5f, 2.0f) == 0.0f);
    CHECK(psfPointFade(10.5f, 10.0f, 2.0f) == doctest::Approx(0.5f));
    CHECK(psfPointFade(20.0f, 10.0f, 2.0f) == 0.0f);
}

TEST_CASE("PSF point fade is continuous and monotonic across radii and DPI scales")
{
    for (float radius : { 1.0f, 1.5f, 10.0f })
    {
        for (float scale : { 0.5f, 1.0f, 2.0f, 4.0f })
        {
            CAPTURE(radius);
            CAPTURE(scale);
            float fadeEnd = std::max(2.0f, radius * scale);
            CHECK(psfPointFade(1.0f, radius, scale) == 1.0f);
            CHECK(psfPointFade(std::nextafter(1.0f, 2.0f), radius, scale) > 1.0f - 1.0e-6f);
            CHECK(psfPointFade(std::nextafter(fadeEnd, 1.0f), radius, scale) < 1.0e-6f);
            CHECK(psfPointFade(fadeEnd, radius, scale) == 0.0f);

            float previous = 1.0f;
            for (int i = 0; i <= 200; ++i)
            {
                float discRadius = static_cast<float>(i) * fadeEnd / 100.0f;
                float fade = psfPointFade(discRadius, radius, scale);
                CHECK(fade <= previous);
                CHECK(fade >= 0.0f);
                CHECK(fade <= 1.0f);
                previous = fade;
            }
        }
    }
}

TEST_CASE("Calibrated legacy star sizes use source flux and physical pixels")
{
    using celestia::engine::detail::calibratedStarSize;
    CHECK(calibratedStarSize(StarStyle::PointStars, 100.0f, 2.0f) == 2.0f);
    CHECK(calibratedStarSize(StarStyle::PointStars, 0.001f, 0.5f) == 1.0f);
    CHECK(calibratedStarSize(StarStyle::FuzzyPointStars, 100.0f, 1.25f) == 7.0f);
    CHECK(calibratedStarSize(StarStyle::ScaledDiscStars, 0.001f, 2.0f) == 10.0f);
    CHECK(calibratedStarSize(StarStyle::ScaledDiscStars, 16.0f, 2.0f) == 20.0f);
    CHECK(calibratedStarSize(StarStyle::ScaledDiscStars, 1.0e12f, 2.0f) == 80.0f);
}

TEST_CASE("Calibrated legacy cores yield to resolved surfaces across their footprint")
{
    using celestia::engine::detail::calibratedStarPointFade;
    CHECK(calibratedStarPointFade(StarStyle::PointStars, 16.0f, 1.0f, 2.0f) == 1.0f);
    CHECK(calibratedStarPointFade(StarStyle::PointStars, 16.0f, 2.0f, 2.0f) == 0.0f);
    CHECK(calibratedStarPointFade(StarStyle::FuzzyPointStars, 16.0f, 3.0f, 2.0f) == doctest::Approx(0.5f));
    CHECK(calibratedStarPointFade(StarStyle::FuzzyPointStars, 16.0f, 5.0f, 2.0f) == 0.0f);
    CHECK(calibratedStarPointFade(StarStyle::ScaledDiscStars, 16.0f, 5.5f, 2.0f) == doctest::Approx(0.5f));
    CHECK(calibratedStarPointFade(StarStyle::ScaledDiscStars, 16.0f, 10.0f, 2.0f) == 0.0f);
}

TEST_CASE("Calibrated label peaks match the pixel-integrated point kernels")
{
    namespace detail = celestia::engine::detail;
    for (auto style : { StarStyle::PointStars, StarStyle::FuzzyPointStars, StarStyle::ScaledDiscStars })
    {
        for (float scale : { 0.5f, 1.25f, 2.0f })
        {
            for (float flux : { 0.01f, 1.0f, 16.0f })
            {
                CAPTURE(style);
                CAPTURE(scale);
                CAPTURE(flux);
                double size = detail::calibratedStarSize(style, flux, scale);
                double mass = 1.0 / size;
                if (style != StarStyle::PointStars)
                {
                    mass = 0;
                    for (int sample = 0; sample < 1024; ++sample)
                    {
                        double x = (sample + 0.5) / 1024.0 - 0.5;
                        mass += std::sqrt(32.0 / std::numbers::pi)
                                * std::exp(-32.0 * x * x / (size * size))
                                / (1024.0 * size * std::erf(std::sqrt(8.0)));
                    }
                }
                CHECK(detail::calibratedStarPeakRadiance(style, flux, scale)
                      == doctest::Approx(flux * scale * scale * mass * mass).epsilon(0.00001));
            }
        }
    }
}

TEST_SUITE_END();
