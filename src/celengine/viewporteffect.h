//
// viewporteffect.h
//
// Copyright © 2020 Celestia Development Team. All rights reserved.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <celrender/gl/buffer.h>
#include <celrender/gl/vertexobject.h>
#include "framebuffer.h"
#include "shadermanager.h"

class Renderer;
class CelestiaGLProgram;
class WarpMesh;

class ViewportEffect
{
public:
    virtual ~ViewportEffect() = default;

    virtual bool preprocess(Renderer*, FramebufferObject*);
    virtual bool prerender(Renderer*, FramebufferObject* fbo, FramebufferObject* dst);
    virtual bool render(Renderer*, FramebufferObject*, int width, int height) = 0;
    virtual bool distortXY(float& x, float& y);

    // Bitmask of FramebufferObject attachment flags this effect samples
    // from the source FBO. Anything else may be discarded before resolve.
    virtual FramebufferObject::Attachment sourceAttachments() const
    {
        return FramebufferObject::Attachment::Color;
    }

    // Whether this effect needs its source FBO to use a floating-point
    // color buffer (GL_RGBA16F) instead of the default GL_RGBA8.
    virtual bool needsFloatSource() const { return false; }

    // Dimensions (in pixels) the effect expects its source FBO to have,
    // given the destination size that this effect must produce. The
    // default returns the destination size unchanged (source matches
    // destination, i.e. no rescaling). An effect may return a different
    // size to have the scene rendered into a rescaled FBO and then map
    // it back to the destination size — e.g. an upscaler returns a
    // smaller size for cheaper scene rendering, a supersampling effect
    // returns a larger one for quality. The view chains these
    // backwards from the screen, so the last effect's destination is
    // always the view's full size.
    virtual std::pair<int, int> sourceSize(int dstWidth, int dstHeight) const
    {
        return { dstWidth, dstHeight };
    }
};

class PassthroughViewportEffect : public ViewportEffect
{
public:
    explicit PassthroughViewportEffect(StaticShader shaderName = StaticShader::Passthrough,
                                       bool needsFloatSource = false);
    ~PassthroughViewportEffect() override = default;

    bool needsFloatSource() const override { return m_needsFloatSource; }
    bool render(Renderer*, FramebufferObject*, int width, int height) override;

private:
    StaticShader m_shaderName;
    bool m_needsFloatSource;

    celestia::gl::VertexObject vo;
    celestia::gl::Buffer bo;

    void initialize();

    bool initialized{ false };
};

// Renders the scene into a downscaled source FBO and then stretches it back
// to full view resolution using the same bilinear-sampled passthrough quad.
// Intended as the initial integration point for a real upscaler; swapping
// the shader for FSR/etc. requires no other changes.
class UpscaleViewportEffect : public PassthroughViewportEffect
{
public:
    explicit UpscaleViewportEffect(float scale = 0.5f,
                                   StaticShader shaderName = StaticShader::Passthrough,
                                   bool needsFloatSource = false);
    ~UpscaleViewportEffect() override = default;

    std::pair<int, int> sourceSize(int dstWidth, int dstHeight) const override;
    bool distortXY(float& x, float& y) override;

    float scale() const { return m_scale; }

private:
    float m_scale;
};

class WarpMeshViewportEffect : public ViewportEffect //NOSONAR
{
public:
    explicit WarpMeshViewportEffect(std::unique_ptr<WarpMesh>&& mesh);
    ~WarpMeshViewportEffect() override;

    bool prerender(Renderer*, FramebufferObject* fbo, FramebufferObject* dst) override;
    bool render(Renderer*, FramebufferObject*, int width, int height) override;
    bool distortXY(float& x, float& y) override;

private:
    celestia::gl::VertexObject vo;
    celestia::gl::Buffer bo;

    std::unique_ptr<WarpMesh> mesh;

    void initialize();

    bool initialized{ false };
};
