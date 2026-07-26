// atmosphererenderer.cpp
//
// Copyright (C) 2001-present, Celestia Development Team
// Original version Chris Laurel <claurel@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "atmosphererenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <celcompat/numbers.h>
#include <celengine/atmosphere.h>
#include <celengine/glsupport.h>
#include <celengine/lightenv.h>
#include <celengine/lodspheremesh.h>
#include <celengine/render.h>
#include <celengine/renderinfo.h>
#include <celengine/shadermanager.h>
#include <celengine/texture.h>
#include <celengine/brunetonatmospherefile.h>
#include <celmath/frustum.h>
#include <celmath/mathlib.h>
#include <celmath/vecgl.h>
#include <celutil/indexlist.h>

#include "brunetonatmosphereresource.h"

using celestia::util::BuildIndexList;
using celestia::util::IndexListCapacity;
using ushort = unsigned short;

namespace celestia::render
{

namespace
{
constexpr int MaxSkyRings = 32;
constexpr int MaxSkySlices = 180;
constexpr int MinSkySlices = 30;

constexpr int MaxVertices = MaxSkySlices * (MaxSkyRings + 1);
constexpr int MaxIndices = IndexListCapacity(MaxSkySlices,  MaxSkyRings + 1);
} // end unnamed namespace

AtmosphereRenderer::AtmosphereRenderer(Renderer &renderer) :
    m_renderer(renderer)
{
}

AtmosphereRenderer::~AtmosphereRenderer()
{
    if (m_depthPartitionTexture != 0)
        glDeleteTextures(1, &m_depthPartitionTexture);
}

void
AtmosphereRenderer::setSceneDepth(
    GLuint depthTexture,
    const std::vector<Eigen::Vector2f>& partitionNearFar,
    int width,
    int height,
    int originX,
    int originY)
{
    m_sceneDepthTexture = depthTexture;
    m_depthPartitionCount =
        static_cast<int>(partitionNearFar.size());
    m_sceneWidth = width;
    m_sceneHeight = height;
    m_sceneOriginX = originX;
    m_sceneOriginY = originY;

    if (m_depthPartitionTexture == 0)
        glGenTextures(1, &m_depthPartitionTexture);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, m_depthPartitionTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RG32F,
        static_cast<GLsizei>(partitionNearFar.size()),
        1,
        0,
        GL_RG,
        GL_FLOAT,
        partitionNearFar.data());
    glActiveTexture(GL_TEXTURE0);
}

void AtmosphereRenderer::initGL()
{
    if (m_initialized)
        return;

    m_initialized = true;

    m_skyVertices.reserve(MaxVertices);
    m_skyIndices.reserve(MaxIndices);
    m_skyContour.reserve(MaxSkySlices + 1);

    m_vo = gl::VertexObject(gl::VertexObject::Primitive::Triangles);
    m_bo = gl::Buffer(gl::Buffer::TargetHint::Array);

    m_vo.addVertexBuffer(
        m_bo,
        CelestiaGLProgram::VertexCoordAttributeIndex,
        3,
        gl::VertexObject::DataType::Float,
        false,
        sizeof(SkyVertex),
        offsetof(SkyVertex, position));
    m_vo.addVertexBuffer(
        m_bo,
        CelestiaGLProgram::ColorAttributeIndex,
        4,
        gl::VertexObject::DataType::UnsignedByte,
        true,
        sizeof(SkyVertex),
        offsetof(SkyVertex, color));
    m_vo.setIndexBuffer(gl::Buffer(gl::Buffer::TargetHint::ElementArray), 0, gl::VertexObject::IndexType::UnsignedShort);

    constexpr std::array<std::array<float, 4>, 4> vertices
    {{
        {{ -1.0f, -1.0f, 0.0f, 1.0f }},
        {{  1.0f, -1.0f, 0.0f, 1.0f }},
        {{ -1.0f,  1.0f, 0.0f, 1.0f }},
        {{  1.0f,  1.0f, 0.0f, 1.0f }},
    }};
    m_brunetonBo = gl::Buffer(gl::Buffer::TargetHint::Array);
    m_brunetonBo.setData(vertices);
    m_brunetonVo =
        gl::VertexObject(gl::VertexObject::Primitive::TriangleStrip);
    m_brunetonVo.addVertexBuffer(
        m_brunetonBo,
        CelestiaGLProgram::VertexCoordAttributeIndex,
        4,
        gl::VertexObject::DataType::Float,
        false,
        sizeof(vertices[0]),
        0);
    m_brunetonVo.setCount(static_cast<GLsizei>(vertices.size()));
}

void
AtmosphereRenderer::computeLegacy(
    const Atmosphere         &atmosphere,
    const LightingState      &ls,
    const Eigen::Vector3f    &center,
    const Eigen::Quaternionf &orientation,
    const Eigen::Vector3f    &semiAxes,
    const Eigen::Vector3f    &sunDirection,
    float                     pixSize,
    bool                      lit)
{
    // Gradually fade in the atmosphere if it's thickness on screen is just
    // over one pixel.
    float fade = std::clamp(pixSize - 2.0f, 0.0f, 1.0f);

    Eigen::Matrix3f rot = orientation.toRotationMatrix();
    Eigen::Matrix3f irot = orientation.conjugate().toRotationMatrix();

    Eigen::Vector3f eyePos(Eigen::Vector3f::Zero());
    float radius = semiAxes.maxCoeff();
    Eigen::Vector3f eyeVec = center - eyePos;
    eyeVec = rot * eyeVec;
    double centerDist = eyeVec.norm();

    float height = atmosphere.height / radius;
    Eigen::Vector3f recipSemiAxes = semiAxes.cwiseInverse();

    // ellipDist is not the true distance from the surface unless the
    // planet is spherical.  Computing the true distance requires finding
    // the roots of a sixth degree polynomial, and isn't actually what we
    // want anyhow since the atmosphere region is just the planet ellipsoid
    // multiplied by a uniform scale factor.  The value that we do compute
    // is the distance to the surface along a line from the eye position to
    // the center of the ellipsoid.
    float ellipDist = (eyeVec.cwiseProduct(recipSemiAxes)).norm() - 1.0f;
    bool within = ellipDist < height;

    // Adjust the tesselation of the sky dome/ring based on distance from the
    // planet surface.
    int nSlices = MaxSkySlices;
    if (ellipDist < 0.25f)
    {
        nSlices = MinSkySlices + std::max(0, static_cast<int>((ellipDist * 4.0f)) * (MaxSkySlices - MinSkySlices));
        nSlices &= ~1;
    }

    int nRings = std::min(1 + static_cast<int>(pixSize) / 5, 6);
    int nHorizonRings = nRings;
    if (within)
        nRings += 12;

    float horizonHeight = height;
    if (within)
    {
        if (ellipDist <= 0.0f)
            horizonHeight = 0.0f;
        else
            horizonHeight *= std::max(std::pow(ellipDist / height, 0.33f), 0.001f);
    }

    Eigen::Vector3f e = -eyeVec;
    Eigen::Vector3f e_ = e.cwiseProduct(recipSemiAxes);
    float ee = e_.dot(e_);

    // Compute the cosine of the altitude of the sun.  This is used to compute
    // the degree of sunset/sunrise coloration.
    float cosSunAltitude = 0.0f;
    {
        // Check for a sun either directly behind or in front of the viewer
        float cosSunAngle = sunDirection.dot(e) / static_cast<float>(centerDist);
        if (cosSunAngle < -1.0f + 1.0e-6f)
        {
            cosSunAltitude = 0.0f;
        }
        else if (cosSunAngle > 1.0f - 1.0e-6f)
        {
            cosSunAltitude = 0.0f;
        }
        else
        {
            Eigen::Vector3f v = (rot * -sunDirection) * static_cast<float>(centerDist);
            Eigen::Vector3f tangentPoint = center + irot * math::ellipsoidTangent(recipSemiAxes, v, e, e_, ee);
            Eigen::Vector3f tangentDir = (tangentPoint - eyePos).normalized();
            cosSunAltitude = sunDirection.dot(tangentDir);
        }
    }

    Eigen::Vector3f normal = eyeVec / static_cast<float>(centerDist);

    Eigen::Vector3f uAxis, vAxis;
    if (std::abs(normal.x()) < std::abs(normal.y()) && std::abs(normal.x()) < std::abs(normal.z()))
    {
        uAxis = Eigen::Vector3f::UnitX().cross(normal);
    }
    else if (std::abs(eyeVec.y()) < std::abs(normal.z()))
    {
        uAxis = Eigen::Vector3f::UnitY().cross(normal);
    }
    else
    {
        uAxis = Eigen::Vector3f::UnitZ().cross(normal);
    }
    uAxis.normalize();
    vAxis = uAxis.cross(normal);

    // Compute the contour of the ellipsoid
    for (int i = 0; i <= nSlices; i++)
    {
        SkyContourPoint p;
        // We want rays with an origin at the eye point and tangent to the the
        // ellipsoid.
        float theta = static_cast<float>(i) / static_cast<float>(nSlices) * 2.0f * numbers::pi_v<float>;
        Eigen::Vector3f w = std::cos(theta) * uAxis + std::sin(theta) * vAxis;
        w *= static_cast<float>(centerDist);

        Eigen::Vector3f toCenter = math::ellipsoidTangent(recipSemiAxes, w, e, e_, ee);
        p.v = irot * toCenter;
        p.centerDist = p.v.norm();
        p.eyeDir = p.v + (center - eyePos);
        p.eyeDist = p.eyeDir.norm();
        p.eyeDir.normalize();

        float skyCapDist = std::hypot(p.eyeDist, horizonHeight * radius);
        p.cosSkyCapAltitude = p.eyeDist / skyCapDist;
        m_skyContour.push_back(p);
    }

    Eigen::Vector3f botColor = atmosphere.lowerColor.linearize(gl::sRGBRendering).toVector3();
    Eigen::Vector3f topColor = atmosphere.upperColor.linearize(gl::sRGBRendering).toVector3();
    Eigen::Vector3f sunsetColor = atmosphere.sunsetColor.linearize(gl::sRGBRendering).toVector3();

    if (within)
    {
        Eigen::Vector3f skyColor = atmosphere.skyColor.linearize(gl::sRGBRendering).toVector3();
        if (ellipDist < 0.0f)
            topColor = skyColor;
        else
            topColor = skyColor + (topColor - skyColor) * (ellipDist / height);
    }

    if (ls.nLights == 0 && lit)
    {
        botColor = topColor = sunsetColor = Eigen::Vector3f::Zero();
    }

    Eigen::Vector3f zenith = m_skyContour[0].v + m_skyContour[nSlices / 2].v;
    zenith.normalize();
    zenith *= m_skyContour[0].centerDist * (1.0f + horizonHeight * 2.0f);

    float minOpacity = within ? (1.0f - ellipDist / height) * 0.75f : 0.0f;
    float sunset = cosSunAltitude < 0.9f ? 0.0f : (cosSunAltitude - 0.9f) * 10.0f;

    // Build the list of vertices
    for (int i = 0; i <= nRings; i++)
    {
        SkyVertex vtx;
        float h = std::min(1.0f, static_cast<float>(i) / static_cast<float>(nHorizonRings));
        float hh = std::sqrt(h);
        float u = i <= nHorizonRings ? 0.0f :
            static_cast<float>(i - nHorizonRings) / static_cast<float>(nRings - nHorizonRings);
        float r = math::lerp(h, 1.0f - (horizonHeight * 0.05f), 1.0f + horizonHeight);

        for (int j = 0; j < nSlices; j++)
        {
            Eigen::Vector3f v;
            if (i <= nHorizonRings)
                v = m_skyContour[j].v * r;
            else
                v = math::mix(m_skyContour[j].v, zenith, u) * r;
            Eigen::Vector3f p = center + v;

            Eigen::Vector3f viewDir = p.normalized();
            float cosSunAngle = viewDir.dot(sunDirection);
            float cosAltitude = viewDir.dot(m_skyContour[j].eyeDir);
            float brightness = 1.0f;
            float coloration = 0.0f;
            if (lit)
            {
                if (sunset > 0.0f && cosSunAngle > 0.7f && cosAltitude > 0.98f)
                {
                    coloration =  (1.0f / 0.30f) * (cosSunAngle - 0.70f);
                    coloration *= 50.0f * (cosAltitude - 0.98f);
                    coloration *= sunset;
                }

                cosSunAngle = m_skyContour[j].v.dot(sunDirection) / m_skyContour[j].centerDist;
                if (cosSunAngle <= -0.2f)
                    brightness = 0.0f;
                else if (cosSunAngle >= 0.3f)
                    brightness = 1.0f;
                else
                    brightness = (cosSunAngle + 0.2f) * 2.0f;
            }

            std::memcpy(&vtx.position[0], p.data(), vtx.position.size() * sizeof(vtx.position[0]));

            float atten = 1.0f - hh;
            Eigen::Vector3f color = math::mix(botColor, topColor, hh);
            brightness *= minOpacity + (1.0f - minOpacity) * fade * atten;
            if (coloration != 0.0f)
                color = math::mix(color, sunsetColor, coloration);

            Color(brightness * color.x(),
                  brightness * color.y(),
                  brightness * color.z(),
                  fade * (minOpacity + (1.0f - minOpacity)) * atten).get(&vtx.color[0]);
            m_skyVertices.push_back(vtx);
        }
    }
    m_skyContour.clear();

    // Create the index list
    BuildIndexList(static_cast<ushort>(nRings), static_cast<ushort>(nSlices), m_skyIndices);
}

bool
AtmosphereRenderer::renderBruneton(
    const RenderInfo& ri,
    const LightingState& ls,
    const Matrices& m,
    const BrunetonAtmosphereResource& resource,
    float luminanceScale,
    const Eigen::Vector3f& bodySemiAxes,
    GLuint surfaceIdTexture,
    GLint surfaceBodyId,
    Texture* cloudTexture,
    float cloudHeight,
    float cloudTextureOffset)
{
    bool useDualSource = false;
#ifndef GL_ES
    useDualSource = ls.nLights > 0 && gl::dualSourceBlending;
#endif

    ShaderManager& shaderManager = m_renderer.getShaderManager();
    auto* program = shaderManager.getShader(
        StaticShader::BrunetonAtmosphere,
        useDualSource
            ? StaticShaderOptions::DualSource
            : StaticShaderOptions::None);
    if (useDualSource && shaderManager.isErrorProgram(program))
    {
        useDualSource = false;
        program = shaderManager.getShader(StaticShader::BrunetonAtmosphere);
    }
    if (program == nullptr)
        return false;

    const auto& parameters = resource.parameters();
    const GLuint programId = program->getID();
    program->use();

    Eigen::Matrix4f modelFromView = Eigen::Matrix4f::Identity();
    modelFromView.topLeftCorner<3, 3>() =
        (parameters.bottomRadius *
         bodySemiAxes.cwiseInverse()).asDiagonal() *
        ri.orientation.conjugate().toRotationMatrix();
    Mat4ShaderParameter(programId, "model_from_view") = modelFromView;
    Mat4ShaderParameter(programId, "view_from_clip") = m.projection->inverse();

    FloatShaderParameter(programId, "atmosphere.bottom_radius") =
        parameters.bottomRadius;
    FloatShaderParameter(programId, "atmosphere.top_radius") =
        parameters.topRadius;
    Vec3ShaderParameter(programId, "atmosphere.solar_irradiance") =
        Eigen::Map<const Eigen::Vector3f>(
            parameters.solarIrradiance.data());
    FloatShaderParameter(programId, "atmosphere.sun_angular_radius") =
        parameters.sunAngularRadius;
    Vec3ShaderParameter(programId, "atmosphere.rayleigh_scattering") =
        Eigen::Map<const Eigen::Vector3f>(parameters.rayleighScattering.data());
    Vec3ShaderParameter(programId, "atmosphere.mie_scattering") =
        Eigen::Map<const Eigen::Vector3f>(parameters.mieScattering.data());
    FloatShaderParameter(programId, "atmosphere.mie_phase_function_g") =
        parameters.miePhaseFunctionG;
    FloatShaderParameter(programId, "atmosphere.mu_s_min") =
        parameters.muSMin;

    Eigen::Vector3f camera = ri.eyePos_obj * parameters.bottomRadius;
    const float cameraRadius = camera.norm();
    if (cameraRadius == 0.0f)
        return false;
    const float minimumCameraRadius =
        std::nextafter(parameters.bottomRadius, parameters.topRadius);
    if (cameraRadius > 0.0f && cameraRadius < minimumCameraRadius)
        camera *= minimumCameraRadius / cameraRadius;
    Vec3ShaderParameter(programId, "camera") = camera;
    Vec3ShaderParameter(programId, "earth_center") = Eigen::Vector3f::Zero();
    Eigen::Vector3f sunDirection =
        ls.nLights == 0
            ? Eigen::Vector3f::UnitZ()
            : ls.lights[0].direction_obj;
    sunDirection =
        (parameters.bottomRadius *
         bodySemiAxes.cwiseInverse()).cwiseProduct(sunDirection).normalized();
    Vec3ShaderParameter(programId, "sun_direction") = sunDirection;
    Vec3ShaderParameter(programId, "sky_spectral_radiance_to_luminance") =
        Eigen::Map<const Eigen::Vector3f>(
            parameters.skySpectralRadianceToLuminance.data());
    Vec3ShaderParameter(programId, "sun_spectral_radiance_to_luminance") =
        Eigen::Map<const Eigen::Vector3f>(
            parameters.sunSpectralRadianceToLuminance.data());
    FloatShaderParameter(programId, "luminance_scale") = luminanceScale;
    Vec2ShaderParameter(programId, "viewport_size") =
        Eigen::Vector2f(
            static_cast<float>(m_sceneWidth),
            static_cast<float>(m_sceneHeight));
    Vec2ShaderParameter(programId, "viewport_origin") =
        Eigen::Vector2f(
            static_cast<float>(m_sceneOriginX),
            static_cast<float>(m_sceneOriginY));
    IntegerShaderParameter(programId, "depth_partition_count") =
        m_depthPartitionCount;

    IntegerShaderParameter(programId, "combined_scattering_textures") =
        parameters.combinedScattering ? 1 : 0;
    IntegerShaderParameter(programId, "manual_float_filtering") =
        resource.usesManualFloatFiltering() ? 1 : 0;
    IntegerShaderParameter(programId, "manual_scattering_filtering") =
        resource.usesManualFloatFiltering() &&
                resource.usesFullPrecisionScattering()
            ? 1
            : 0;
    IntegerShaderParameter(programId, "transmittance_texture") = 0;
    IntegerShaderParameter(programId, "scattering_texture") = 1;
    IntegerShaderParameter(programId, "single_mie_scattering_texture") = 2;
    IntegerShaderParameter(programId, "scene_depth_texture") = 3;
    IntegerShaderParameter(programId, "depth_partition_texture") = 4;
    IntegerShaderParameter(programId, "irradiance_texture") = 5;
    IntegerShaderParameter(programId, "surface_id_texture") = 7;
    FloatShaderParameter(programId, "surface_body_id") =
        static_cast<float>(surfaceBodyId);

    TextureTile cloudTile(0);
    const bool renderClouds =
        cloudTexture != nullptr &&
        (cloudTile = cloudTexture->getTile(0, 0, 0)).texID != 0;
    IntegerShaderParameter(programId, "render_clouds") =
        renderClouds ? 1 : 0;
    IntegerShaderParameter(programId, "cloud_texture") = 6;
    IntegerShaderParameter(programId, "cloud_texture_has_alpha") =
        renderClouds && cloudTexture->hasAlpha() ? 1 : 0;
    FloatShaderParameter(programId, "cloud_radius") =
        parameters.bottomRadius *
        (1.0f + cloudHeight / bodySemiAxes.maxCoeff());
    FloatShaderParameter(programId, "cloud_texture_offset") =
        cloudTextureOffset;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, resource.transmittanceTexture());
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, resource.scatteringTexture());
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(
        GL_TEXTURE_3D,
        parameters.combinedScattering
            ? resource.scatteringTexture()
            : resource.singleMieTexture());
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_sceneDepthTexture);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, m_depthPartitionTexture);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, resource.irradianceTexture());
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(
        GL_TEXTURE_2D,
        renderClouds ? cloudTile.texID : resource.transmittanceTexture());
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, surfaceIdTexture);
    glActiveTexture(GL_TEXTURE0);

    Renderer::PipelineState state;
    state.blending = true;
    if (useDualSource)
    {
        state.blendFunc = { GL_ONE, GL_SRC1_COLOR };
        m_renderer.setPipelineState(state);
        IntegerShaderParameter(programId, "render_mode") = 1;
        m_brunetonVo.draw();
        return true;
    }

    state.blendFunc = { GL_ZERO, GL_SRC_COLOR };
    m_renderer.setPipelineState(state);
    IntegerShaderParameter(programId, "render_mode") = 0;
    m_brunetonVo.draw();

    if (ls.nLights == 0)
        return true;

    state.blendFunc = { GL_ONE, GL_ONE };
    m_renderer.setPipelineState(state);
    IntegerShaderParameter(programId, "render_mode") = 1;
    m_brunetonVo.draw();
    return true;
}

void
AtmosphereRenderer::renderLegacy(
    const Atmosphere         &atmosphere,
    const LightingState      &ls,
    const Eigen::Vector3f    &center,
    const Eigen::Quaternionf &orientation,
    const Eigen::Vector3f    &semiAxes,
    const Eigen::Vector3f    &sunDirection,
    float                     pixSize,
    bool                      lit,
    const Matrices           &m)
{
    computeLegacy(atmosphere, ls, center, orientation, semiAxes, sunDirection, pixSize, lit);

    ShaderProperties shadprop;
    shadprop.texUsage = TexUsage::VertexColors;
    shadprop.lightModel = LightingModel::UnlitModel;
    auto *prog = m_renderer.getShaderManager().getShader(shadprop);
    if (prog == nullptr)
        return;

    Renderer::PipelineState ps;
    ps.depthTest = true;
    ps.blending = true;
    ps.blendFunc = {GL_ONE, GL_ONE_MINUS_SRC_ALPHA};
    m_renderer.setPipelineState(ps);

    m_bo.invalidateData().setData(m_skyVertices, gl::Buffer::BufferUsage::StreamDraw);
    m_vo.getIndexBuffer().invalidateData().setData(m_skyIndices, gl::Buffer::BufferUsage::StreamDraw);

    prog->use();
    prog->setMVPMatrices(*m.projection, *m.modelview);
    m_vo.draw(gl::VertexObject::Primitive::TriangleStrip, static_cast<int>(m_skyIndices.size()));

    m_skyIndices.clear();
    m_skyVertices.clear();
}

void
AtmosphereRenderer::render(
    const RenderInfo         &ri,
    const Atmosphere         &atmosphere,
    const LightingState      &ls,
    const Eigen::Quaternionf &/*planetOrientation*/,
    float                     radius,
    const math::Frustum      &frustum,
    const Matrices           &m)
{
    ShaderProperties shadprop;
    shadprop.nLights = static_cast<ushort>(ls.nLights);

    shadprop.texUsage |= TexUsage::Scattering;
    shadprop.lightModel = LightingModel::AtmosphereModel;

    bool useDualSource = false;
    GLenum scatteringBlendDestination = GL_ONE;
    useDualSource = ls.nLights > 0 && gl::dualSourceBlending;
    if (useDualSource)
#ifdef GL_ES
        scatteringBlendDestination = GL_SRC1_COLOR_EXT;
#else
        scatteringBlendDestination = GL_SRC1_COLOR;
#endif

    ShaderProperties transmissionProps = shadprop;
    transmissionProps.nLights = 0;
    transmissionProps.effects = LightingEffects::AtmosphereTransmission;

    if (useDualSource)
        shadprop.effects = LightingEffects::AtmosphereDualSource;

    CelestiaGLProgram* transmissionProg = useDualSource
        ? nullptr
        : m_renderer.getShaderManager().getShader(transmissionProps);
    CelestiaGLProgram* scatteringProg = ls.nLights == 0
        ? nullptr
        : m_renderer.getShaderManager().getShader(shadprop);
    if ((!useDualSource && transmissionProg == nullptr) ||
        (ls.nLights > 0 && scatteringProg == nullptr))
        return;

    float extinctionThreshold = m_renderer.getAtmosphereExtinctionThreshold();
    float atmosphereRadius = radius +
                             m_renderer.getAtmosphereShellHeight(atmosphere.mieScaleHeight);
    float atmScale = atmosphereRadius / radius;

    auto setupAtmosphereProgram = [&](CelestiaGLProgram* prog)
    {
        prog->use();
        prog->eyePosition = ls.eyePos_obj / atmScale;
        prog->setAtmosphereParameters(atmosphere, radius, atmosphereRadius, atmosphereRadius,
                                      m_renderer.getAtmosphereSegmentCount(),
                                      extinctionThreshold);
        prog->setMVPMatrices(*m.projection, (*m.modelview) * math::scale(atmScale));
    };

#if 0
    // Currently eclipse shadows are ignored when rendering atmospheres
    if (shadprop.shadowCounts != 0)
        prog->setEclipseShadowParameters(ls, radius, planetOrientation);
#endif

    glFrontFace(GL_CW);

    Renderer::PipelineState ps;
    ps.blending = true;
    ps.depthTest = true;

    math::Frustum shellFrustum = frustum;
    shellFrustum.transform(math::scale(1.0f / atmScale));

    if (transmissionProg != nullptr)
    {
        setupAtmosphereProgram(transmissionProg);
        ps.blendFunc = {GL_ZERO, GL_SRC_COLOR};
        m_renderer.setPipelineState(ps);
        m_renderer.m_lodSphere->render(LODSphereMesh::Normals,
                                       shellFrustum,
                                       ri.pixWidth,
                                       nullptr);
    }

    if (scatteringProg != nullptr)
    {
        scatteringProg->use();
        scatteringProg->setLightParameters(ls, ri.color, ri.specularColor, Color::Black);
        scatteringProg->ambientColor = Eigen::Vector3f::Zero();
        setupAtmosphereProgram(scatteringProg);

        ps.blendFunc = {GL_ONE, scatteringBlendDestination};
        m_renderer.setPipelineState(ps);
        m_renderer.m_lodSphere->render(LODSphereMesh::Normals,
                                       shellFrustum,
                                       ri.pixWidth,
                                       nullptr);
    }

    glFrontFace(GL_CCW);
}

} // namespace celestia::render
