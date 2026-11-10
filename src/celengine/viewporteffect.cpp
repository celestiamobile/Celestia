//
// viewporteffect.cpp
//
// Copyright © 2020 Celestia Development Team. All rights reserved.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "viewporteffect.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

#include "framebuffer.h"
#include "render.h"
#include "warpmesh.h"

namespace gl = celestia::gl;
namespace util = celestia::util;

namespace
{

void prepareEffect(Renderer* renderer, CelestiaGLProgram* prog,
                   FramebufferObject* fbo, bool preserveDepth)
{
    Renderer::PipelineState ps;
    ps.depthTest = preserveDepth;
    ps.depthMask = preserveDepth;
    renderer->setPipelineState(ps);
    prog->samplerParam("tex") = 0;
    prog->samplerParam("depthTex") = 1;
    prog->intParam("preserveDepth") = preserveDepth ? 1 : 0;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, preserveDepth ? fbo->depthTexture() : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fbo->colorTexture());
}

} // namespace

bool ViewportEffect::preprocess(Renderer* renderer, FramebufferObject* fbo)
{
    return fbo->bind();
}

bool ViewportEffect::prerender(Renderer* renderer, FramebufferObject* fbo, FramebufferObject* dst,
                              bool preserveDepth)
{
    auto attachments = sourceAttachments();
    if (preserveDepth)
        attachments |= FramebufferObject::Attachment::Depth;
    // Discard unused attachments now. MSAA path skips this since resolve()
    // invalidates the MSAA renderbuffers itself.
    if (!fbo->isMultisample())
    {
        constexpr auto allAttachments =
            FramebufferObject::Attachment::Color | FramebufferObject::Attachment::Depth;
        fbo->discard(allAttachments & ~attachments);
    }

    // For renderbuffer MSAA (desktop GL / GLES3), blit the MSAA color buffer into
    // the resolve texture before switching to the destination framebuffer
    if (!fbo->resolve(attachments))
        return false;

    if (!dst->bind())
        return false;

    // Wrapped caller framebuffers have no size metadata; retain their viewport.
    if (dst->width() != 0 && dst->height() != 0)
    {
        renderer->removeScissor();
        renderer->setViewport(0, 0, dst->width(), dst->height());
    }

    Renderer::PipelineState ps;
    ps.depthMask = true;
    renderer->setPipelineState(ps);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return true;
}

bool ViewportEffect::distortXY(float &x, float &y)
{
    return true;
}

void ViewportEffect::mapToOutput(const Eigen::Vector2f& position, int, int,
                                std::vector<Eigen::Vector2f>& output) const
{
    output.push_back(position);
}

PassthroughViewportEffect::PassthroughViewportEffect(bool needsFloatSource, bool convertToSRGB) :
    m_needsFloatSource(needsFloatSource),
    m_convertToSRGB(convertToSRGB)
{
}

bool PassthroughViewportEffect::render(Renderer* renderer, FramebufferObject* fbo, int width, int height,
                                      bool preserveDepth)
{
    bool toneMap = m_convertToSRGB && renderer->getToneMappingMode() != ToneMappingMode::None;
    auto *prog = renderer->getShaderManager().getShader(
        StaticShader::sRGB,
        toneMap ? StaticShaderOptions::ToneMap : StaticShaderOptions::None);
    if (prog == nullptr)
        return false;

    prog->use();
    prog->intParam("convertToSRGB") = m_convertToSRGB ? 1 : 0;
    if (toneMap)
        prog->floatParam("exposure") = renderer->getToneMappingExposure();
    prepareEffect(renderer, prog, fbo, preserveDepth);
    drawQuad();
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

void PassthroughViewportEffect::drawQuad()
{
    initialize();
    vo.draw();
}

FsrViewportEffect::FsrViewportEffect(Pass pass, float parameter) :
    PassthroughViewportEffect(false, false),
    m_pass(pass),
    m_parameter(parameter)
{
}

Eigen::Vector2i FsrViewportEffect::inputSize(const Eigen::Vector2i& outputSize) const
{
    if (m_pass == Pass::Sharpen)
        return outputSize;

    return {
        std::max(1, static_cast<int>(std::ceil(outputSize.x() / m_parameter))),
        std::max(1, static_cast<int>(std::ceil(outputSize.y() / m_parameter))),
    };
}

bool FsrViewportEffect::render(Renderer* renderer, FramebufferObject* fbo,
                               int width, int height, bool preserveDepth)
{
    auto* prog = renderer->getShaderManager().getShader(
        m_pass == Pass::Upscale ? StaticShader::FsrEasu : StaticShader::FsrRcas);
    if (prog == nullptr)
        return false;

    prog->use();
    prog->vec2Param("outputSize") = Eigen::Vector2f(width, height);
    prog->floatParam("sharpness") = m_parameter;
    prepareEffect(renderer, prog, fbo, preserveDepth);
    drawQuad();
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

void PassthroughViewportEffect::initialize()
{
    if (initialized)
        return;
    initialized = true;

    static constexpr std::array quadVertices = {
        // positions   // texCoords
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,

        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f
    };

    vo = gl::VertexObject(gl::VertexObject::Primitive::Triangles);
    auto bo = gl::Buffer::create(gl::Buffer::TargetHint::Array,
                                 std::as_bytes(std::span{quadVertices}),
                                 gl::Buffer::BufferUsage::StaticDraw);

    vo.setCount(6);
    vo.addVertexBuffer(
        bo,
        CelestiaGLProgram::VertexCoordAttributeIndex,
        2,
        gl::VertexObject::DataType::Float,
        false,
        4 * sizeof(float),
        0);
    vo.addVertexBuffer(
        bo,
        CelestiaGLProgram::TextureCoord0AttributeIndex,
        2,
        gl::VertexObject::DataType::Float,
        false,
        4 * sizeof(float),
        2 * sizeof(float));
}

WarpMeshViewportEffect::WarpMeshViewportEffect(std::unique_ptr<WarpMesh>&& _mesh) :
    mesh(std::move(_mesh))
{
}

WarpMeshViewportEffect::~WarpMeshViewportEffect() = default;

bool WarpMeshViewportEffect::prerender(Renderer* renderer, FramebufferObject* fbo, FramebufferObject* dst,
                                     bool preserveDepth)
{
    if (mesh == nullptr)
        return false;

    return ViewportEffect::prerender(renderer, fbo, dst, preserveDepth);
}

bool WarpMeshViewportEffect::render(Renderer* renderer, FramebufferObject* fbo, int width, int height,
                                   bool preserveDepth)
{
    auto *prog = renderer->getShaderManager().getShader(StaticShader::WarpMesh);
    if (prog == nullptr)
        return false;

    initialize();

    prog->use();
    prog->floatParam("screenRatio") = (float)height / width;
    prepareEffect(renderer, prog, fbo, preserveDepth);
    vo.draw();
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

void WarpMeshViewportEffect::initialize()
{
    if (initialized)
        return;
    initialized = true;

    vo = gl::VertexObject(gl::VertexObject::Primitive::Triangles);
    mesh->setUpVertexObject(vo);
}

bool WarpMeshViewportEffect::distortXY(float &x, float &y)
{
    if (mesh == nullptr)
        return false;

    float u;
    float v;
    if (!mesh->mapVertex(x * 2.0f, y * 2.0f, u, v))
        return false;

    x = u / 2.0f;
    y = v / 2.0f;
    return true;
}

void WarpMeshViewportEffect::mapToOutput(const Eigen::Vector2f& position, int width, int height,
                                        std::vector<Eigen::Vector2f>& output) const
{
    if (mesh == nullptr)
        return;

    const auto first = output.size();
    const float screenRatio = static_cast<float>(height) / width;
    const float offset = 0.5f - screenRatio * 0.5f;
    mesh->mapTexture((position.x() / width - offset) / screenRatio,
                     position.y() / height, output);
    for (auto i = first; i < output.size(); ++i)
    {
        output[i].x() = (output[i].x() * screenRatio + 1.0f) * 0.5f * width;
        output[i].y() = (output[i].y() + 1.0f) * 0.5f * height;
    }
}
