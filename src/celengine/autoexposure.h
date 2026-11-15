// Copyright (C) 2026 Celestia Development Team. All rights reserved.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include "framebuffer.h"

#include <celrender/gl/vertexobject.h>
#include <memory>

class Renderer;

// Owned by a view. Completed GPU measurements drive the next scene's CPU and GPU exposure.
class AutoExposure
{
public:
    static constexpr float MaximumExposure = 32.0f;
    static constexpr float MiddleGray      = 0.18f;

    explicit AutoExposure(float initialExposure);
    ~AutoExposure();
    AutoExposure(const AutoExposure &)            = delete;
    AutoExposure &operator=(const AutoExposure &) = delete;

    bool  advance(double now);
    float exposure() const;
    float meterExposure() const;
    bool  needsMetering(double now) const;
    // The physical-light attachment must be resolved. Caller restores destination, viewport and
    // scissor.
    bool meter(Renderer &, const FramebufferObject &scene, double now);

private:
    bool initialize();
    bool readMeasurement();

    std::unique_ptr<FramebufferObject> m_samples;
    std::unique_ptr<FramebufferObject> m_meter;
    std::unique_ptr<FramebufferObject> m_target;
    celestia::gl::VertexObject         m_quad;
    GLuint                             m_readback{ 0 };
    GLsync                             m_fence{ nullptr };
    double                             m_ev;
    double                             m_targetEV;
    float                              m_meterExposure;
    double                             m_lastTime{ -1.0 };
    double                             m_lastMeterTime{ -1.0 };
    bool                               m_hasMeasurement{ false };
    bool                               m_readbackQueued{ false };
};
