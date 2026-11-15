// Copyright (C) 2026 Celestia Development Team. All rights reserved.
// Licensed under the GNU General Public License, version 2 or later.

#pragma once

#include "framebuffer.h"

#include <celrender/gl/vertexobject.h>
#include <memory>
#include <vector>

class Renderer;

// Per-view workspace. Consumes the resolved HDR scene without modifying it.
class Bloom
{
public:
    bool   render(Renderer &, const FramebufferObject &);
    GLuint texture() const
    {
        return m_levels.front()->colorTexture();
    }

private:
    bool initialize(GLuint width, GLuint height);

    std::vector<std::unique_ptr<FramebufferObject>> m_levels;
    celestia::gl::VertexObject                      m_quad;
    bool                                            m_initialized{ false };
};
