// brunetonatmosphereresource.h
//
// Copyright (C) 2026, the Celestia Development Team
//
// Per-body GPU resources for the shared Bruneton sky shader:
//   - one std140 UBO matching AtmosphereBlock,
//   - the transmittance, scattering, optional single-Mie, and
//     irradiance LUTs as GL textures.
//
// One instance per atmospheric body. The shader program itself
// (StaticShader::Atmosphere) is shared across all instances.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <celengine/atmfile.h>
#include <celengine/brunetonatmosphere.h>
#include <celengine/glsupport.h>

namespace celestia::render
{

class BrunetonAtmosphereResource
{
public:
    BrunetonAtmosphereResource() = default;
    ~BrunetonAtmosphereResource();

    BrunetonAtmosphereResource(const BrunetonAtmosphereResource&) = delete;
    BrunetonAtmosphereResource& operator=(const BrunetonAtmosphereResource&) = delete;

    BrunetonAtmosphereResource(BrunetonAtmosphereResource&&) noexcept;
    BrunetonAtmosphereResource& operator=(BrunetonAtmosphereResource&&) noexcept;

    // Create the UBO + four textures and upload the contents of `data`.
    // Replaces any previously uploaded data (the old GL objects are
    // released). On failure, logs and leaves the object in the empty
    // state. Requires an active GL context.
    bool upload(const engine::AtmFileData& data);

    bool isReady() const noexcept { return m_ubo != 0; }

    GLuint ubo()                   const noexcept { return m_ubo; }
    GLuint transmittanceTexture()  const noexcept { return m_transmittance; }
    GLuint scatteringTexture()     const noexcept { return m_scattering; }
    GLuint singleMieTexture()      const noexcept { return m_singleMie; }
    GLuint irradianceTexture()     const noexcept { return m_irradiance; }

private:
    void releaseGL() noexcept;

    GLuint m_ubo{ 0 };
    GLuint m_transmittance{ 0 };
    GLuint m_scattering{ 0 };
    GLuint m_singleMie{ 0 };
    GLuint m_irradiance{ 0 };
};

} // namespace celestia::render
