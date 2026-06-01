// brunetonatmosphererenderer.cpp
//
// Copyright (C) 2026, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "brunetonatmosphererenderer.h"

#include <array>

#include <celengine/glsupport.h>
#include <celengine/render.h>
#include <celengine/shadermanager.h>
#include <celrender/brunetonatmosphereresource.h>
#include <celrender/gl/buffer.h>
#include <celrender/gl/vertexobject.h>

namespace celestia::render
{

namespace
{

// The shared atmosphere program binds its single uniform block to this
// binding point. Kept at 0 since this is currently the only UBO Celestia
// uses, and the binding point is process-local to the program object.
constexpr GLuint AtmosphereUboBinding = 0;

constexpr GLint TransmittanceUnit = 0;
constexpr GLint ScatteringUnit    = 1;
constexpr GLint SingleMieUnit     = 2;
constexpr GLint IrradianceUnit    = 3;

void
bindLut(GLint unit, GLenum target, GLuint tex)
{
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(target, tex);
}

} // namespace

BrunetonAtmosphereRenderer::BrunetonAtmosphereRenderer(Renderer& renderer) :
    m_renderer(renderer)
{
}

BrunetonAtmosphereRenderer::~BrunetonAtmosphereRenderer() = default;

void
BrunetonAtmosphereRenderer::initGL()
{
    if (m_initialized)
        return;

    // Triangle-strip covering the full NDC quad. The atmosphere vertex
    // shader pushes z to the far plane and forwards xy as v_ndc.
    static const std::array<float, 8> quad = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };

    m_quadBuffer = std::make_unique<gl::Buffer>(gl::Buffer::TargetHint::Array, quad);
    m_quadVao    = std::make_unique<gl::VertexObject>(gl::VertexObject::Primitive::TriangleStrip);
    m_quadVao->setCount(4);
    m_quadVao->addVertexBuffer(
        *m_quadBuffer,
        CelestiaGLProgram::VertexCoordAttributeIndex,
        2,
        gl::VertexObject::DataType::Float,
        false,
        2 * sizeof(float),
        0);

    m_initialized = true;
}

void
BrunetonAtmosphereRenderer::render(const BrunetonAtmosphereResource& resource,
                                   const FrameParams&                params)
{
    if (!resource.isReady())
        return;

    auto* prog = m_renderer.getShaderManager().getShader(StaticShader::Atmosphere);
    if (prog == nullptr)
        return;

    initGL();

    prog->use();

    // Bind the per-body UBO at AtmosphereUboBinding for this program.
    GLint progId = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &progId);
    const GLuint blockIdx = glGetUniformBlockIndex(static_cast<GLuint>(progId), "AtmosphereBlock");
    if (blockIdx != GL_INVALID_INDEX)
    {
        glUniformBlockBinding(static_cast<GLuint>(progId), blockIdx, AtmosphereUboBinding);
        glBindBufferBase(GL_UNIFORM_BUFFER, AtmosphereUboBinding, resource.ubo());
    }

    // Bind the four LUT samplers. single_mie_scattering_texture is
    // declared unconditionally in the shader; in combined mode we
    // alias it to the scattering unit so a sampler3D mismatch can't
    // produce GL_INVALID_OPERATION on draw.
    prog->samplerParam("transmittance_texture")          = TransmittanceUnit;
    prog->samplerParam("scattering_texture")             = ScatteringUnit;
    prog->samplerParam("irradiance_texture")             = IrradianceUnit;
    const GLuint singleMieTex = resource.singleMieTexture() != 0
                                  ? resource.singleMieTexture()
                                  : resource.scatteringTexture();
    prog->samplerParam("single_mie_scattering_texture") =
        resource.singleMieTexture() != 0 ? SingleMieUnit : ScatteringUnit;

    bindLut(TransmittanceUnit, GL_TEXTURE_2D, resource.transmittanceTexture());
    bindLut(ScatteringUnit,    GL_TEXTURE_3D, resource.scatteringTexture());
    if (resource.singleMieTexture() != 0)
        bindLut(SingleMieUnit, GL_TEXTURE_3D, singleMieTex);
    bindLut(IrradianceUnit,    GL_TEXTURE_2D, resource.irradianceTexture());

    // Per-frame uniforms.
    prog->mat4Param("inv_projection")    = params.inv_projection;
    prog->mat4Param("inv_modelview_km")  = params.inv_modelview_km;
    prog->vec3Param("camera_km")         = params.camera_km;
    prog->vec3Param("sun_direction")     = params.sun_direction;
    prog->vec3Param("white_point")       = params.white_point;
    prog->floatParam("exposure")         = params.exposure;

    // The vertex shader emits clip-space at z=1 (far plane). Identity
    // MVP so that set_vp's MVPMatrix * in_Position is a no-op.
    prog->setMVPMatrices(Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity());

    m_quadVao->draw();

    glActiveTexture(GL_TEXTURE0);
}

} // namespace celestia::render
