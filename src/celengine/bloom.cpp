// Copyright (C) 2026 Celestia Development Team. All rights reserved.
// Licensed under the GNU General Public License, version 2 or later.

#include "bloom.h"

#include "render.h"
#include "shadermanager.h"

#include <algorithm>
#include <array>
#include <bit>
#include <celrender/gl/buffer.h>
#include <celutil/logger.h>
#include <span>

namespace gl = celestia::gl;
using celestia::util::GetLogger;

bool
Bloom::initialize(GLuint width, GLuint height)
{
    const int levels = std::max(1, static_cast<int>(std::bit_width(std::min(width, height))) - 4);
    width            = std::max(1u, width / 2);
    height           = std::max(1u, height / 2);
    if (!m_levels.empty()
        && (m_levels.front()->width() != width || m_levels.front()->height() != height
            || m_levels.size() != static_cast<std::size_t>(levels)))
        m_levels.clear();
    if (m_levels.empty())
    {
        for (int level = 0; level < levels; ++level)
        {
            auto fbo = std::make_unique<FramebufferObject>(
                width,
                height,
                FramebufferObject::Attachment::Color,
                1,
                true);
            if (!fbo->isValid())
            {
                GetLogger()->error("Unable to allocate HDR bloom buffers.\n");
                m_levels.clear();
                return false;
            }
            m_levels.push_back(std::move(fbo));
            if (width == 1 && height == 1)
                break;
            width  = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
    }
    if (!m_initialized)
    {
        constexpr std::array vertices{
            -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f,
            -1.0f, 1.0f,  0.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f,
        };
        m_quad      = gl::VertexObject(gl::VertexObject::Primitive::TriangleStrip);
        auto buffer = gl::Buffer::create(
            gl::Buffer::TargetHint::Array,
            std::as_bytes(std::span{ vertices }),
            gl::Buffer::BufferUsage::StaticDraw);
        m_quad.setCount(4);
        m_quad.addVertexBuffer(
            buffer,
            CelestiaGLProgram::VertexCoordAttributeIndex,
            2,
            gl::VertexObject::DataType::Float,
            false,
            4 * sizeof(float),
            0);
        m_quad.addVertexBuffer(
            buffer,
            CelestiaGLProgram::TextureCoord0AttributeIndex,
            2,
            gl::VertexObject::DataType::Float,
            false,
            4 * sizeof(float),
            2 * sizeof(float));
        m_initialized = true;
    }
    return true;
}

bool
Bloom::render(Renderer &renderer, const FramebufferObject &scene)
{
    if (!scene.useFloatColor())
    {
        GetLogger()->error("Bloom requires an HDR scene.\n");
        return false;
    }
    auto *down      = renderer.getShaderManager().getShader(StaticShader::BloomDownsample);
    auto *prefilter = renderer.getShaderManager().getShader(
        StaticShader::BloomDownsample, StaticShaderOptions::BloomPrefilter);
    auto *coarseDown = renderer.getShaderManager().getShader(
        StaticShader::BloomDownsample,
        StaticShaderOptions::BloomCoarse);
    auto *up = renderer.getShaderManager().getShader(StaticShader::BloomUpsample);
    auto *coarseUp = renderer.getShaderManager().getShader(
        StaticShader::BloomUpsample,
        StaticShaderOptions::BloomCoarse);
    if (down == nullptr || prefilter == nullptr || coarseDown == nullptr || up == nullptr
        || coarseUp == nullptr)
    {
        GetLogger()->error("Unable to load HDR bloom shaders.\n");
        return false;
    }
    if (!initialize(scene.width(), scene.height()))
        return false;

    renderer.removeScissor();
    renderer.setPipelineState({});
    glActiveTexture(GL_TEXTURE0);
    const Eigen::Vector2f sceneSize(
        static_cast<float>(scene.width()),
        static_cast<float>(scene.height()));
    GLuint source = scene.colorTexture();
    CelestiaGLProgram *activeDown = nullptr;
    for (std::size_t i = 0; i < m_levels.size(); ++i)
    {
        auto *program = i == 0 ? prefilter : i + 3 >= m_levels.size() ? coarseDown : down;
        if (program != activeDown)
        {
            program->use();
            program->samplerParam("tex") = 0;
            activeDown                   = program;
        }
        auto &target = *m_levels[i];
        if (!target.bind())
        {
            GetLogger()->error("Unable to bind HDR bloom downsample buffer.\n");
            return false;
        }
        glViewport(0, 0, target.width(), target.height());
        glBindTexture(GL_TEXTURE_2D, source);
        m_quad.draw();
        source = target.colorTexture();
    }

    Renderer::PipelineState ps;
    ps.blending  = true;
    ps.blendFunc = { GL_ONE, GL_ONE_MINUS_SRC_ALPHA };
    renderer.setPipelineState(ps);
    CelestiaGLProgram *activeUp = nullptr;
    for (std::size_t i = m_levels.size() - 1; i > 0; --i)
    {
        auto *program = i + 2 >= m_levels.size() ? coarseUp : up;
        if (program != activeUp)
        {
            program->use();
            program->samplerParam("tex")    = 0;
            program->vec2Param("sceneSize") = sceneSize;
            activeUp                        = program;
        }
        auto &target = *m_levels[i - 1];
        if (!target.bind())
        {
            GetLogger()->error("Unable to bind HDR bloom upsample buffer.\n");
            return false;
        }
        glViewport(0, 0, target.width(), target.height());
        glBindTexture(GL_TEXTURE_2D, m_levels[i]->colorTexture());
        program->floatParam("zeroBorder") = i + 1 == m_levels.size() ? 1.0f : 0.0f;
        m_quad.draw();
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}
