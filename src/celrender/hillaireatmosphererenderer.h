// hillaireatmosphererenderer.h
//
// Copyright (C) 2001-present, Celestia Development Team
//
// Physically-based sky/atmosphere renderer implementing Sebastien Hillaire's
// 2020 technique ("A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique"). LUTs (transmittance, multiple scattering) are generated on the
// GPU via fragment-shader render-to-texture so the renderer works on the
// OpenGL 3.3 core / OpenGL ES 3.0 baseline (no compute shaders). The sky is
// composited onto an atmosphere shell so it is naturally occluded by other
// objects. Up to two directional light sources are supported.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <memory>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <celengine/glshader.h>
#include "gl/buffer.h"
#include "gl/vertexobject.h"

struct Atmosphere;
class Renderer;
struct RenderInfo;
class LightingState;
struct Matrices;
class FramebufferObject;

namespace celestia::math
{
class Frustum;
}

namespace celestia::render
{

class HillaireAtmosphereRenderer
{
public:
    explicit HillaireAtmosphereRenderer(Renderer &renderer);
    ~HillaireAtmosphereRenderer();
    HillaireAtmosphereRenderer(const HillaireAtmosphereRenderer&) = delete;
    HillaireAtmosphereRenderer(HillaireAtmosphereRenderer&&) = delete;
    HillaireAtmosphereRenderer& operator=(const HillaireAtmosphereRenderer&) = delete;
    HillaireAtmosphereRenderer& operator=(HillaireAtmosphereRenderer&&) = delete;

    void initGL();

    // Returns true if the atmosphere was rendered (valid programs + params).
    bool render(
        const RenderInfo         &ri,
        const Atmosphere         &atmosphere,
        const LightingState      &ls,
        const Eigen::Quaternionf &planetOrientation,
        const Eigen::Vector3f    &semiAxes,
        float                     radius,
        const math::Frustum      &frustum,
        const Matrices           &m,
        float                     fade);

private:
    // Physical, km-based atmosphere parameters derived from a Celestia
    // Atmosphere plus the planet radius. Used both for LUT generation and the
    // sky pass, and hashed to decide when the constant LUTs need regeneration.
    struct Params
    {
        float bottomRadius{ 0.0f };
        float topRadius{ 0.0f };
        Eigen::Vector3f rayleighScattering{ Eigen::Vector3f::Zero() };
        float rayleighDensityExpScale{ 0.0f };
        Eigen::Vector3f mieScattering{ Eigen::Vector3f::Zero() };
        Eigen::Vector3f mieExtinction{ Eigen::Vector3f::Zero() };
        float mieDensityExpScale{ 0.0f };
        float miePhaseG{ 0.0f };
        Eigen::Vector3f absorptionExtinction{ Eigen::Vector3f::Zero() };
        Eigen::Vector3f groundAlbedo{ Eigen::Vector3f::Zero() };
        float multiScatteringFactor{ 1.0f };

        bool approxEqual(const Params &o) const;
    };

    bool compilePrograms();
    void buildGeometry();
    void ensureLuts(const Params &params);
    void setAtmosphereUniforms(GLuint programId, const Params &params) const;
    Params makeParams(const Atmosphere &atmosphere, float radius) const;

    Renderer                  &m_renderer;

    GLProgram                  m_transmittanceProgram;
    GLProgram                  m_multiscatterProgram;
    GLProgram                  m_skyProgram;

    std::unique_ptr<FramebufferObject> m_transmittanceLut;
    std::unique_ptr<FramebufferObject> m_multiscatterLut;

    gl::Buffer                 m_quadBo;
    gl::VertexObject           m_quadVo;
    gl::Buffer                 m_shellBo;
    gl::VertexObject           m_shellVo;

    Params                     m_lutParams;
    bool                       m_lutValid{ false };
    bool                       m_initialized{ false };
    bool                       m_programsValid{ false };
};

} // namespace celestia::render
