// Copyright (C) 2026, the Celestia Development Team

#include "brunetonatmosphereresource.h"

#include <utility>

#include <celutil/logger.h>

namespace celestia::render
{

namespace
{

GLenum
drainErrors()
{
    GLenum first = GL_NO_ERROR;
    for (GLenum error = glGetError();
         error != GL_NO_ERROR;
         error = glGetError())
    {
        if (first == GL_NO_ERROR)
            first = error;
    }
    return first;
}

GLuint
createTexture2D(const engine::BrunetonTextureData& data, bool linearFiltering)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0)
        return 0;

    glBindTexture(GL_TEXTURE_2D, texture);
    const GLint filter = linearFiltering ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA32F,
                 static_cast<GLsizei>(data.width),
                 static_cast<GLsizei>(data.height),
                 0,
                 GL_RGBA,
                 GL_FLOAT,
                 data.texels.data());
    return texture;
}

GLuint
createTexture3D(const engine::BrunetonTextureData& data)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0)
        return 0;

    glBindTexture(GL_TEXTURE_3D, texture);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_3D,
                 0,
                 GL_RGBA16F,
                 static_cast<GLsizei>(data.width),
                 static_cast<GLsizei>(data.height),
                 static_cast<GLsizei>(data.depth),
                 0,
                 GL_RGBA,
                 GL_FLOAT,
                 data.texels.data());
    return texture;
}

} // namespace

BrunetonAtmosphereResource::~BrunetonAtmosphereResource()
{
    release();
}

BrunetonAtmosphereResource::BrunetonAtmosphereResource(
    BrunetonAtmosphereResource&& other) noexcept :
    m_parameters(other.m_parameters),
    m_transmittance(std::exchange(other.m_transmittance, 0)),
    m_phase(std::exchange(other.m_phase, 0)),
    m_scattering(std::exchange(other.m_scattering, 0)),
    m_singleMie(std::exchange(other.m_singleMie, 0)),
    m_irradiance(std::exchange(other.m_irradiance, 0)),
    m_gpuBytes(std::exchange(other.m_gpuBytes, 0)),
    m_manualFloatFiltering(std::exchange(other.m_manualFloatFiltering, false))
{
}

BrunetonAtmosphereResource&
BrunetonAtmosphereResource::operator=(BrunetonAtmosphereResource&& other) noexcept
{
    if (this != &other)
    {
        release();
        m_parameters = other.m_parameters;
        m_transmittance = std::exchange(other.m_transmittance, 0);
        m_phase = std::exchange(other.m_phase, 0);
        m_scattering = std::exchange(other.m_scattering, 0);
        m_singleMie = std::exchange(other.m_singleMie, 0);
        m_irradiance = std::exchange(other.m_irradiance, 0);
        m_gpuBytes = std::exchange(other.m_gpuBytes, 0);
        m_manualFloatFiltering =
            std::exchange(other.m_manualFloatFiltering, false);
    }
    return *this;
}

bool
BrunetonAtmosphereResource::upload(const engine::BrunetonAtmosphereData& data)
{
    release();

    // Debug-only diagnostics: bracket this upload so unrelated GL errors are
    // distinguishable from errors produced while creating the LUT textures.
    if (const GLenum previousError = drainErrors(); previousError != GL_NO_ERROR)
    {
        util::GetLogger()->warn(
            "Cleared pre-existing GL error 0x{:x} before uploading Bruneton atmosphere LUTs.\n",
            previousError);
    }

    GLint previous2D = 0;
    GLint previous3D = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous2D);
    glGetIntegerv(GL_TEXTURE_BINDING_3D, &previous3D);
    const auto restoreBindings = [previous2D, previous3D]()
    {
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous2D));
        glBindTexture(GL_TEXTURE_3D, static_cast<GLuint>(previous3D));
    };

#ifdef GL_ES
    const bool floatLinearFiltering = gl::OES_texture_float_linear;
#else
    constexpr bool floatLinearFiltering = true;
#endif

    m_transmittance = createTexture2D(data.transmittance, floatLinearFiltering);
    m_phase = createTexture2D(data.phase, floatLinearFiltering);
    m_scattering = createTexture3D(data.scattering);
    m_singleMie = createTexture3D(data.singleMie);
    m_irradiance = createTexture2D(data.irradiance, floatLinearFiltering);

    const bool complete =
        m_transmittance != 0 &&
        m_phase != 0 &&
        m_scattering != 0 &&
        m_singleMie != 0 &&
        m_irradiance != 0;
    const GLenum error = drainErrors();
    if (!complete || error != GL_NO_ERROR)
    {
        util::GetLogger()->error(
            "Failed to upload Bruneton atmosphere LUTs (GL error 0x{:x}).\n",
            error);
        release();
        restoreBindings();
        return false;
    }

    m_parameters = data.parameters;
    const auto texelCount = [](const engine::BrunetonTextureData& texture)
    {
        return static_cast<std::size_t>(texture.width) *
               texture.height * texture.depth;
    };
    m_gpuBytes =
        (texelCount(data.transmittance) + texelCount(data.phase) +
         texelCount(data.irradiance)) *
            4u * sizeof(float) +
        (texelCount(data.scattering) + texelCount(data.singleMie)) *
            4u * sizeof(std::uint16_t);
    m_manualFloatFiltering = !floatLinearFiltering;
    restoreBindings();
    return true;
}

std::size_t
BrunetonAtmosphereResource::gpuBytes() const noexcept
{
    if (m_transmittance == 0)
        return 0;

    return m_gpuBytes;
}

void
BrunetonAtmosphereResource::release() noexcept
{
    if (m_transmittance != 0)
        glDeleteTextures(1, &m_transmittance);
    if (m_phase != 0)
        glDeleteTextures(1, &m_phase);
    if (m_scattering != 0)
        glDeleteTextures(1, &m_scattering);
    if (m_singleMie != 0)
        glDeleteTextures(1, &m_singleMie);
    if (m_irradiance != 0)
        glDeleteTextures(1, &m_irradiance);
    m_transmittance = 0;
    m_phase = 0;
    m_scattering = 0;
    m_singleMie = 0;
    m_irradiance = 0;
    m_gpuBytes = 0;
    m_manualFloatFiltering = false;
}

} // namespace celestia::render
