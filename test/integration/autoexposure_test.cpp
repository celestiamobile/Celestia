// Copyright (C) 2026 Celestia Development Team. All rights reserved.
// Licensed under the GNU General Public License, version 2 or later.

#include <OpenGL/OpenGL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <span>

#include <doctest.h>

#include <celengine/atmosphere.h>
#include <celengine/autoexposure.h>
#include <celengine/framebuffer.h>
#include <celengine/glsupport.h>
#include <celengine/lightenv.h>
#include <celengine/pointstarvertexbuffer.h>
#include <celengine/radiometry.h>
#include <celengine/rendcontext.h>
#include <celengine/render.h>
#include <celengine/shadermanager.h>
#include <celrender/gl/buffer.h>
#include <celrender/gl/vertexobject.h>
#include <celrender/legacylargestarrenderer.h>
#include <celutil/logger.h>

namespace
{

class Context
{
public:
    explicit Context(bool linear = false)
    {
        celestia::util::CreateLogger();
        CGLPixelFormatAttribute attributes[]{
            kCGLPFAOpenGLProfile, static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),
            kCGLPFAAllowOfflineRenderers, static_cast<CGLPixelFormatAttribute>(0),
        };
        CGLPixelFormatObj format{};
        GLint count{};
        REQUIRE(CGLChoosePixelFormat(attributes, &format, &count) == kCGLNoError);
        REQUIRE(format != nullptr);
        auto result = CGLCreateContext(format, nullptr, &context);
        CGLDestroyPixelFormat(format);
        REQUIRE(result == kCGLNoError);
        REQUIRE(CGLSetCurrentContext(context) == kCGLNoError);
        REQUIRE(celestia::gl::init());
        previousLinear = celestia::gl::sRGBRendering;
        celestia::gl::sRGBRendering = linear;
    }

    ~Context()
    {
        celestia::gl::sRGBRendering = previousLinear;
        CGLSetCurrentContext(nullptr);
        CGLDestroyContext(context);
    }

private:
    CGLContextObj context{};
    bool previousLinear{};
};

struct MeterFixture
{
    explicit MeterFixture(bool linear = false) : context(linear) {}

    Context context;
    Renderer renderer;

    void clear(FramebufferObject& source)
    {
        REQUIRE(source.bind());
        renderer.removeScissor();
        renderer.setPipelineState({});
        const float black[]{ 0, 0, 0, 0 };
        glClearBufferfv(GL_COLOR, 1, black);
    }

    void light(int x, int y, int width, int height, float luminance)
    {
        renderer.setScissor(x, y, width, height);
        const float color[]{ luminance, luminance, luminance, 1 };
        glClearBufferfv(GL_COLOR, 1, color);
        renderer.removeScissor();
    }

    void measure(AutoExposure& meter, const FramebufferObject& source, double now = 0)
    {
        REQUIRE(meter.meter(renderer, source, now));
        glFinish();
        REQUIRE(meter.advance(now));
        glFinish();
        REQUIRE(meter.advance(now));
        REQUIRE(glGetError() == GL_NO_ERROR);
    }
};

constexpr auto Attachments =
    FramebufferObject::Attachment::Color | FramebufferObject::Attachment::Meter;

struct DustRayFixture : MeterFixture
{
    struct Pixel
    {
        std::array<float, 4> display{};
        std::array<float, 4> meter{};
    };

    DustRayFixture() :
        MeterFixture(true),
        target(32, 32, Attachments | FramebufferObject::Attachment::Depth, 1, true),
        triangle(celestia::gl::VertexObject::Primitive::Triangles)
    {
        REQUIRE(target.isValid());
        renderer.setViewport(0, 0, 32, 32);
        constexpr std::array vertices{ -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
        buffer = celestia::gl::Buffer::create(celestia::gl::Buffer::TargetHint::Array,
            std::as_bytes(std::span{ vertices }), celestia::gl::Buffer::BufferUsage::StaticDraw);
        triangle.addVertexBuffer(buffer, CelestiaGLProgram::VertexCoordAttributeIndex, 2,
            celestia::gl::VertexObject::DataType::Float, false, 2 * sizeof(float), 0).setCount(3);
        auto& manager = renderer.getShaderManager();
        manager.setCameraExposure(1);
        shader = manager.getShader(StaticShader::Comet);
        REQUIRE(shader != nullptr);
        shader->use();
        shader->intParam("calibratedDust") = 1;
        shader->vec3Param("tailAxis") = Eigen::Vector3f(0, 1, 0);
        shader->floatParam("columnScale") = 1;
        shader->floatParam("dustAsymmetry") = 0;
        shader->intParam("dustLightCount") = 1;
        shader->vec3Param("dustLightColor[0]") = Eigen::Vector3f::Ones();
        setRay(Eigen::Vector3f(0, .5f, -10), Eigen::Vector3f(0, .5f, 0),
               Eigen::Vector3f(0, 0, -1));
    }

    void setRay(const Eigen::Vector3f& camera, const Eigen::Vector3f& boundary,
                const Eigen::Vector3f& normal)
    {
        shader->use();
        Eigen::Matrix4f projection = Eigen::Matrix4f::Identity();
        projection(2, 2) = 1e-12f;
        shader->setMVPMatrices(projection);
        shader->vec3Param("cameraTail") = camera;
        shader->vec3Param("tailOrigin") = -boundary * 1e10f;
        shader->floatParam("tailLength") = 1e10f;
        glVertexAttrib3f(CelestiaGLProgram::NormalAttributeIndex, normal.x(), normal.y(), normal.z());
    }

    Pixel pixel(bool occluded = false)
    {
        REQUIRE(target.bind());
        Renderer::PipelineState state;
        state.depthMask = true;
        renderer.setPipelineState(state);
        const float depth = occluded ? .25f : 1;
        glClearBufferfv(GL_DEPTH, 0, &depth);
        state.depthMask = false;
        state.depthTest = true;
        renderer.setPipelineState(state);
        const float black[]{ 0, 0, 0, 0 };
        glClearBufferfv(GL_COLOR, 0, black);
        glClearBufferfv(GL_COLOR, 1, black);
        shader->use();
        triangle.draw();
        Pixel result;
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(16, 16, 1, 1, GL_RGBA, GL_FLOAT, result.display.data());
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glReadPixels(16, 16, 1, 1, GL_RGBA, GL_FLOAT, result.meter.data());
        REQUIRE(glGetError() == GL_NO_ERROR);
        return result;
    }

    float radiance(bool occluded = false)
    {
        return pixel(occluded).display[0];
    }

    FramebufferObject target;
    celestia::gl::Buffer::SharedPtr buffer;
    celestia::gl::VertexObject triangle;
    CelestiaGLProgram* shader{};
};

} // namespace

TEST_SUITE_BEGIN("Automatic exposure GPU");

TEST_CASE("Dust columns survive near-plane clipping without double scattering")
{
    DustRayFixture fixture;
    constexpr float radius = .0707106781f;
    constexpr float length = 1e10f;
    const std::array vertices{
        -1.0f, -1.0f, -radius * length, 3.0f, -1.0f, -radius * length,
        -1.0f, 3.0f, -radius * length,
        -1.0f, -1.0f, radius * length, 3.0f, -1.0f, radius * length,
        -1.0f, 3.0f, radius * length,
    };
    auto buffer = celestia::gl::Buffer::create(celestia::gl::Buffer::TargetHint::Array,
        std::as_bytes(std::span{ vertices }), celestia::gl::Buffer::BufferUsage::StaticDraw);
    celestia::gl::VertexObject mesh(celestia::gl::VertexObject::Primitive::Triangles);
    mesh.addVertexBuffer(buffer, CelestiaGLProgram::VertexCoordAttributeIndex, 3,
        celestia::gl::VertexObject::DataType::Float, false, 3 * sizeof(float), 0);
    fixture.renderer.getShaderManager().setPhysicalLightScale(.25f);
    fixture.shader->use();
    fixture.shader->vec4Param("dustLightPosition[0]") = Eigen::Vector4f(10000, .5f, 0, 1e-8f);
    fixture.shader->vec3Param("dustLightColor[0]") = Eigen::Vector3f::Constant(1e8f);

    for (bool reverse : { false, true })
    for (bool perspective : { false, true })
    {
        CAPTURE(reverse);
        CAPTURE(perspective);
        fixture.setRay(Eigen::Vector3f(0, .5f, reverse ? 10 : -10),
                       Eigen::Vector3f(0, .5f, 0), Eigen::Vector3f(0, 0, -1));
        auto draw = [&](float clipOffset, bool occluded = false)
        {
            REQUIRE(fixture.target.bind());
            Renderer::PipelineState state;
            state.depthMask = true;
            fixture.renderer.setPipelineState(state);
            const float depth = occluded ? 0 : 1;
            const float black[]{ 0, 0, 0, 0 };
            glClearBufferfv(GL_DEPTH, 0, &depth);
            glClearBufferfv(GL_COLOR, 0, black);
            glClearBufferfv(GL_COLOR, 1, black);
            state.depthMask = false;
            state.depthTest = true;
            state.blending = true;
            state.blendFunc.src = GL_ONE;
            state.blendFunc.dst = GL_ONE;
            fixture.renderer.setPipelineState(state);
            Eigen::Matrix4f projection = Eigen::Matrix4f::Identity();
            projection(2, 2) = (reverse ? -1 : 1) / length;
            projection(2, 3) = clipOffset;
            projection(3, 2) = perspective ? .1f * projection(2, 2) : 0;
            fixture.shader->use();
            fixture.shader->setMVPMatrices(projection);
            glVertexAttrib3f(CelestiaGLProgram::NormalAttributeIndex, 0, 0, -1);
            mesh.draw(3, 0);
            glVertexAttrib3f(CelestiaGLProgram::NormalAttributeIndex, 0, 0, 1);
            mesh.draw(3, 3);
            DustRayFixture::Pixel pixel;
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glReadPixels(16, 16, 1, 1, GL_RGBA, GL_FLOAT, pixel.display.data());
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glReadPixels(16, 16, 1, 1, GL_RGBA, GL_FLOAT, pixel.meter.data());
            REQUIRE(glGetError() == GL_NO_ERROR);
            return pixel;
        };
        auto ordinary = draw(0);
        REQUIRE(ordinary.display[0] > 0);
        REQUIRE(ordinary.meter[0] > 0);
        for (float offset : { -.92f, -.94f, -.98f, -1.02f })
        {
            CAPTURE(offset);
            auto clipped = draw(offset);
            for (int channel = 0; channel < 3; ++channel)
            {
                CHECK(clipped.display[channel] ==
                      doctest::Approx(ordinary.display[channel]).epsilon(.002).scale(0));
                CHECK(clipped.meter[channel] ==
                      doctest::Approx(ordinary.meter[channel]).epsilon(.002).scale(0));
            }
        }
        auto blocked = draw(-.98f, true);
        CHECK(blocked.display[0] == 0);
        CHECK(blocked.meter[0] == 0);
    }
}

TEST_CASE("Dust columns scatter multiple lights with independent display and meter exposure")
{
    DustRayFixture fixture;
    auto& manager = fixture.renderer.getShaderManager();
    const Eigen::Vector3f tint(1.0f, 0.5f, 0.25f);
    for (bool inside : { false, true })
    for (bool occluded : { false, true })
    for (int lights : { 0, 1, 2 })
        for (float exposure : { 0.5f, 4.0f })
        {
            CAPTURE(inside);
            CAPTURE(occluded);
            CAPTURE(lights);
            CAPTURE(exposure);
            manager.setCameraExposure(exposure);
            manager.setPhysicalLightScale(0.25f / exposure);
            fixture.setRay(Eigen::Vector3f(0, .5f, inside ? 0 : -10),
                           Eigen::Vector3f(0, .5f, inside ? std::sqrt(.005f) : 0),
                           Eigen::Vector3f(0, 0, inside ? 1 : -1));
            fixture.shader->intParam("dustLightCount") = lights;
            for (int i = 0; i < lights; ++i)
            {
                auto index = std::to_string(i);
                fixture.shader->vec4Param(("dustLightPosition[" + index + "]").c_str()) =
                    Eigen::Vector4f(10000.0f, 0.5f, 0.0f, 1.0e-8f);
                fixture.shader->vec3Param(("dustLightColor[" + index + "]").c_str()) = tint * 1.0e8f;
            }
            auto pixel = fixture.pixel(occluded);
            float column = 0.25f * std::sqrt(0.005f) * 16.0f / 15.0f;
            if (occluded)
                column = 0;
            else if (inside)
                column *= 0.5f;
            for (int channel = 0; channel < 3; ++channel)
            {
                float expected = lights * tint[channel] * column / (4.0f * std::numbers::pi_v<float>);
                if (expected == 0)
                {
                    CHECK(pixel.display[channel] == 0);
                    CHECK(pixel.meter[channel] == 0);
                }
                else
                {
                    CHECK(pixel.display[channel] == doctest::Approx(expected * exposure).epsilon(0.01).scale(0));
                    CHECK(pixel.meter[channel] == doctest::Approx(expected * 0.25f).epsilon(0.01).scale(0));
                }
            }
            CHECK(glGetError() == GL_NO_ERROR);
        }
}

TEST_CASE("Dust sungrazer columns resolve steep illumination from either viewing direction")
{
    DustRayFixture fixture;
    for (double distance : { .001, .01, .1 })
    for (float asymmetry : { -.6f, 0.0f, .95f })
    for (bool reverse : { false, true })
    {
        CAPTURE(distance);
        CAPTURE(asymmetry);
        CAPTURE(reverse);
        fixture.setRay(Eigen::Vector3f(0, reverse ? 4 : -3, 0),
                       Eigen::Vector3f(0, reverse ? 1 : 0, 0),
                       Eigen::Vector3f(0, reverse ? 1 : -1, 0));
        fixture.shader->floatParam("dustAsymmetry") = asymmetry;
        fixture.shader->vec4Param("dustLightPosition[0]") =
            Eigen::Vector4f(0, -distance, 0, .25 * distance * distance);
        double a = 1 + distance;
        double integral = a * a * (1 / distance - 1 / a) -
                          2 * a * std::log(a / distance) + 1;
        double phase = celestia::engine::detail::dustScatteringPhase(reverse ? 1 : -1, asymmetry);
        CHECK(fixture.radiance() ==
              doctest::Approx(integral * phase).epsilon(.002).scale(0));
    }
}

TEST_CASE("Dust exposure recovers extreme finite products independently for display and metering")
{
    DustRayFixture fixture;
    fixture.setRay(Eigen::Vector3f(0, -3, 0), Eigen::Vector3f::Zero(), Eigen::Vector3f(0, -1, 0));
    fixture.shader->vec4Param("dustLightPosition[0]") = Eigen::Vector4f(0, -5e-6f, 0, 1e-14f);
    auto scale = celestia::engine::detail::cometDustColumnScale(
        1e6f, 1000.0 / 149597870.7, 8, 1e7f, .001);
    REQUIRE(scale.has_value());
    float light = celestia::astro::lumToIrradiance(.01f, 1e8f) * .1f;
    struct Scenario
    {
        float columnScale;
        float light;
        float exposure;
        float meterScale;
    };
    const std::array scenarios{
        Scenario{ *scale, light, 1e-37f, 1 },
        Scenario{ *scale, light, 1, 1 },
        Scenario{ *scale, light, 1, 1e-37f },
        Scenario{ 1e-25f, 1e-20f, 1e38f, .25f },
        Scenario{ 1e-20f, 1e-20f, 1e-4f, 1e38f },
        Scenario{ *scale, light, 1e-37f, 0 },
        Scenario{ 0, light, 1, 1 },
    };
    constexpr double distance = 5e-6;
    double column = (1 / distance + 2 - 2 * (1 + distance) * std::log((1 + distance) / distance)) /
                    (4 * std::numbers::pi);
    for (const auto& scenario : scenarios)
    {
        CAPTURE(scenario.columnScale);
        CAPTURE(scenario.light);
        CAPTURE(scenario.exposure);
        CAPTURE(scenario.meterScale);
        auto& manager = fixture.renderer.getShaderManager();
        manager.setCameraExposure(scenario.exposure);
        manager.setPhysicalLightScale(scenario.meterScale);
        fixture.shader->use();
        fixture.shader->floatParam("columnScale") = scenario.columnScale;
        fixture.shader->vec3Param("dustLightColor[0]") =
            Eigen::Vector3f(scenario.light, 0, scenario.light * .5f);
        auto pixel = fixture.pixel();
        for (int channel = 0; channel < 3; ++channel)
        {
            double tint = channel == 0 ? 1 : (channel == 1 ? 0 : .5);
            double exposed = column * scenario.columnScale * scenario.light * tint * scenario.exposure;
            auto check = [](float actual, double expected)
            {
                CHECK(std::isfinite(actual));
                expected = std::min(expected, 65504.0);
                if (expected < 0x1p-25)
                    CHECK(actual == 0);
                else
                    CHECK(actual == doctest::Approx(expected).epsilon(.005).scale(0));
            };
            check(pixel.display[channel], exposed);
            check(pixel.meter[channel], exposed * scenario.meterScale);
        }
    }
}

TEST_CASE("Dust close-source quadrature covers both sides of each light's closest approach")
{
    DustRayFixture fixture;
    fixture.shader->intParam("dustLightCount") = 2;
    constexpr std::array sources{ std::array{ .01, .2 }, std::array{ -.04, .8 } };
    for (unsigned int light = 0; light < sources.size(); ++light)
    {
        auto index = std::to_string(light);
        fixture.shader->vec4Param(("dustLightPosition[" + index + "]").c_str()) =
            Eigen::Vector4f(sources[light][0], sources[light][1], 0, 1e-8f);
        fixture.shader->vec3Param(("dustLightColor[" + index + "]").c_str()) = Eigen::Vector3f::Ones();
    }
    for (float asymmetry : { -.6f, 0.0f, .95f })
    for (double start : { 0.0, .5 })
    {
        CAPTURE(asymmetry);
        CAPTURE(start);
        fixture.setRay(Eigen::Vector3f(0, start == 0 ? -3 : start, 0),
                       Eigen::Vector3f(0, start == 0 ? 0 : 1, 0),
                       Eigen::Vector3f(0, start == 0 ? -1 : 1, 0));
        fixture.shader->floatParam("dustAsymmetry") = asymmetry;
        double expected = 0;
        constexpr int intervals = 16384;
        for (int i = 0; i <= intervals; ++i)
        {
            double y = start + (1 - start) * i / intervals;
            double weight = i == 0 || i == intervals ? 1 : (i % 2 == 0 ? 2 : 4);
            for (const auto& source : sources)
            {
                double dy = y - source[1];
                double squaredDistance = source[0] * source[0] + dy * dy;
                double cosine = -dy / std::sqrt(squaredDistance);
                double denominator = 1 + asymmetry * asymmetry - 2 * asymmetry * cosine;
                double phase = (1 - asymmetry * asymmetry) /
                               (4 * std::numbers::pi * denominator * std::sqrt(denominator));
                expected += weight * (1 - y) * (1 - y) * phase / squaredDistance;
            }
        }
        expected *= (1 - start) / (3 * intervals);
        CHECK(fixture.radiance() ==
              doctest::Approx(expected).epsilon(.005).scale(0));
    }
}

TEST_CASE("Night and material emission bypass reflected albedo and preserve physical metering")
{
    MeterFixture fixture(true);
    FramebufferObject source(32, 32, Attachments, 1, true);
    REQUIRE(source.isValid());
    fixture.renderer.setViewport(0, 0, 32, 32);
    auto& manager = fixture.renderer.getShaderManager();
    constexpr std::array vertices{ -1.0f, -1.0f, 0.0f, 3.0f, -1.0f, 0.0f,
                                  -1.0f, 3.0f, 0.0f };
    celestia::gl::VertexObject triangle(celestia::gl::VertexObject::Primitive::Triangles);
    auto buffer = celestia::gl::Buffer::create(
        celestia::gl::Buffer::TargetHint::Array, std::as_bytes(std::span{ vertices }),
        celestia::gl::Buffer::BufferUsage::StaticDraw);
    triangle.setCount(3);
    triangle.addVertexBuffer(buffer, CelestiaGLProgram::VertexCoordAttributeIndex, 3,
                            celestia::gl::VertexObject::DataType::Float, false,
                            3 * sizeof(float), 0);
    glVertexAttrib3f(CelestiaGLProgram::NormalAttributeIndex, 0, 0, 1);
    glVertexAttrib2f(CelestiaGLProgram::TextureCoord0AttributeIndex, 0.5f, 0.5f);

    std::array<GLuint, 3> textures{};
    glGenTextures(3, textures.data());
    const std::array texels{
        std::array{ 0.0f, 0.0f, 0.0f, 1.0f },
        std::array{ 0.5f, 0.25f, 0.125f, 1.0f },
        std::array{ 0.125f, 0.25f, 0.5f, 1.0f },
    };
    for (unsigned int index = 0; index < textures.size(); ++index)
    {
        glActiveTexture(GL_TEXTURE0 + index);
        glBindTexture(GL_TEXTURE_2D, textures[index]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT,
                     texels[index].data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    const Eigen::Vector3f emission(0.25f, 0.125f, 0.5f);
    for (auto model : { LightingModel::DiffuseModel, LightingModel::LunarLambertModel,
                        LightingModel::PerPixelSpecularModel })
        for (unsigned int lights : { 0U, 1U, 2U })
            for (float exposure : { 0.25f, 4.0f })
                for (float reflectance : { 0.0f, 1.0f })
                    for (bool day : { false, true })
                {
                    CAPTURE(model);
                    CAPTURE(lights);
                    CAPTURE(exposure);
                    CAPTURE(reflectance);
                    CAPTURE(day);
                    ShaderProperties properties;
                    properties.lightModel = model;
                    properties.nLights = lights;
                    properties.texUsage = TexUsage::DiffuseTexture | TexUsage::NightTexture |
                                          TexUsage::EmissiveTexture;
                    properties.effects = LightingEffects::SurfaceEmission;
                    auto* shader = manager.getShader(properties);
                    REQUIRE(shader != nullptr);
                    manager.setCameraExposure(exposure);
                    manager.setPhysicalLightScale(0.5f / exposure);
                    REQUIRE(source.bind());
                    fixture.renderer.setPipelineState({});
                    shader->use();
                    shader->setMVPMatrices(Eigen::Matrix4f::Identity());
                    LightingState lighting;
                    lighting.nLights = lights;
                    lighting.radianceScale = 100.0f;
                    lighting.surfaceReflectanceScale = reflectance;
                    lighting.surfaceDimming = 0.25f;
                    for (unsigned int index = 0; index < lights; ++index)
                    {
                        lighting.lights[index].color = Color::White;
                        lighting.lights[index].irradiance = 10.0f;
                        lighting.lights[index].direction_obj =
                            Eigen::Vector3f(0.0f, 0.0f, day ? 1.0f : -1.0f);
                    }
                    shader->setLightParameters(lighting, Eigen::Vector3f::Zero(),
                                                Eigen::Vector3f::Zero(), emission, 0.37f);
                    shader->nightEmissiveScale = 2.0f;
                    shader->materialEmissiveScale = 3.0f;
                    shader->emissionDimming = 0.25f;
                    shader->lunarLambert = 0.5f;
                    shader->shininess = 4.0f;
                    triangle.draw();
                    std::array<float, 4> color{}, meter{};
                    glReadBuffer(GL_COLOR_ATTACHMENT0);
                    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_FLOAT, color.data());
                    glReadBuffer(GL_COLOR_ATTACHMENT1);
                    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_FLOAT, meter.data());
                    for (std::size_t channel = 0; channel < 3; ++channel)
                    {
                        float nightScale = day && lights > 0 ? 0.0f : 2.0f;
                        float expected = emission[channel] + texels[1][channel] * nightScale +
                                         texels[2][channel] * 3.0f;
                        CHECK(color[channel] ==
                              doctest::Approx(expected * exposure * 0.25f).epsilon(0.002));
                        CHECK(meter[channel] ==
                              doctest::Approx(expected * 0.5f).epsilon(0.002));
                    }
                    CHECK(color[3] == doctest::Approx(0.37f).epsilon(0.002));
                    CHECK(meter[3] == doctest::Approx(0.37f).epsilon(0.002));
                    CHECK(glGetError() == GL_NO_ERROR);
                }
    ShaderProperties unlitProperties;
    unlitProperties.lightModel = LightingModel::EmissiveModel;
    unlitProperties.effects = LightingEffects::CalibratedEmission;
    unlitProperties.texUsage = TexUsage::DiffuseTexture | TexUsage::EmissiveTexture;
    auto* unlit = manager.getShader(unlitProperties);
    REQUIRE(unlit != nullptr);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures[2]);
    for (float scale : { 0.0f, 3.0f })
    {
        CAPTURE(scale);
        manager.setCameraExposure(2.0f);
        manager.setPhysicalLightScale(0.25f);
        fixture.renderer.setPipelineState({});
        unlit->use();
        unlit->setMVPMatrices(Eigen::Matrix4f::Identity());
        unlit->lights.diffuse[0] = Eigen::Vector3f::Zero();
        unlit->materialEmission = emission;
        unlit->emissiveScale = scale;
        unlit->opacity = 0.37f;
        unlit->emissionDimming = 1.0f;
        triangle.draw();
        std::array<float, 4> color{}, meter{};
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(16, 16, 1, 1, GL_RGBA, GL_FLOAT, color.data());
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        glReadPixels(16, 16, 1, 1, GL_RGBA, GL_FLOAT, meter.data());
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            float expected = (emission[channel] + texels[2][channel]) * scale;
            CHECK(color[channel] == doctest::Approx(expected * 2.0f).epsilon(0.002));
            CHECK(meter[channel] == doctest::Approx(expected * 0.5f).epsilon(0.002));
        }
        CHECK(color[3] == doctest::Approx(0.37f).epsilon(0.002));
        CHECK(glGetError() == GL_NO_ERROR);
    }
    glDeleteTextures(3, textures.data());
}

TEST_CASE("Lit and unlit CMOD emission decode once without tinting by diffuse color")
{
    for (bool linear : { false, true })
    {
        MeterFixture fixture(linear);
        FramebufferObject source(32, 32, Attachments, 1, true);
        REQUIRE(source.isValid());
        REQUIRE(source.bind());
        fixture.renderer.setViewport(0, 0, 32, 32);
        fixture.renderer.getShaderManager().setCameraExposure(2.0f);
        fixture.renderer.getShaderManager().setPhysicalLightScale(0.25f);
        constexpr std::array vertices{ -1.0f, -1.0f, 0.0f, 3.0f, -1.0f, 0.0f,
                                      -1.0f, 3.0f, 0.0f };
        celestia::gl::VertexObject triangle(celestia::gl::VertexObject::Primitive::Triangles);
        auto buffer = celestia::gl::Buffer::create(
            celestia::gl::Buffer::TargetHint::Array, std::as_bytes(std::span{ vertices }),
            celestia::gl::Buffer::BufferUsage::StaticDraw);
        triangle.setCount(3);
        triangle.addVertexBuffer(buffer, CelestiaGLProgram::VertexCoordAttributeIndex, 3,
                                celestia::gl::VertexObject::DataType::Float, false,
                                3 * sizeof(float), 0);
        glVertexAttrib3f(CelestiaGLProgram::NormalAttributeIndex, 0, 0, 1);
        const Eigen::Matrix4f identity = Eigen::Matrix4f::Identity();
        LightingState lighting;
        GLSL_RenderContext lit(&fixture.renderer, lighting, 1.0f,
                               Eigen::Quaternionf::Identity(), &identity, &identity);
        GLSLUnlit_RenderContext unlit(&fixture.renderer, &identity, &identity);
        lit.setEmissiveScale(3.0f);
        unlit.setEmissiveScale(linear ? 3.0f : 1.0f);
        cmod::Material material;
        material.diffuse = cmod::Color(0.0f, 0.0f, 0.0f);
        material.emissive = cmod::Color(0.5f, 0.25f, 0.125f);
        material.opacity = 0.37f;
        for (RenderContext* context : { static_cast<RenderContext*>(&lit),
                                        static_cast<RenderContext*>(&unlit) })
        {
            CAPTURE(linear);
            CAPTURE(context == &unlit);
            context->setMaterial(&material, RenderContext::MaterialColorSpace::sRGB);
            fixture.renderer.setPipelineState({});
            triangle.draw();
            std::array<float, 4> color{}, meter{};
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glReadPixels(16, 16, 1, 1, GL_RGBA, GL_FLOAT, color.data());
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glReadPixels(16, 16, 1, 1, GL_RGBA, GL_FLOAT, meter.data());
            Eigen::Vector3f expected = material.emissive.linearize(linear).toVector3();
            if (linear)
                expected *= 3.0f;
            else if (context == &unlit)
                expected.setZero();
            else
                expected = Color(material.emissive.toVector3()).toVector3();
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                CHECK(color[channel] == doctest::Approx(expected[channel] * 2.0f).epsilon(0.002));
                CHECK(meter[channel] == doctest::Approx(expected[channel] * 0.5f).epsilon(0.002));
            }
            CHECK(color[3] == doctest::Approx(0.37f).epsilon(0.002));
            CHECK(glGetError() == GL_NO_ERROR);
        }
    }
}

TEST_CASE("Atmospheric transmission attenuates visual and physical light equally")
{
    MeterFixture fixture(true);
    FramebufferObject source(32, 32, Attachments, 1, true);
    REQUIRE(source.isValid());
    fixture.renderer.setViewport(0, 0, 32, 32);
    auto& manager = fixture.renderer.getShaderManager();

    constexpr std::array vertices{ -1.0f, -1.0f, 1.0f, 3.0f, -1.0f, 1.0f,
                                  -1.0f, 3.0f, 1.0f };
    celestia::gl::VertexObject triangle(celestia::gl::VertexObject::Primitive::Triangles);
    auto buffer = celestia::gl::Buffer::create(
        celestia::gl::Buffer::TargetHint::Array,
        std::as_bytes(std::span{ vertices }),
        celestia::gl::Buffer::BufferUsage::StaticDraw);
    triangle.setCount(3);
    triangle.addVertexBuffer(buffer, CelestiaGLProgram::VertexCoordAttributeIndex, 3,
                            celestia::gl::VertexObject::DataType::Float, false,
                            3 * sizeof(float), 0);

    Atmosphere atmosphere;
    atmosphere.mieCoeff = 0.05f;
    atmosphere.mieScaleHeight = 0.1f;
    atmosphere.rayleighCoeff = Eigen::Vector3f(0.1f, 0.2f, 0.3f);
    atmosphere.rayleighScaleHeight = 0.2f;
    atmosphere.absorptionCoeff = Eigen::Vector3f(0.05f, 0.1f, 0.2f);
    const float visual[]{ 4.0f, 2.0f, 1.0f, 1.0f };
    const float physical[]{ 0.5f, 0.25f, 0.125f, 1.0f };
    auto read = [](GLenum attachment)
    {
        std::array<float, 4> color{};
        glReadBuffer(attachment);
        glReadPixels(16, 16, 1, 1, GL_RGBA, GL_FLOAT, color.data());
        return color;
    };

    for (bool separate : { false, true })
    {
        CAPTURE(separate);
        ShaderProperties properties{};
        properties.lightModel = LightingModel::AtmosphereModel;
        properties.texUsage = TexUsage::Scattering;
        properties.effects = LightingEffects::AtmosphereTransmission;
        properties.separateRayleighMieScaleHeights = separate;
        auto* shader = manager.getShader(properties);
        REQUIRE(shader != nullptr);
        std::array<float, 4> reference{};
        for (float exposure : { 0.25f, 4.0f })
            for (float sensorScale : { 0.125f, 2.0f })
                for (float dimming : { 0.25f, 2.0f })
                {
                    CAPTURE(exposure);
                    CAPTURE(sensorScale);
                    CAPTURE(dimming);
                    REQUIRE(source.bind());
                    manager.setCameraExposure(exposure);
                    manager.setPhysicalLightScale(sensorScale);
                    shader->use();
                    shader->setMVPMatrices(Eigen::Matrix4f::Identity());
                    shader->eyePosition = Eigen::Vector3f(0.0f, 0.0f, 0.9f);
                    shader->setAtmosphereParameters(atmosphere, 0.8f, 1.0f, 1.0f, 16, 0.001f);
                    shader->emissionDimming = dimming;
                    fixture.renderer.setPipelineState({});
                    triangle.draw();
                    auto transmission = read(GL_COLOR_ATTACHMENT0);
                    auto meterTransmission = read(GL_COLOR_ATTACHMENT1);
                    if (reference[3] == 0.0f)
                        reference = transmission;
                    for (std::size_t channel = 0; channel < 3; ++channel)
                    {
                        CHECK(transmission[channel] > 0.0f);
                        CHECK(transmission[channel] < 1.0f);
                        CHECK(transmission[channel] == doctest::Approx(reference[channel]));
                        CHECK(meterTransmission[channel] ==
                              doctest::Approx(transmission[channel]));
                    }

                    glClearBufferfv(GL_COLOR, 0, visual);
                    glClearBufferfv(GL_COLOR, 1, physical);
                    Renderer::PipelineState state;
                    state.blending = true;
                    state.blendFunc = { GL_ZERO, GL_SRC_COLOR };
                    fixture.renderer.setPipelineState(state);
                    triangle.draw();
                    auto attenuatedVisual = read(GL_COLOR_ATTACHMENT0);
                    auto attenuatedPhysical = read(GL_COLOR_ATTACHMENT1);
                    for (std::size_t channel = 0; channel < 3; ++channel)
                    {
                        CHECK(attenuatedVisual[channel] ==
                              doctest::Approx(visual[channel] * transmission[channel])
                                  .epsilon(0.002));
                        CHECK(attenuatedPhysical[channel] ==
                              doctest::Approx(physical[channel] * transmission[channel])
                                  .epsilon(0.002));
                    }
                    CHECK(glGetError() == GL_NO_ERROR);
                }

        Atmosphere transparent = atmosphere;
        transparent.mieCoeff = 0.0f;
        transparent.rayleighCoeff.setZero();
        transparent.absorptionCoeff.setZero();
        shader->setAtmosphereParameters(transparent, 0.8f, 1.0f, 1.0f, 16, 0.001f);
        glClearBufferfv(GL_COLOR, 0, visual);
        glClearBufferfv(GL_COLOR, 1, physical);
        triangle.draw();
        auto unchangedVisual = read(GL_COLOR_ATTACHMENT0);
        auto unchangedPhysical = read(GL_COLOR_ATTACHMENT1);
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            CHECK(unchangedVisual[channel] == doctest::Approx(visual[channel]));
            CHECK(unchangedPhysical[channel] == doctest::Approx(physical[channel]));
        }
        CHECK(glGetError() == GL_NO_ERROR);
    }
}

TEST_CASE("Physical metering covers isolated pixels and non-power-of-two edges")
{
    MeterFixture fixture;
    for (auto size : std::array{ std::array{ 321, 193 }, std::array{ 2048, 1536 } })
    {
        FramebufferObject source(size[0], size[1], Attachments, 1, true);
        REQUIRE(source.isValid());
        for (auto pixel : std::array{
                 std::array{ 0, 0 }, std::array{ 2, 1 }, std::array{ 65, 34 },
                 std::array{ size[0] - 1, size[1] - 1 } })
        {
            CAPTURE(size);
            CAPTURE(pixel);
            fixture.clear(source);
            fixture.light(pixel[0], pixel[1], 1, 1, 4);
            AutoExposure meter(1);
            fixture.measure(meter, source);
            CHECK(std::abs(std::log2(meter.exposure() / 4.5f)) < 0.025f);
        }
    }
}

TEST_CASE("Highlight rejection remains continuous as retained light appears")
{
    MeterFixture fixture;
    FramebufferObject source(256, 256, Attachments, 1, true);
    REQUIRE(source.isValid());
    float previous = 0;
    for (int pixels = 653; pixels <= 659; ++pixels)
    {
        CAPTURE(pixels);
        fixture.clear(source);
        fixture.light(0, 0, 256, pixels / 256, 4);
        fixture.light(0, pixels / 256, pixels % 256, 1, 4);
        AutoExposure meter(1);
        fixture.measure(meter, source);
        double retained = std::max(0.0, pixels - 65536 * 0.01);
        double mean = 4 * (0.01 + 0.99 * retained / pixels);
        CHECK(std::abs(std::log2(meter.exposure() / (0.18 / mean))) < 0.025);
        if (previous > 0)
            CHECK(std::abs(std::log2(meter.exposure() / previous)) < 0.25f);
        previous = meter.exposure();
    }
}

TEST_CASE("Clipped sparse light recovers sensor range without adapting to censored values")
{
    MeterFixture fixture;
    FramebufferObject source(321, 193, Attachments, 1, true);
    REQUIRE(source.isValid());
    for (float initial : { 1.0f, 32.0f })
    {
        CAPTURE(initial);
        AutoExposure meter(initial);
        for (int frame = 0; frame < 8; ++frame)
        {
            fixture.clear(source);
            fixture.light(2, 1, 1, 1, 1.0e9f * meter.meterExposure());
            fixture.measure(meter, source, frame * 0.1);
            if (frame == 0)
            {
                CHECK(meter.exposure() == doctest::Approx(initial));
                CHECK(meter.meterExposure() < initial);
            }
        }
        CHECK(std::abs(std::log2(meter.exposure() / 1.8e-8f)) < 0.025f);
    }
}

TEST_CASE("Physical measurements do not depend on display exposure")
{
    MeterFixture fixture;
    FramebufferObject source(321, 193, Attachments, 1, true);
    REQUIRE(source.isValid());
    fixture.clear(source);
    fixture.light(2, 1, 1, 1, 4);
    for (float display : { 0.001f, 1.0f, 1000.0f })
    {
        CAPTURE(display);
        fixture.renderer.setCameraExposure(display);
        AutoExposure meter(1);
        fixture.measure(meter, source);
        CHECK(std::abs(std::log2(meter.exposure() / 4.5f)) < 0.025f);
    }
}

TEST_CASE("Legacy halo sprites and billboards are visual-only without changing ordinary stars")
{
    MeterFixture fixture;
    FramebufferObject source(32, 32, Attachments, 1, true);
    REQUIRE(source.isValid());
    const Eigen::Matrix4f identity = Eigen::Matrix4f::Identity();
    fixture.renderer.setCurrentProjectionMatrix(identity);
    fixture.renderer.setCurrentModelViewMatrix(identity);
    fixture.renderer.setViewport(0, 0, 32, 32);
    fixture.renderer.getShaderManager().setPhysicalLightScale(1);

    GLuint texture{};
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    const float texel[]{ 0.5f, 0.25f, 0.125f, 1 };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, texel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    for (bool billboard : { false, true })
        for (bool visualOnly : { false, true })
            for (float exposure : { 1.0e-8f, 2.0f })
            {
                CAPTURE(billboard);
                CAPTURE(visualOnly);
                CAPTURE(exposure);
                REQUIRE(source.bind());
                const float black[]{ 0, 0, 0, 0 };
                const float physical[]{ 0.25f, 0.5f, 0.75f, 1 };
                glClearBufferfv(GL_COLOR, 0, black);
                glClearBufferfv(GL_COLOR, 1, physical);
                fixture.renderer.getShaderManager().setCameraExposure(exposure);
                Renderer::PipelineState state;
                state.blending = true;
                state.blendFunc = { GL_SRC_ALPHA, GL_ONE };
                fixture.renderer.setPipelineState(state);
                if (billboard)
                {
                    celestia::render::LegacyLargeStarRenderer buffer(fixture.renderer, 1, false);
                    buffer.start(visualOnly ? StaticShaderOptions::VisualOnly : StaticShaderOptions::None);
                    buffer.addStar(Eigen::Vector3f::Zero(), Color::White, 8);
                    buffer.finish();
                }
                else
                {
                    PointStarVertexBuffer buffer(fixture.renderer, 1);
                    buffer.startSprites(false, visualOnly);
                    buffer.addStar(Eigen::Vector3f::Zero(), Color::White, 8, 1);
                    buffer.finish();
                }
                std::array<float, 4> color{}, meter{};
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                glReadPixels(16, 16, 1, 1, GL_RGBA, GL_FLOAT, color.data());
                glReadBuffer(GL_COLOR_ATTACHMENT1);
                glReadPixels(16, 16, 1, 1, GL_RGBA, GL_FLOAT, meter.data());
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    float expected = texel[channel] * (visualOnly ? 1.0f : exposure);
                    CHECK(color[channel] == doctest::Approx(expected).epsilon(0.002));
                    CHECK(meter[channel] == doctest::Approx(
                        physical[channel] + (visualOnly ? 0.0f : expected)).epsilon(0.002));
                }
                CHECK(glGetError() == GL_NO_ERROR);
            }
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &texture);
}

TEST_SUITE_END();
