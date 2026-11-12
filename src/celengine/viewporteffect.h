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
#include <vector>

#include <Eigen/Core>

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
    // Effects must reproject depth with color when annotations follow the chain.
    virtual bool prerender(Renderer*, FramebufferObject* fbo, FramebufferObject* dst,
                           bool preserveDepth = false);
    virtual bool render(Renderer*, FramebufferObject*, int width, int height,
                        bool preserveDepth = false) = 0;
    virtual bool distortXY(float& x, float& y);
    // Work backwards from the destination to determine each pass's source size.
    virtual Eigen::Vector2i inputSize(const Eigen::Vector2i& outputSize) const { return outputSize; }
    // Anchors use logical view pixels, independent of framebuffer resolution.
    // Overlapping warps can produce multiple anchors.
    virtual void mapToOutput(const Eigen::Vector2f&, int width, int height,
                             std::vector<Eigen::Vector2f>&) const;

    // Bitmask of FramebufferObject attachment flags this effect samples
    // from the source FBO. Anything else may be discarded before resolve.
    virtual FramebufferObject::Attachment sourceAttachments() const
    {
        return FramebufferObject::Attachment::Color;
    }

    // Whether this effect needs its source FBO to use a floating-point
    // color buffer (GL_RGBA16F) instead of the default GL_RGBA8.
    virtual bool needsFloatSource() const { return false; }
};

class PassthroughViewportEffect : public ViewportEffect
{
public:
    explicit PassthroughViewportEffect(bool needsFloatSource = false, bool convertToSRGB = true);
    ~PassthroughViewportEffect() override = default;

    bool needsFloatSource() const override { return m_needsFloatSource; }
    bool render(Renderer*, FramebufferObject*, int width, int height,
                bool preserveDepth = false) override;

protected:
    void drawQuad();

private:
    bool m_needsFloatSource;
    bool m_convertToSRGB;

    celestia::gl::VertexObject vo;

    void initialize();

    bool initialized{ false };
};

class FsrViewportEffect : public PassthroughViewportEffect
{
public:
    enum class Pass { Upscale, Sharpen };

    FsrViewportEffect(Pass pass, float parameter);
    Eigen::Vector2i inputSize(const Eigen::Vector2i& outputSize) const override;
    bool render(Renderer*, FramebufferObject*, int width, int height,
                bool preserveDepth = false) override;

private:
    Pass m_pass;
    float m_parameter;
};

class MetalFXViewportEffect : public PassthroughViewportEffect
{
public:
    explicit MetalFXViewportEffect(float scale);
    Eigen::Vector2i inputSize(const Eigen::Vector2i&) const override;
    bool render(Renderer*, FramebufferObject*, int width, int height,
                bool preserveDepth = false) override;

private:
    float m_scale;
};

class WarpMeshViewportEffect : public ViewportEffect //NOSONAR
{
public:
    explicit WarpMeshViewportEffect(std::unique_ptr<WarpMesh>&& mesh);
    ~WarpMeshViewportEffect() override;

    bool prerender(Renderer*, FramebufferObject* fbo, FramebufferObject* dst,
                   bool preserveDepth = false) override;
    bool render(Renderer*, FramebufferObject*, int width, int height,
                bool preserveDepth = false) override;
    bool distortXY(float& x, float& y) override;
    void mapToOutput(const Eigen::Vector2f&, int width, int height,
                     std::vector<Eigen::Vector2f>&) const override;

private:
    celestia::gl::VertexObject vo;

    std::unique_ptr<WarpMesh> mesh;

    void initialize();

    bool initialized{ false };
};
