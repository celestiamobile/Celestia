// cometrenderer.h
//
// Copyright (C) 2001-present, the Celestia Development Team
// Original version by Chris Laurel <claurel@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "cometrenderer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include <Eigen/Geometry>

#include <celastro/astro.h>
#include <celengine/body.h>
#include <celengine/glsupport.h>
#include <celengine/observer.h>
#include <celengine/radiometry.h>
#include <celengine/render.h>
#include <celengine/shadermanager.h>
#include <celmath/mathlib.h>
#include <celmath/vecgl.h>
#include <celutil/indexlist.h>
#include <celutil/logger.h>

using celestia::util::BuildIndexList;
using celestia::util::IndexListCapacity;
using ushort = unsigned short;

namespace celestia::render
{

namespace
{
constexpr int MaxCometTailPoints = 120;
constexpr int MaxCometTailSlices = 48;
constexpr int MaxVertices = MaxCometTailPoints*MaxCometTailSlices + MaxCometTailSlices + 1;
constexpr int MaxIndices = IndexListCapacity(MaxCometTailSlices, MaxCometTailPoints) + 3 * MaxCometTailSlices;

// Distance from the Sun at which comet tails will start to fade out
constexpr float CometTailAttenDistSol = astro::AUtoKilometers(5.0f);
} // end unnamed namespace

CometRenderer::CometRenderer(Renderer &renderer) :
    m_renderer(renderer),
    m_vertices(std::make_unique<CometTailVertex[]>(MaxVertices)),
    m_indices(std::make_unique<GLushort[]>(MaxIndices))
{
}

CometRenderer::~CometRenderer() = default;

bool
CometRenderer::initGL()
{
    if (m_initialized)
        return true;

    m_prog = m_renderer.getShaderManager().getShader(StaticShader::Comet);
    if (m_prog == nullptr)
        return false;

    m_brightnessLoc = m_prog->attribIndex("in_Brightness");

    m_vo = gl::VertexObject(gl::VertexObject::Primitive::TriangleStrip);
    m_bo = gl::Buffer::create(gl::Buffer::TargetHint::Array,
                              static_cast<GLsizeiptr>(sizeof(CometTailVertex) * MaxVertices),
                              gl::Buffer::BufferUsage::StreamDraw);
    m_ibo = gl::Buffer::create(gl::Buffer::TargetHint::ElementArray,
                               static_cast<GLsizeiptr>(sizeof(GLushort) * MaxIndices),
                               gl::Buffer::BufferUsage::StreamDraw);

    m_vo
        .addVertexBuffer(
            m_bo,
            CelestiaGLProgram::VertexCoordAttributeIndex,
            3,
            gl::VertexObject::DataType::Float,
            false,
            sizeof(CometTailVertex),
            offsetof(CometTailVertex, point))
        .addVertexBuffer(
            m_bo,
            CelestiaGLProgram::NormalAttributeIndex,
            3,
            gl::VertexObject::DataType::Float,
            false,
            sizeof(CometTailVertex),
            offsetof(CometTailVertex, normal))
        .addVertexBuffer(
            m_bo,
            m_brightnessLoc,
            1,
            gl::VertexObject::DataType::Float,
            false,
            sizeof(CometTailVertex),
            offsetof(CometTailVertex, brightness))
        .setIndexBuffer(m_ibo, 0, gl::VertexObject::IndexType::UnsignedShort);

    m_initialized = true;
    return true;
}

void
CometRenderer::render(const Body &body,
                      const Observer &observer,
                      const Eigen::Vector3f &pos,
                      float dustTailLength,
                      float discSizeInPixels,
                      const Matrices &m)
{
    if (m_prog == nullptr)
        return;

    double now = observer.getTime();
    bool calibrated = gl::sRGBRendering;
    const auto parameters = GetBodyFeaturesManager()->getCometTailProperties(&body);
    if (calibrated && (parameters.dustCrossSection == 0.0f || parameters.albedo == 0.0f))
        return;
    std::vector<const LightSource*> dustLights;
    if (calibrated)
    {
        for (const auto& light : m_renderer.getLightSources())
            if (light.luminosity > 0.0f)
                dustLights.push_back(&light);
        if (dustLights.empty())
            return;
        auto count = std::min(dustLights.size(), static_cast<std::size_t>(MaxShaderLights));
        std::partial_sort(dustLights.begin(), dustLights.begin() + count, dustLights.end(),
                         [&](const auto* a, const auto* b)
                         {
                             return a->luminosity / (pos.cast<double>() - a->position).squaredNorm() >
                                    b->luminosity / (pos.cast<double>() - b->position).squaredNorm();
                         });
        dustLights.resize(count);
    }

    Eigen::Vector3f cometPoints[MaxCometTailPoints];
#if 0
    Eigen::Vector3d pos0 = body.getOrbit(now)->positionAtTime(now);
    Eigen::Vector3d pos1 = body.getOrbit(now)->positionAtTime(now - 0.01);
    Eigen::Vector3d vd = pos1 - pos0;
#endif

    // Adjust the amount of triangles used for the comet tail based on
    // the screen size of the comet.
    float lod = std::clamp(discSizeInPixels / 1000.0f, 0.2f, 1.0f);
    auto nTailPoints = static_cast<int>(MaxCometTailPoints * lod);
    auto nTailSlices = static_cast<int>(MaxCometTailSlices * lod);

    float irradiance_max = 0.0f;
    // Find the sun with the largest irrradiance of light onto the comet
    // as function of the comet's position;
    // irradiance = sun's luminosity / square(distanceFromSun);
    Eigen::Vector3d sunPos(Eigen::Vector3d::Zero());
    for (const auto star : m_renderer.getNearStars())
    {
        if (!calibrated && star->getVisibility())
        {
            Eigen::Vector3d p = star->getPosition(now).offsetFromKm(observer.getPosition());
            float distanceFromSun = static_cast<float>((pos.cast<double>() - p).norm());
            float irradiance = star->getBolometricLuminosity() / math::square(distanceFromSun);

            if (irradiance > irradiance_max)
            {
                irradiance_max = irradiance;
                sunPos = p;
            }
        }
    }
    if (calibrated)
        sunPos = dustLights.front()->position;

    float fadeDistance = 1.0f / (CometTailAttenDistSol * std::sqrt(irradiance_max));

    // direction to sun with dominant light irradiance:
    Eigen::Vector3f sunDir = (pos.cast<double>() - sunPos).cast<float>().normalized();

    float dustTailRadius = dustTailLength * 0.1f;

    Eigen::Vector3f origin = -sunDir * (body.getRadius() * 100);

    for (int i = 0; i < nTailPoints; i++)
    {
        float alpha = static_cast<float>(i) / static_cast<float>(calibrated ? nTailPoints - 1 : nTailPoints);
        alpha = alpha * alpha;
        cometPoints[i] = origin + sunDir * (dustTailLength * alpha);
    }

    // We need three axes to define the coordinate system for rendering the
    // comet. The first axis is the sun-to-comet direction, and the other
    // two are chose orthogonal to each other and the primary axis.
    Eigen::Vector3f v = (cometPoints[1] - cometPoints[0]).normalized();
    Eigen::Quaternionf q = body.getEclipticToEquatorial(now).cast<float>();
    Eigen::Vector3f u = v.unitOrthogonal();
    Eigen::Vector3f w = u.cross(v);

    for (int i = 0; i < nTailPoints; i++)
    {
        float brightness = 1.0f - static_cast<float>(i) / static_cast<float>(nTailPoints - 1);
        Eigen::Vector3f v0, v1;
        float w0, w1;
        // Special case for the first vertex in the comet tail
        if (i == 0)
        {
            v0 = cometPoints[1] - cometPoints[0];
            v0.normalize();
            v1 = v0;
            w0 = 1.0f;
            w1 = 0.0f;
        }
        else
        {
            v0 = cometPoints[i] - cometPoints[i - 1];
            float sectionLength = v0.norm();
            v0.normalize();

            if (i == nTailPoints - 1)
            {
                v1 = v0;
            }
            else
            {
                v1 = (cometPoints[i + 1] - cometPoints[i]).normalized();
                q.setFromTwoVectors(v0, v1);
                Eigen::Matrix3f rot = q.toRotationMatrix();
                u = rot * u;
                v = rot * v;
                w = rot * w;
            }
            float dr = dustTailRadius / static_cast<float>(nTailPoints) / sectionLength;
            w0 = std::atan(dr);
            float d = std::sqrt(1.0f + w0 * w0);
            w1 = 1.0f / d;
            w0 = w0 / d;
        }

        float radius = static_cast<float>(i) /
                       static_cast<float>(calibrated ? nTailPoints - 1 : nTailPoints) * dustTailRadius;
        for (int j = 0; j < nTailSlices; j++)
        {
            float theta = 2.0f * std::numbers::pi_v<float> * static_cast<float>(j) / static_cast<float>(nTailSlices);
            float s, c;
            math::sincos(theta, s, c);
            CometTailVertex& vtx = m_vertices[i * nTailSlices + j];
            if (calibrated)
                vtx.normal = (u * s + w * c) * (2.0f * radius) - v * (0.1f * dustTailRadius);
            else
                vtx.normal = u * (s * w1) + w * (c * w1) + v * w0;
            vtx.normal.normalize();
            s *= radius;
            c *= radius;

            vtx.point = cometPoints[i] + u * s + w * c;
            vtx.brightness = brightness;
        }
    }

    BuildIndexList(static_cast<ushort>(nTailPoints-1), static_cast<ushort>(nTailSlices), m_indices.get());
    int count = IndexListCapacity(nTailSlices, nTailPoints);
    if (calibrated)
    {
        int center = nTailPoints * nTailSlices;
        m_vertices[center] = { cometPoints[nTailPoints - 1], sunDir, 0.0f };
        for (int j = 0; j < nTailSlices; ++j)
        {
            m_vertices[center + 1 + j] = {
                m_vertices[center - nTailSlices + j].point, sunDir, 0.0f };
            m_indices[count + 3 * j] = static_cast<ushort>(center);
            m_indices[count + 3 * j + 1] = static_cast<ushort>(center + 1 + j);
            m_indices[count + 3 * j + 2] = static_cast<ushort>(center + 1 + (j + 1) % nTailSlices);
        }
    }

    // If fadeDistFromSun = x/x0 >= 1.0, comet tail starts fading,
    // i.e. fadeFactor quickly transits from 1 to 0.
    float fadeFactor = calibrated ? 1.0f :
        0.5f * (1.0f - std::tanh(fadeDistance - 1.0f / fadeDistance));

    Renderer::PipelineState ps;
    ps.blending = true;
    ps.blendFunc.src = calibrated ? GL_ONE : GL_SRC_ALPHA;
    ps.blendFunc.dst = GL_ONE;
    ps.depthTest = true;
    m_renderer.setPipelineState(ps);

    m_prog->use();
    m_prog->setMVPMatrices(*m.projection, (*m.modelview) * math::translate(pos));
    m_prog->vec3Param("color") = GetBodyFeaturesManager()->getCometTailColor(&body).toVector3();
    m_prog->vec3Param("viewDir") = pos.normalized();
    m_prog->floatParam("fadeFactor") = fadeFactor;
    m_prog->intParam("calibratedDust") = calibrated ? 1 : 0;
    m_prog->vec3Param("tailOrigin") = origin;
    m_prog->floatParam("tailLength") = dustTailLength;
    if (calibrated)
    {
        double sourceDistance = std::max((pos.cast<double>() - sunPos).norm(),
                                        static_cast<double>(dustLights.front()->radius));
        double distanceAU = astro::kilometersToAU(sourceDistance);
        double angularWidth = m_renderer.getLogicalPixelAngularWidth();
        auto columnScale = engine::detail::cometDustColumnScale(
            parameters.dustCrossSection, distanceAU, parameters.activityExponent, dustTailRadius, angularWidth);
        if (!columnScale.has_value())
        {
            util::GetLogger()->error("Nonfinite or unrepresentable comet dust column for {}\n", body.getName());
            return;
        }
        m_prog->floatParam("columnScale") = *columnScale;
        m_prog->floatParam("dustAsymmetry") = parameters.asymmetry;
        m_prog->vec3Param("cameraTail") = (-pos - origin) / dustTailLength;
        m_prog->vec3Param("tailAxis") = sunDir;
        m_prog->intParam("dustLightCount") = static_cast<int>(dustLights.size());
        const auto tint = GetBodyFeaturesManager()->getCometTailColor(&body).linearize().toVector3();
        for (std::size_t i = 0; i < dustLights.size(); ++i)
        {
            const auto& light = *dustLights[i];
            Eigen::Vector3f position = ((light.position - pos.cast<double>()).cast<float>() - origin) / dustTailLength;
            float radius = light.radius / dustTailLength;
            auto index = std::to_string(i);
            m_prog->vec4Param(("dustLightPosition[" + index + "]").c_str()) =
                Eigen::Vector4f(position.x(), position.y(), position.z(), std::max(radius * radius, 1.0e-16f));
            Eigen::Vector3f lightColor = light.color.toVector3().cwiseProduct(tint) *
                (astro::lumToIrradiance(light.luminosity, dustTailLength) * parameters.albedo);
            if (!position.allFinite() || !lightColor.allFinite() || !std::isfinite(radius * radius))
            {
                util::GetLogger()->error("Nonfinite comet dust illumination for {}\n", body.getName());
                return;
            }
            m_prog->vec3Param(("dustLightColor[" + index + "]").c_str()) = lightColor;
        }
    }

    int vertexCount = nTailPoints * nTailSlices + (calibrated ? nTailSlices + 1 : 0);
    int indexCount = count + (calibrated ? 3 * nTailSlices : 0);
    m_bo->invalidateData().setSubData(0, std::as_bytes(std::span(m_vertices.get(), vertexCount)));
    m_ibo->invalidateData().setSubData(0, std::as_bytes(std::span(m_indices.get(), indexCount)));

    glDisable(GL_CULL_FACE);
    m_vo.draw(count);
    if (calibrated)
        m_vo.draw(gl::VertexObject::Primitive::Triangles, 3 * nTailSlices, count);
    glEnable(GL_CULL_FACE);
}

} // namespace celestia::render
