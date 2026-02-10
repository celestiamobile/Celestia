// helpers.h
//
// Copyright (C) 2020-present, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <cstdio>

#include <celengine/glsupport.h>

#include <fmt/format.h>
#include <imgui.h>

#ifdef USE_SDL3
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_stdinc.h>
#else
#include <SDL_messagebox.h>
#include <SDL_stdinc.h>
#endif

#include <celutil/flag.h>
#include <celutil/uniquedel.h>
#include "sdl_compat.h"

namespace celestia::sdl
{
namespace detail
{
inline void
fatalErrorImpl(fmt::string_view format, fmt::format_args args)
{
    auto message = fmt::vformat(format, args);
    if (!ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                              "Fatal Error",
                              message.c_str(),
                              nullptr))
        fmt::print(stderr, "{}\n", message);
}

} // end namespace celestia::sdl::detail

template <typename... Args>
void
fatalError(const char *format, Args&&... args)
{
    detail::fatalErrorImpl(fmt::string_view(format), fmt::make_format_args(args...));
}

template<typename T>
using UniqueSDL = util::UniquePtrDel<T, &SDL_free>;

template<typename T>
void
enumCheckbox(const char* label, T& value, T flag)
{
    bool set = util::is_set(value, flag);
    ImGui::Checkbox(label, &set);
    if (set)
        value |= flag;
    else
        value &= ~flag;
}

} // end namespace celestia::sdl
