// Copyright (C) 2026-present, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include <numbers>

#include <doctest.h>

#include <celastro/astro.h>
#include <celengine/radiometry.h>
#include <celengine/rendcontext.h>

namespace
{

class MaterialContext : public RenderContext
{
public:
    MaterialContext() : RenderContext(nullptr) {}

    void makeCurrent(const cmod::Material& m) override
    {
        diffuse = convertMaterialColor(m.diffuse);
        specular = convertMaterialColor(m.specular);
        emissive = convertMaterialColor(m.emissive);
        opacity = m.opacity;
    }

    cmod::Color diffuse;
    cmod::Color specular;
    cmod::Color emissive;
    float opacity{ 1.0f };
};

class ScopedSRGBRendering
{
public:
    explicit ScopedSRGBRendering(bool enabled) : previous(celestia::gl::sRGBRendering)
    {
        celestia::gl::sRGBRendering = enabled;
    }

    ~ScopedSRGBRendering()
    {
        celestia::gl::sRGBRendering = previous;
    }

private:
    bool previous;
};

} // namespace

TEST_CASE("SSC texture overrides retain already-linear material colors")
{
    ScopedSRGBRendering srgb(true);
    MaterialContext context;
    cmod::Material material;
    material.diffuse = cmod::Color(Color(0.8f, 0.6f, 0.4f).linearize());
    material.specular = cmod::Color(Color(0.3f, 0.2f, 0.1f).linearize());
    material.opacity = 0.37f;

    context.setMaterial(&material, RenderContext::MaterialColorSpace::Linear);
    CHECK(context.diffuse == material.diffuse);
    CHECK(context.specular == material.specular);
    CHECK(context.opacity == material.opacity);

    context.lock();
    cmod::Material meshMaterial;
    context.setMaterial(&meshMaterial, RenderContext::MaterialColorSpace::sRGB);
    context.updateShader(cmod::VertexDescription{}, cmod::PrimitiveGroupType::TriList);
    CHECK(context.getMaterial() == &material);
    CHECK(context.diffuse == material.diffuse);
    CHECK(context.specular == material.specular);
}

TEST_CASE("Authored CMOD colors decode once after a linear override")
{
    ScopedSRGBRendering srgb(true);
    MaterialContext context;
    cmod::Material material;
    material.diffuse = cmod::Color(0.8f, 0.6f, 0.02f);
    material.specular = cmod::Color(0.3f, 0.2f, 0.1f);
    material.emissive = cmod::Color(0.1f, 0.02f, 0.01f);
    material.opacity = 0.37f;

    context.setMaterial(&material, RenderContext::MaterialColorSpace::Linear);
    context.setMaterial(&material, RenderContext::MaterialColorSpace::sRGB);
    CHECK(context.diffuse.red() == doctest::Approx(0.603827f));
    CHECK(context.diffuse.green() == doctest::Approx(0.318547f));
    CHECK(context.diffuse.blue() == doctest::Approx(0.00154799f));
    CHECK(context.specular.red() == doctest::Approx(0.073239f));
    CHECK(context.emissive.green() == doctest::Approx(0.00154799f));
    CHECK(context.opacity == material.opacity);
}

TEST_CASE("Legacy material colors are unchanged for either source color space")
{
    ScopedSRGBRendering srgb(false);
    MaterialContext context;
    cmod::Material material;
    material.diffuse = cmod::Color(0.8f, 0.6f, 0.02f);
    material.specular = cmod::Color(0.3f, 0.2f, 0.1f);
    material.emissive = cmod::Color(0.1f, 0.02f, 0.01f);

    for (auto colorSpace : {RenderContext::MaterialColorSpace::sRGB,
                            RenderContext::MaterialColorSpace::Linear})
    {
        context.setMaterial(&material, colorSpace);
        CHECK(context.diffuse == material.diffuse);
        CHECK(context.specular == material.specular);
        CHECK(context.emissive == material.emissive);
    }

}

TEST_CASE("Emission integrates to the same visual magnitude as stellar irradiance")
{
    namespace detail = celestia::engine::detail;
    namespace astro = celestia::astro;
    constexpr float luminance = 100.0f;
    constexpr float solidAngle = 0.0002f;
    float magnitude = astro::irradianceToMag(luminance * solidAngle / 2.54e-6f);
    for (float pixelAngle : { 0.0001f, 0.0002f, 0.0004f })
    {
        float pixels = solidAngle / (pixelAngle * pixelAngle);
        CHECK(detail::emittedRadiance(luminance, pixelAngle) * pixels ==
              doctest::Approx(astro::magToIrradiance(magnitude)));
        CHECK(detail::emittedRadiance(0.0f, pixelAngle) == 0.0f);
        CHECK(detail::emittedRadiance(2.0f * luminance, pixelAngle) ==
              doctest::Approx(2.0f * detail::emittedRadiance(luminance, pixelAngle)));
    }

    CHECK(detail::emittedRadiance(luminance, 0.0002f) ==
          doctest::Approx(4.0f * detail::emittedRadiance(luminance, 0.0001f)));
    CHECK(detail::emittedDiscIrradiance(luminance, 1.0f, 10000.0) ==
          doctest::Approx(std::numbers::pi_v<float> *
                          detail::emittedRadiance(luminance, 0.0001f)));
    CHECK(detail::emittedDiscIrradiance(luminance, 1.0f, 10000.0) ==
          doctest::Approx(4.0f * detail::emittedDiscIrradiance(luminance, 1.0f, 20000.0)));
}

TEST_CASE("Dust scattering conserves its phase integral and separates activity from illumination")
{
    namespace detail = celestia::engine::detail;
    constexpr int samples = 10000;
    for (float asymmetry : { -0.6f, 0.0f, 0.6f })
    {
        double integral = 0.0;
        for (int i = 0; i < samples; ++i)
            integral += detail::dustScatteringPhase(-1.0f + (2.0f * i + 1.0f) / samples, asymmetry);
        integral *= 4.0 * std::numbers::pi / samples;
        CHECK(integral == doctest::Approx(1.0).epsilon(0.0001));
    }
    CHECK(detail::dustScatteringPhase(1.0f, 0.6f) >
          detail::dustScatteringPhase(0.0f, 0.6f));
    CHECK(detail::dustScatteringPhase(0.0f, 0.6f) >
          detail::dustScatteringPhase(-1.0f, 0.6f));
    CHECK(detail::cometDustActivity(2.0f, 0.0f) == 1.0f);
    CHECK(detail::cometDustActivity(2.0f, 2.0f) == doctest::Approx(0.25f));
    CHECK(detail::cometDustActivity(0.5f, 2.0f) == doctest::Approx(4.0f));
    CHECK(std::isfinite(detail::cometDustActivity(1000.0 / 149597870.7, 8.0f)));
    CHECK(detail::cometDustColumnScale(0, 1000.0 / 149597870.7, 8, 1e7f, 0.001) == 0.0f);
    CHECK_FALSE(detail::cometDustColumnScale(1e12f, 10.0 / 149597870.7, 8, 1e7f, 0.001).has_value());
}
