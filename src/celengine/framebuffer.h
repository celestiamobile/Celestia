// framebuffer.h
//
// Copyright (C) 2010-2020, the Celestia Development Team
// Original version by Chris Laurel <claurel@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include "glsupport.h"

class FramebufferObject
{
 public:
    enum
    {
        ColorAttachment = 0x1,
        DepthAttachment = 0x2
    };
    FramebufferObject() = delete;
    FramebufferObject(GLuint width, GLuint height, unsigned int attachments, int samples = 1, bool useFloatColor = false, bool foveated = false);
    FramebufferObject(const FramebufferObject&) = delete;
    FramebufferObject(FramebufferObject&&) noexcept;
    FramebufferObject& operator=(const FramebufferObject&) = delete;
    FramebufferObject& operator=(FramebufferObject&&) noexcept;
    ~FramebufferObject();

    // Create a non-owning wrapper around the currently bound GL framebuffer.
    // Only bind() is valid on the resulting object; other methods will assert.
    static FramebufferObject wrapCurrentBinding();

    // Returns true iff the GL_QCOM_texture_foveated extension is exposed by the
    // current GL context. Safe to call at any time after gl::init().
    static bool isFoveationSupported();

    // Mark an externally-owned GL_TEXTURE_2D as foveated (Adreno
    // GL_QCOM_texture_foveated). Must be called on a texture before it is used
    // as a color attachment. minPixelDensity is a [0,1] hint that caps how
    // aggressively the driver may reduce peripheral density (0 = no floor,
    // driver chooses). No-op if the extension is unsupported.
    static void enableTextureFoveation(GLuint texture, float minPixelDensity = 0.0f);

    // Set foveation parameters on an externally-owned texture. The texture
    // must have been marked foveated via enableTextureFoveation() (or be the
    // color attachment of a foveated FramebufferObject). focalX/focalY are in
    // NDC space ([-1, 1], 0 = image center). No-op if the extension is
    // unsupported.
    static void setTextureFoveationParameters(GLuint texture,
                                              GLuint layer,
                                              GLuint focalPoint,
                                              float focalX,
                                              float focalY,
                                              float gainX,
                                              float gainY,
                                              float foveaArea);

    bool isValid() const;
    GLuint width() const
    {
        return m_width;
    }

    GLuint height() const
    {
        return m_height;
    }

    int samples() const
    {
        return m_samples;
    }

    bool useFloatColor() const
    {
        return m_useFloatColor;
    }

    bool foveated() const
    {
        return m_foveated;
    }

    // Update foveation parameters for this FBO's color texture. layer is
    // typically 0 for a non-array texture. Has no effect if the FBO was not
    // created with foveated = true.
    void setFoveationParameters(GLuint layer,
                                GLuint focalPoint,
                                float focalX,
                                float focalY,
                                float gainX,
                                float gainY,
                                float foveaArea) const;

    GLuint colorTexture() const;
    GLuint depthTexture() const;

    bool bind();
    bool unbind(GLint oldfboId);
    bool resolve() const;

 private:
    explicit FramebufferObject(GLuint fboId); // non-owning wrapper; only bind() is valid

    void generateColorTexture();
    void generateDepthTexture();
    void generateFbo(unsigned int attachments);
    void generateMSAAFbo(unsigned int attachments);
    void cleanup();

 private:
    GLuint m_width;
    GLuint m_height;
    GLuint m_colorTexId;
    GLuint m_depthTexId;
    GLuint m_fboId;
    GLuint m_msaaFboId{ 0 };
    GLuint m_colorRboId{ 0 };
    GLuint m_depthRboId{ 0 };
    int    m_samples;
    bool   m_useFloatColor;
    bool   m_foveated{ false };
    GLenum m_status;
    bool   m_owned;
};
