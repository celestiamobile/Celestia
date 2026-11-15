// starfield.cpp
//
// Copyright (C) 2001-present, the Celestia Development Team
// Original version by Chris Laurel <claurel@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include <celrender/gl/vertexobject.h>
#include <celrender/legacylargestarrenderer.h>
#include <cmath>
#include <celutil/color.h>
#include "glsupport.h"
#include "objectrenderer.h"
#include "shadermanager.h"
#include "render.h"
#include "texture.h"
#include "pointstarvertexbuffer.h"
#include "starpipelineowner.h"

namespace gl = celestia::gl;
namespace util = celestia::util;

PointStarVertexBuffer::PointStarVertexBuffer(const Renderer &renderer,
                                             capacity_t capacity) :
    m_renderer(renderer),
    m_capacity(capacity),
    m_vertices(std::make_unique<StarVertex[]>(capacity))
{
}

PointStarVertexBuffer::~PointStarVertexBuffer() = default;

void PointStarVertexBuffer::startSprites(bool calibrated, bool visualOnly)
{
    m_calibrated = calibrated;
    m_prog = m_renderer.getShaderManager().getShader(
        calibrated ? StaticShader::CalibratedStar : StaticShader::Star,
        visualOnly ? StaticShaderOptions::VisualOnly : StaticShaderOptions::None);
    m_pointSizeFromVertex = true;
}

void PointStarVertexBuffer::startBasicPoints(bool calibrated)
{
    m_calibrated = calibrated;
    m_pointSizeFromVertex = false;
    if (calibrated)
    {
        m_prog = m_renderer.getShaderManager().getShader(StaticShader::CalibratedStar);
        return;
    }
    ShaderProperties shadprop;
    shadprop.texUsage = TexUsage::VertexColors | TexUsage::StaticPointSize;
    shadprop.lightModel = LightingModel::UnlitModel;
    m_prog = m_renderer.getShaderManager().getShader(shadprop);
    m_pointSizeFromVertex = false;
}

void PointStarVertexBuffer::render()
{
    if (m_nStars == 0 || m_prog == nullptr)
        return;

    makeCurrent();

    if (!m_calibrated && m_texture != nullptr)
        m_texture->bind();

    m_bo->invalidateData().setSubData(0, std::as_bytes(std::span(m_vertices.get(), m_nStars)));

#ifndef GL_ES
    glEnable(GL_PROGRAM_POINT_SIZE);
#endif
    if (m_pointSizeFromVertex)
        m_vo1->draw(m_nStars);
    else
        m_vo2->draw(m_nStars);
#ifndef GL_ES
    glDisable(GL_PROGRAM_POINT_SIZE);
#endif
    m_nStars = 0;
}

void PointStarVertexBuffer::makeCurrent()
{
    auto &owner = m_renderer.starPipelineOwner();
    if (m_prog == nullptr)
        return;

    owner.setActive(this);  // flushes whoever held the pipeline before

    setupVertexArrayObject();

    m_prog->use();
    m_prog->setMVPMatrices(m_renderer.getCurrentProjectionMatrix(), m_renderer.getCurrentModelViewMatrix());
    if (m_calibrated)
    {
        m_prog->floatParam("pointMode") = m_pointSizeFromVertex ? 0.0f : 1.0f;
        m_prog->floatParam("pointScale") = m_pointScale;
    }
    if (m_pointSizeFromVertex)
    {
        m_prog->samplerParam("starTex") = 0;
    }
    else
    {
        m_prog->pointScale = m_pointScale;
        glVertexAttrib1f(CelestiaGLProgram::PointSizeAttributeIndex,
                        m_calibrated ? std::max(1.0f, std::ceil(m_pointScale)) : 1.0f);
    }
}

void PointStarVertexBuffer::setupVertexArrayObject()
{
    if (m_initialized)
        return;

    m_initialized = true;

    m_bo = gl::Buffer::create(gl::Buffer::TargetHint::Array,
                              static_cast<GLsizeiptr>(sizeof(StarVertex) * m_capacity),
                              gl::Buffer::BufferUsage::StreamDraw);
    m_vo1 = std::make_unique<gl::VertexObject>(gl::VertexObject::Primitive::Points);
    m_vo2 = std::make_unique<gl::VertexObject>(gl::VertexObject::Primitive::Points);

    m_vo1->addVertexBuffer(
        m_bo,
        CelestiaGLProgram::VertexCoordAttributeIndex,
        3,
        gl::VertexObject::DataType::Float,
        false,
        sizeof(StarVertex),
        offsetof(StarVertex, position));

    m_vo1->addVertexBuffer(
        m_bo,
        CelestiaGLProgram::ColorAttributeIndex,
        4,
        gl::VertexObject::DataType::UnsignedByte,
        true,
        sizeof(StarVertex),
        offsetof(StarVertex, color));

    m_vo1->addVertexBuffer(
        m_bo,
        CelestiaGLProgram::PointSizeAttributeIndex,
        1,
        gl::VertexObject::DataType::Float,
        false,
        sizeof(StarVertex),
        offsetof(StarVertex, size));

    m_vo2->addVertexBuffer(
        m_bo,
        CelestiaGLProgram::VertexCoordAttributeIndex,
        3,
        gl::VertexObject::DataType::Float,
        false,
        sizeof(StarVertex),
        offsetof(StarVertex, position));

    m_vo2->addVertexBuffer(
        m_bo,
        CelestiaGLProgram::ColorAttributeIndex,
        4,
        gl::VertexObject::DataType::UnsignedByte,
        true,
        sizeof(StarVertex),
        offsetof(StarVertex, color));

    for (auto *vo : {m_vo1.get(), m_vo2.get()})
        vo->addVertexBuffer(m_bo, CelestiaGLProgram::IntensityAttributeIndex, 1,
                            gl::VertexObject::DataType::Float, false,
                            sizeof(StarVertex), offsetof(StarVertex, irradiance));
}

void PointStarVertexBuffer::finish()
{
    render();
    if (m_largeActive)
    {
        m_largeRenderer->finish();
        m_largeActive = false;
    }
    m_renderer.starPipelineOwner().clearIfActive(this);
}

void PointStarVertexBuffer::addStar(const Eigen::Vector3f &pos, const Color &color,
                                    float size, float irradiance)
{
    if (m_calibrated && m_pointSizeFromVertex && std::ceil(size) > gl::maxPointSize)
    {
        if (m_largeRenderer == nullptr)
            m_largeRenderer = std::make_unique<celestia::render::LegacyLargeStarRenderer>(
                m_renderer, m_capacity, true);
        if (!m_largeActive)
        {
            m_largeRenderer->start();
            m_largeActive = true;
        }
        m_largeRenderer->addStar(pos, color, size, 0.0f, irradiance);
        return;
    }
    assert(m_nStars < m_capacity);
    m_vertices[m_nStars].position = pos;
    m_vertices[m_nStars].size = size;
    m_vertices[m_nStars].irradiance = irradiance;
    color.get(m_vertices[m_nStars].color);
    ++m_nStars;
    if (m_nStars == m_capacity)
        render();
}

void PointStarVertexBuffer::setTexture(Texture *texture)
{
    m_texture = texture;
}

void PointStarVertexBuffer::setPointScale(float pointSize)
{
    m_pointScale = pointSize;
}
