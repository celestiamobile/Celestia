// clipboard.cpp
//
// Copyright (C) 2020-present, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "clipboard.h"

#include <string>

#ifdef USE_SDL3
#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>
#else
#include <SDL_clipboard.h>
#include <SDL_stdinc.h>
#endif

#include <celestia/celestiacore.h>
#include <celestia/celestiastate.h>
#include <celestia/hud.h>
#include <celestia/url.h>
#include <celutil/gettext.h>
#include "sdl_compat.h"

namespace celestia::sdl
{

void
doCopy(CelestiaCore& appCore)
{
    CelestiaState appState(&appCore);
    appState.captureState();

    if (SetClipboardText(Url(appState).getAsString().c_str()))
        appCore.flash(_("Copied URL"));
}

void
doPaste(CelestiaCore& appCore)
{
    if (!HasClipboardText())
        return;

    char* str = GetClipboardText();
    if (str == nullptr)
        return;

    if (appCore.getTextEnterMode() == Hud::TextEnterMode::Normal)
    {
        if (appCore.goToUrl(str))
            appCore.flash(_("Pasting URL"));
    }
    else
    {
        appCore.setTypedText(str);
    }

    SDL_free(str);
}

} // end namespace celestia::sdl
