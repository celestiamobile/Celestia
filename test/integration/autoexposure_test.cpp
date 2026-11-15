// Copyright (C) 2026 Celestia Development Team. All rights reserved.
// Licensed under the GNU General Public License, version 2 or later.

#include <OpenGL/OpenGL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

#include <doctest.h>

#include <celengine/atmosphere.h>
#include <celengine/autoexposure.h>
#include <celengine/framebuffer.h>
#include <celengine/glsupport.h>
#include <celengine/pointstarvertexbuffer.h>
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

} // namespace

TEST_SUITE_BEGIN("Automatic exposure GPU");

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
