// brunetonatmosphererenderer.h
//
// Copyright (C) 2026, the Celestia Development Team
//
// Renderer for atmospheres that have a baked Bruneton .atm LUT set
// attached (via BrunetonAtmosphereResource). Draws a fullscreen quad
// using the shared StaticShader::Atmosphere program, with per-body
// data bound from the resource's UBO + four LUT textures.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <memory>

#include <Eigen/Core>
#include <Eigen/Geometry>

namespace celestia::gl
{
class Buffer;
class VertexObject;
}

class Renderer;

namespace celestia::render
{

class BrunetonAtmosphereResource;

class BrunetonAtmosphereRenderer
{
public:
    explicit BrunetonAtmosphereRenderer(Renderer& renderer);
    ~BrunetonAtmosphereRenderer();

    BrunetonAtmosphereRenderer(const BrunetonAtmosphereRenderer&) = delete;
    BrunetonAtmosphereRenderer(BrunetonAtmosphereRenderer&&) = delete;
    BrunetonAtmosphereRenderer& operator=(const BrunetonAtmosphereRenderer&) = delete;
    BrunetonAtmosphereRenderer& operator=(BrunetonAtmosphereRenderer&&) = delete;

    // Per-frame, per-body parameters that the shader expects. All
    // positions/directions are in object-space (the body's local frame)
    // and length units are kilometres (Bruneton's convention).
    struct FrameParams
    {
        Eigen::Matrix4f inv_projection;     // clip-space -> view-space (km)
        Eigen::Matrix4f inv_modelview_km;   // view-space -> object-space (km)
        Eigen::Vector3f camera_km;          // observer in object-space (km)
        Eigen::Vector3f sun_direction;      // unit vector, object-space
        Eigen::Vector3f white_point;        // from .atm header
        float           exposure;
    };

    // Bind the shared atmosphere program + this body's UBO and LUTs,
    // and draw the fullscreen quad. The caller is responsible for
    // having set up the depth/blend state appropriate to where this
    // pass sits in the frame.
    void render(const BrunetonAtmosphereResource& resource,
                const FrameParams&                params);

private:
    void initGL();

    Renderer&                         m_renderer;
    std::unique_ptr<gl::Buffer>       m_quadBuffer;
    std::unique_ptr<gl::VertexObject> m_quadVao;
    bool                              m_initialized{ false };
};

} // namespace celestia::render
