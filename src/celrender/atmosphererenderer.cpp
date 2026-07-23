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
#include <array>
#include <cmath>
#include <cstring>
#include <string>

#include <celcompat/numbers.h>
#include <celengine/atmosphere.h>
#include <celengine/glsupport.h>
#include <celengine/lightenv.h>
#include <celengine/lodspheremesh.h>
#include <celengine/render.h>
#include <celengine/renderinfo.h>
#include <celengine/shadermanager.h>
#include <celmath/frustum.h>
#include <celmath/mathlib.h>
#include <celmath/vecgl.h>
#include <celutil/indexlist.h>
#include <celutil/logger.h>
#include "brunetonatmosphere.h"

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

constexpr std::size_t BrunetonTextureCount = 6;

std::size_t
textureIndex(BrunetonTextureKind kind)
{
    return static_cast<std::size_t>(kind) - 1;
}
} // end unnamed namespace

struct AtmosphereRenderer::BrunetonResources
{
    ~BrunetonResources()
    {
        glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
    }

    bool load(const std::filesystem::path& path)
    {
        BrunetonAtmosphereData data;
        if (!data.load(path))
            return false;

        const BrunetonTextureData* phase =
            data.find(BrunetonTextureKind::Phase);
        const BrunetonTextureData* transmittance =
            data.find(BrunetonTextureKind::Transmittance);
        const BrunetonTextureData* irradiance =
            data.find(BrunetonTextureKind::IndirectIlluminance);
        const BrunetonTextureData* multiple =
            data.find(BrunetonTextureKind::MultipleScattering);
        const BrunetonTextureData* aerosols =
            data.find(BrunetonTextureKind::SingleAerosolsScattering);
        const BrunetonTextureData* theta =
            data.find(BrunetonTextureKind::ThetaDeviation);
        hasThetaDeviation = theta != nullptr;
        if (phase->height != 2 ||
            multiple->width != aerosols->width ||
            multiple->height != aerosols->height ||
            multiple->depth != aerosols->depth ||
            (theta != nullptr &&
             (theta->width != transmittance->width ||
              theta->height != transmittance->height)) ||
            irradiance->depth != 1)
        {
            util::GetLogger()->error(
                "Incompatible Bruneton atmosphere texture dimensions in {}.\n", path);
            return false;
        }

        while (glGetError() != GL_NO_ERROR)
        {
        }
        glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
        for (const BrunetonTextureData& texture : data.textures())
        {
            auto index = textureIndex(texture.kind);
            dimensions[index] = {
                static_cast<GLint>(texture.width),
                static_cast<GLint>(texture.height),
                static_cast<GLint>(texture.depth),
            };

            GLenum target = texture.dimensions == 3 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
            glBindTexture(target, textures[index]);
#ifdef GL_ES
            glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
#else
            glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
#endif
            glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            if (target == GL_TEXTURE_3D)
            {
                glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
                glTexImage3D(target, 0, GL_RGB32F,
                             dimensions[index].x(), dimensions[index].y(), dimensions[index].z(),
                             0, GL_RGB, GL_FLOAT, texture.pixels.data());
            }
            else
            {
                glTexImage2D(target, 0, GL_RGB32F,
                             dimensions[index].x(), dimensions[index].y(),
                             0, GL_RGB, GL_FLOAT, texture.pixels.data());
            }
        }

        glBindTexture(GL_TEXTURE_2D, 0);
        glBindTexture(GL_TEXTURE_3D, 0);
        valid = glGetError() == GL_NO_ERROR;
        if (!valid)
            util::GetLogger()->error("Failed to upload Bruneton atmosphere textures from {}.\n", path);
        return valid;
    }

    void bind(CelestiaGLProgram& program, bool refraction) const
    {
        constexpr std::array<GLenum, BrunetonTextureCount> targets{
            GL_TEXTURE_2D,
            GL_TEXTURE_2D,
            GL_TEXTURE_2D,
            GL_TEXTURE_3D,
            GL_TEXTURE_3D,
            GL_TEXTURE_2D,
        };
        constexpr std::array<const char*, BrunetonTextureCount> samplers{
            "uPhaseTexture",
            "uTransmittanceTexture",
            "uIrradianceTexture",
            "uMultipleScatteringTexture",
            "uSingleAerosolsScatteringTexture",
            "uThetaDeviationTexture",
        };

        std::size_t count = refraction ? textures.size() : textures.size() - 1;
        for (std::size_t i = 0; i < count; ++i)
        {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(i));
            glBindTexture(targets[i], textures[i]);
            program.samplerParam(samplers[i]) = static_cast<int>(i);
        }
    }

    std::array<GLuint, BrunetonTextureCount> textures{};
    std::array<Eigen::Vector3i, BrunetonTextureCount> dimensions{};
    bool hasThetaDeviation{ false };
    bool valid{ false };
};

AtmosphereRenderer::AtmosphereRenderer(Renderer &renderer) :
    m_renderer(renderer)
{
}

AtmosphereRenderer::~AtmosphereRenderer() = default;

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
    const Eigen::Vector3f    &cameraPosition,
    float                     radius,
    const math::Frustum      &frustum,
    const Matrices           &m)
{
    if (!atmosphere.brunetonData.empty())
    {
        renderBruneton(ri, atmosphere, ls, cameraPosition, radius, frustum, m);
        return;
    }

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

void
AtmosphereRenderer::renderBruneton(
    const RenderInfo         &ri,
    const Atmosphere         &atmosphere,
    const LightingState      &ls,
    const Eigen::Vector3f    &cameraPosition,
    float                     radius,
    const math::Frustum      &frustum,
    const Matrices           &m)
{
    auto [resourceIt, inserted] =
        m_brunetonResources.try_emplace(atmosphere.brunetonData);
    if (inserted)
    {
        resourceIt->second = std::make_unique<BrunetonResources>();
        if (!resourceIt->second->load(atmosphere.brunetonData))
            return;
    }
    BrunetonResources* resources = resourceIt->second.get();
    if (resources == nullptr || !resources->valid)
    {
        return;
    }

    if (atmosphere.refraction && !resources->hasThetaDeviation)
    {
        util::GetLogger()->error(
            "Bruneton atmosphere {} enables refraction but has no theta-deviation texture.\n",
            atmosphere.brunetonData);
        resources->valid = false;
        return;
    }

    CelestiaGLProgram* program =
        m_renderer.getShaderManager().getShader(StaticShader::BrunetonAtmosphere);
    if (program == nullptr)
        return;

    const auto& dimensions = resources->dimensions;
    const Eigen::Vector3i& transmittance =
        dimensions[textureIndex(BrunetonTextureKind::Transmittance)];
    const Eigen::Vector3i& irradiance =
        dimensions[textureIndex(BrunetonTextureKind::IndirectIlluminance)];
    const Eigen::Vector3i& scattering =
        dimensions[textureIndex(BrunetonTextureKind::MultipleScattering)];
    if (scattering.x() % static_cast<int>(atmosphere.scatteringTextureNuSize) != 0)
    {
        util::GetLogger()->error(
            "Bruneton atmosphere {} has an invalid scattering Nu size.\n",
            atmosphere.brunetonData);
        resources->valid = false;
        return;
    }

    float topRadius = radius + atmosphere.height;
    float atmosphereScale = topRadius / radius;
    Eigen::Vector3f sunDirection = ls.nLights == 0
        ? Eigen::Vector3f::UnitZ()
        : ls.lights[0].direction_obj.normalized();

    program->use();
    program->setMVPMatrices(*m.projection, (*m.modelview) * math::scale(atmosphereScale));
    program->vec3Param("uCamera") = cameraPosition;
    program->vec3Param("uSunDirection") = sunDirection;
    program->vec3Param("uSolarIlluminance") = atmosphere.sunIlluminance;
    program->floatParam("uBottomRadius") = radius;
    program->floatParam("uTopRadius") = topRadius;
    program->floatParam("uSunAngularRadius") = atmosphere.sunAngularRadius;
    program->floatParam("uMuSMin") = std::cos(atmosphere.maxSunZenithAngle);
    program->intParam("uTransmittanceTextureWidth") = transmittance.x();
    program->intParam("uTransmittanceTextureHeight") = transmittance.y();
    program->intParam("uScatteringTextureRSize") = scattering.z();
    program->intParam("uScatteringTextureMuSize") = scattering.y();
    program->intParam("uScatteringTextureNuSize") =
        static_cast<int>(atmosphere.scatteringTextureNuSize);
    program->intParam("uScatteringTextureMuSSize") =
        scattering.x() / static_cast<int>(atmosphere.scatteringTextureNuSize);
    program->intParam("uIrradianceTextureWidth") = irradiance.x();
    program->intParam("uIrradianceTextureHeight") = irradiance.y();
    program->intParam("uRefraction") = atmosphere.refraction ? 1 : 0;
    program->intParam("uHasSun") = ls.nLights == 0 ? 0 : 1;
    resources->bind(*program, atmosphere.refraction);

    math::Frustum shellFrustum = frustum;
    shellFrustum.transform(math::scale(1.0f / atmosphereScale));

    Renderer::PipelineState state;
    state.blending = true;
    state.depthTest = true;

    program->intParam("uRenderMode") = 0;
    state.blendFunc = { GL_ZERO, GL_SRC_COLOR };
    m_renderer.setPipelineState(state);
    m_renderer.m_lodSphere->render(LODSphereMesh::Normals,
                                   shellFrustum,
                                   ri.pixWidth,
                                   nullptr);

    if (ls.nLights > 0)
    {
        program->intParam("uRenderMode") = 1;
        state.blendFunc = { GL_ONE, GL_ONE };
        m_renderer.setPipelineState(state);
        m_renderer.m_lodSphere->render(LODSphereMesh::Normals,
                                       shellFrustum,
                                       ri.pixWidth,
                                       nullptr);
    }

    glActiveTexture(GL_TEXTURE0);
}

} // namespace celestia::render
