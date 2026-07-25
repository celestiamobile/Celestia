// Copyright (C) 2026, the Celestia Development Team

#pragma once

#include <cstddef>

#include <celengine/brunetonatmospherefile.h>
#include <celengine/glsupport.h>
#include <celutil/classops.h>

namespace celestia::render
{

class BrunetonAtmosphereResource : private util::NoCopy
{
public:
    BrunetonAtmosphereResource() = default;
    ~BrunetonAtmosphereResource();

    BrunetonAtmosphereResource(BrunetonAtmosphereResource&&) noexcept;
    BrunetonAtmosphereResource& operator=(BrunetonAtmosphereResource&&) noexcept;

    bool upload(const engine::BrunetonAtmosphereData&);

    GLuint transmittanceTexture() const noexcept { return m_transmittance; }
    GLuint scatteringTexture() const noexcept { return m_scattering; }
    GLuint singleMieTexture() const noexcept { return m_singleMie; }
    GLuint irradianceTexture() const noexcept { return m_irradiance; }
    bool usesManualFloatFiltering() const noexcept { return m_manualFloatFiltering; }
    const engine::BrunetonAtmosphereParameters& parameters() const noexcept
    {
        return m_parameters;
    }

    std::size_t gpuBytes() const noexcept;

private:
    void release() noexcept;

    engine::BrunetonAtmosphereParameters m_parameters;
    GLuint m_transmittance{ 0 };
    GLuint m_scattering{ 0 };
    GLuint m_singleMie{ 0 };
    GLuint m_irradiance{ 0 };
    std::size_t m_gpuBytes{ 0 };
    bool m_manualFloatFiltering{ false };
};

} // namespace celestia::render
