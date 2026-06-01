// brunetonatmosphereresource.cpp
//
// Copyright (C) 2026, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "brunetonatmosphereresource.h"

#include <cstring>
#include <utility>

#include <celutil/logger.h>

namespace celestia::render
{

using celestia::engine::AtmFileData;
using celestia::engine::AtmIrradianceTextureHeight;
using celestia::engine::AtmIrradianceTextureWidth;
using celestia::engine::AtmScatteringTextureDepth;
using celestia::engine::AtmScatteringTextureHeight;
using celestia::engine::AtmScatteringTextureWidth;
using celestia::engine::AtmTransmittanceTextureHeight;
using celestia::engine::AtmTransmittanceTextureWidth;
using celestia::engine::Std140AtmosphereBlock;
using celestia::util::GetLogger;

namespace
{

GLuint
createLut2D(GLsizei w, GLsizei h, const float* texels)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, texels);
    return tex;
}

GLuint
createLut3D(GLsizei w, GLsizei h, GLsizei d, const float* texels)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_3D, tex);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA32F, w, h, d, 0, GL_RGBA, GL_FLOAT, texels);
    return tex;
}

// Build the UBO bytes from the .atm header: identical to Std140AtmosphereBlock.
Std140AtmosphereBlock
makeUboBlock(const AtmFileData& d)
{
    Std140AtmosphereBlock block{};
    block.ATMOSPHERE = d.header.parameters;
    std::memcpy(block.SKY_SPECTRAL_RADIANCE_TO_LUMINANCE,
                d.header.sky_spectral_radiance_to_luminance,
                sizeof(block.SKY_SPECTRAL_RADIANCE_TO_LUMINANCE));
    std::memcpy(block.SUN_SPECTRAL_RADIANCE_TO_LUMINANCE,
                d.header.sun_spectral_radiance_to_luminance,
                sizeof(block.SUN_SPECTRAL_RADIANCE_TO_LUMINANCE));
    return block;
}

} // namespace

BrunetonAtmosphereResource::~BrunetonAtmosphereResource()
{
    releaseGL();
}

BrunetonAtmosphereResource::BrunetonAtmosphereResource(BrunetonAtmosphereResource&& other) noexcept :
    m_ubo(std::exchange(other.m_ubo, 0)),
    m_transmittance(std::exchange(other.m_transmittance, 0)),
    m_scattering(std::exchange(other.m_scattering, 0)),
    m_singleMie(std::exchange(other.m_singleMie, 0)),
    m_irradiance(std::exchange(other.m_irradiance, 0))
{
}

BrunetonAtmosphereResource&
BrunetonAtmosphereResource::operator=(BrunetonAtmosphereResource&& other) noexcept
{
    if (this != &other)
    {
        releaseGL();
        m_ubo            = std::exchange(other.m_ubo, 0);
        m_transmittance  = std::exchange(other.m_transmittance, 0);
        m_scattering     = std::exchange(other.m_scattering, 0);
        m_singleMie      = std::exchange(other.m_singleMie, 0);
        m_irradiance     = std::exchange(other.m_irradiance, 0);
    }
    return *this;
}

void
BrunetonAtmosphereResource::releaseGL() noexcept
{
    if (m_ubo            != 0) glDeleteBuffers(1, &m_ubo);
    if (m_transmittance  != 0) glDeleteTextures(1, &m_transmittance);
    if (m_scattering     != 0) glDeleteTextures(1, &m_scattering);
    if (m_singleMie      != 0) glDeleteTextures(1, &m_singleMie);
    if (m_irradiance     != 0) glDeleteTextures(1, &m_irradiance);
    m_ubo = m_transmittance = m_scattering = m_singleMie = m_irradiance = 0;
}

bool
BrunetonAtmosphereResource::upload(const AtmFileData& data)
{
    releaseGL();

    // Sanity-check the LUT vector sizes against the dimensions declared in
    // the file's header (LoadAtmFile has already cross-checked the header
    // dimensions against the shader's compile-time constants).
    const std::size_t expectTrans   = static_cast<std::size_t>(AtmTransmittanceTextureWidth) *
                                      AtmTransmittanceTextureHeight * 4;
    const std::size_t expectScatter = static_cast<std::size_t>(AtmScatteringTextureWidth) *
                                      AtmScatteringTextureHeight * AtmScatteringTextureDepth * 4;
    const std::size_t expectIrr     = static_cast<std::size_t>(AtmIrradianceTextureWidth) *
                                      AtmIrradianceTextureHeight * 4;

    if (data.transmittance.size() != expectTrans ||
        data.scattering.size()    != expectScatter ||
        data.irradiance.size()    != expectIrr)
    {
        GetLogger()->error("BrunetonAtmosphereResource::upload: LUT vector size mismatch\n");
        return false;
    }
    if (data.header.combined_scattering == 0 && data.single_mie_scattering.size() != expectScatter)
    {
        GetLogger()->error("BrunetonAtmosphereResource::upload: single_mie LUT size mismatch\n");
        return false;
    }

    m_transmittance = createLut2D(AtmTransmittanceTextureWidth,
                                  AtmTransmittanceTextureHeight,
                                  data.transmittance.data());
    m_scattering    = createLut3D(AtmScatteringTextureWidth,
                                  AtmScatteringTextureHeight,
                                  AtmScatteringTextureDepth,
                                  data.scattering.data());
    if (data.header.combined_scattering == 0)
    {
        m_singleMie = createLut3D(AtmScatteringTextureWidth,
                                  AtmScatteringTextureHeight,
                                  AtmScatteringTextureDepth,
                                  data.single_mie_scattering.data());
    }
    m_irradiance    = createLut2D(AtmIrradianceTextureWidth,
                                  AtmIrradianceTextureHeight,
                                  data.irradiance.data());

    const Std140AtmosphereBlock block = makeUboBlock(data);
    glGenBuffers(1, &m_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, m_ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(block), &block, GL_STATIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    return true;
}

} // namespace celestia::render
