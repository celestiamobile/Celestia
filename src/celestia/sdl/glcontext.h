// glcontext.h
//
// Copyright (C) 2025-present, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#ifdef USE_SDL3
#include <SDL3/SDL_video.h>
#else
#include <SDL_video.h>
#endif

namespace celestia::sdl
{

class UniqueGLContext //NOSONAR
{
public:
    explicit UniqueGLContext(SDL_GLContext context);
    ~UniqueGLContext();

    UniqueGLContext(const UniqueGLContext&) = delete;
    UniqueGLContext& operator=(const UniqueGLContext&) = delete;
    UniqueGLContext(UniqueGLContext&&) noexcept;
    UniqueGLContext& operator=(UniqueGLContext&&) noexcept;

    SDL_GLContext get() const { return m_context; }

private:
    SDL_GLContext m_context;
};

} // end namespace celestia::sdl
