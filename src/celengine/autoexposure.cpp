// Copyright (C) 2026 Celestia Development Team. All rights reserved.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "autoexposure.h"

#include "render.h"
#include "shadermanager.h"

#include <algorithm>
#include <array>
#include <celrender/gl/buffer.h>
#include <celutil/logger.h>
#include <cmath>
#include <cstring>
#include <span>

namespace gl = celestia::gl;
using celestia::util::GetLogger;

namespace
{
constexpr int    MeterSize     = 32;
constexpr int    SampleSize    = 256;
constexpr double MeterInterval = 1.0 / 15.0;
} // namespace

AutoExposure::AutoExposure(float initialExposure) :
    m_ev(std::log2(initialExposure)), m_targetEV(m_ev), m_meterExposure(initialExposure)
{
}

AutoExposure::~AutoExposure()
{
    if (m_fence != nullptr)
        glDeleteSync(m_fence);
    if (m_readback != 0)
        glDeleteBuffers(1, &m_readback);
}

bool
AutoExposure::initialize()
{
    if (m_meter != nullptr)
        return true;

    m_samples = std::make_unique<FramebufferObject>(
        SampleSize,
        SampleSize,
        FramebufferObject::Attachment::Color,
        1,
        true);
    m_meter = std::make_unique<FramebufferObject>(
        MeterSize,
        MeterSize,
        FramebufferObject::Attachment::Color,
        1,
        true);
    m_target = std::make_unique<FramebufferObject>(1, 1, FramebufferObject::Attachment::Color);
    if (!m_samples->isValid() || !m_meter->isValid() || !m_target->isValid())
    {
        GetLogger()->error("Unable to allocate auto camera exposure buffers.\n");
        return false;
    }

    GLint previousBuffer = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousBuffer);
    glGenBuffers(1, &m_readback);
    if (m_readback == 0)
    {
        GetLogger()->error("Unable to create auto camera exposure readback.\n");
        return false;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_readback);
    glBufferData(GL_PIXEL_PACK_BUFFER, 4, nullptr, GL_STREAM_READ);
    GLint bufferSize = 0;
    glGetBufferParameteriv(GL_PIXEL_PACK_BUFFER, GL_BUFFER_SIZE, &bufferSize);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, previousBuffer);
    if (bufferSize != 4)
    {
        GetLogger()->error("Unable to allocate auto camera exposure readback.\n");
        return false;
    }

    static constexpr std::array vertices = {
        -1.0f, 1.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f, 1.0f, 0.0f, 1.0f, 1.0f,  -1.0f, 1.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f,
    };
    m_quad      = gl::VertexObject(gl::VertexObject::Primitive::Triangles);
    auto buffer = gl::Buffer::create(
        gl::Buffer::TargetHint::Array,
        std::as_bytes(std::span{ vertices }),
        gl::Buffer::BufferUsage::StaticDraw);
    m_quad.setCount(6);
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
    return true;
}

bool
AutoExposure::readMeasurement()
{
    if (m_fence == nullptr)
        return true;
    GLenum status = glClientWaitSync(m_fence, 0, 0);
    if (status == GL_TIMEOUT_EXPIRED)
        return true;
    if (status == GL_WAIT_FAILED)
    {
        GetLogger()->error("Unable to poll auto camera exposure readback.\n");
        return false;
    }

    if (!m_readbackQueued)
    {
        // Even a PBO read can wait for unfinished rendering on some drivers.
        // Queue the transfer only after the measurement's rendering fence has signaled.
        GLint previousFramebuffer = 0;
        GLint previousBuffer      = 0;
        GLint skipPixels          = 0;
        GLint skipRows            = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousFramebuffer);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousBuffer);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_target->id());
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_readback);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, previousBuffer);
        glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
        glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, previousFramebuffer);
        glDeleteSync(m_fence);
        m_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (m_fence == nullptr)
        {
            GetLogger()->error("Unable to fence auto camera exposure readback.\n");
            return false;
        }
        m_readbackQueued = true;
        glFlush();
        return true;
    }

    GLint previousBuffer = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousBuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_readback);
    const void *mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 4, GL_MAP_READ_BIT);
    if (mapped == nullptr)
    {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, previousBuffer);
        GetLogger()->error("Unable to map auto camera exposure readback.\n");
        return false;
    }
    std::array<unsigned char, 4> bytes;
    std::memcpy(bytes.data(), mapped, bytes.size());
    bool valid = glUnmapBuffer(GL_PIXEL_PACK_BUFFER) == GL_TRUE;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, previousBuffer);
    glDeleteSync(m_fence);
    m_fence = nullptr;
    if (!valid)
    {
        GetLogger()->error("Invalid auto camera exposure readback.\n");
        return false;
    }

    // ExposureTarget packs the display and measurement exposures separately.
    double meterEV = (static_cast<double>(bytes[2]) * 256.0 + bytes[3]) * (64.0 / 65535.0) - 40.0;
    if (meterEV < -39.01 || meterEV > std::log2(MaximumExposure) + 0.01)
    {
        GetLogger()->error("Invalid physical-light meter exposure.\n");
        return false;
    }
    m_meterExposure = static_cast<float>(std::exp2(meterEV));
    if (bytes[0] == 0 && bytes[1] == 0)
    {
        if (meterEV <= -38.99)
        {
            GetLogger()->error("Physical-light meter exceeds its HDR range.\n");
            return false;
        }
        // A clipped measurement adjusts the sensor range, not the displayed exposure.
        return true;
    }
    double targetEV = (static_cast<double>(bytes[0]) * 256.0 + bytes[1]) * (64.0 / 65535.0) - 40.0;
    if (targetEV < -39.01 || targetEV > std::log2(MaximumExposure) + 0.01)
    {
        GetLogger()->error("Invalid auto camera exposure measurement.\n");
        return false;
    }
    m_targetEV = targetEV;
    if (!m_hasMeasurement)
        m_ev = m_targetEV;
    m_hasMeasurement = true;
    return true;
}

bool
AutoExposure::advance(double now)
{
    if (!std::isfinite(now))
    {
        GetLogger()->error("Auto camera exposure requires a finite time.\n");
        return false;
    }
    double dt   = m_lastTime < 0.0 ? 0.0 : std::clamp(now - m_lastTime, 0.0, 0.25);
    double rate = m_targetEV < m_ev ? 3.0 : 1.0;
    m_ev += (m_targetEV - m_ev) * -std::expm1(-rate * dt);
    m_lastTime = now;
    return readMeasurement();
}

float
AutoExposure::exposure() const
{
    return static_cast<float>(std::exp2(m_ev));
}

float
AutoExposure::meterExposure() const
{
    return m_meterExposure;
}

bool
AutoExposure::needsMetering(double now) const
{
    return m_fence == nullptr
           && !(now >= m_lastMeterTime && now - m_lastMeterTime < MeterInterval);
}

bool
AutoExposure::meter(Renderer &renderer, const FramebufferObject &scene, double now)
{
    if (!std::isfinite(now) || !scene.useFloatColor() || scene.meterTexture() == 0)
    {
        GetLogger()->error(
            "Auto camera exposure requires a finite time and an HDR physical-light attachment.\n");
        return false;
    }
    if (!needsMetering(now))
        return true;
    auto *meter  = renderer.getShaderManager().getShader(StaticShader::ExposureMeter);
    auto *target = renderer.getShaderManager().getShader(StaticShader::ExposureTarget);
    if (meter == nullptr || target == nullptr)
    {
        GetLogger()->error("Unable to load auto camera exposure shaders.\n");
        return false;
    }
    if (!initialize())
        return false;

    renderer.removeScissor();
    renderer.setPipelineState({});
    glActiveTexture(GL_TEXTURE0);
    if (!m_samples->bind())
    {
        GetLogger()->error("Unable to bind auto camera exposure samples.\n");
        return false;
    }
    glViewport(0, 0, SampleSize, SampleSize);
    meter->use();
    meter->samplerParam("tex") = 0;
    meter->intParam("outputSize") = SampleSize;
    meter->intParam("reduce") = 0;
    glBindTexture(GL_TEXTURE_2D, scene.meterTexture());
    m_quad.draw();

    if (!m_meter->bind())
    {
        GetLogger()->error("Unable to bind auto camera exposure meter.\n");
        return false;
    }
    glViewport(0, 0, MeterSize, MeterSize);
    meter->intParam("outputSize") = MeterSize;
    meter->intParam("reduce") = 1;
    glBindTexture(GL_TEXTURE_2D, m_samples->colorTexture());
    m_quad.draw();
    glBindTexture(GL_TEXTURE_2D, m_meter->colorTexture());

    if (!m_target->bind())
    {
        GetLogger()->error("Unable to bind auto camera exposure target.\n");
        return false;
    }
    glViewport(0, 0, 1, 1);
    constexpr std::array invalidMeasurement{ 0.0f, 0.0f, 0.0f, 0.0f };
    glClearBufferfv(GL_COLOR, 0, invalidMeasurement.data());
    target->use();
    target->samplerParam("meterTex") = 0;
    target->floatParam("maxEV")      = std::log2(MaximumExposure);
    target->floatParam("middleGray") = MiddleGray;
    target->floatParam("cameraEV")   = std::log2(m_meterExposure);
    m_quad.draw();
    glBindTexture(GL_TEXTURE_2D, 0);

    m_readbackQueued = false;
    m_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (m_fence == nullptr)
    {
        GetLogger()->error("Unable to fence auto camera exposure measurement.\n");
        return false;
    }
    glFlush();
    m_lastMeterTime = now;
    return true;
}
