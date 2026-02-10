// sdl_compat.h
//
// SDL2/SDL3 compatibility layer for Celestia
//
// Copyright (C) 2026, Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

// Conditional SDL includes
#ifdef USE_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#else
#include <SDL.h>
#include <SDL_opengl.h>
#endif

namespace celestia::sdl
{

// ============================================================================
// SDL_version structure (SDL3 doesn't declare it, so we define it)
// ============================================================================

#ifdef USE_SDL3
struct SDL_version
{
    Uint8 major;
    Uint8 minor;
    Uint8 patch;
};
#endif

// ============================================================================
// Type Aliases
// ============================================================================

#ifdef USE_SDL3
using SDLWindowFlags = Uint64;
using SDL_Keymod = Uint16;
#else
using SDLWindowFlags = Uint32;
#endif

// ============================================================================
// Keyboard Modifier Constants (SDL3 renamed these)
// ============================================================================

#ifdef USE_SDL3
constexpr SDL_Keymod KMOD_CTRL = SDL_KMOD_CTRL;
constexpr SDL_Keymod KMOD_SHIFT = SDL_KMOD_SHIFT;
constexpr SDL_Keymod KMOD_ALT = SDL_KMOD_ALT;
#endif

// ============================================================================
// Keyboard Event Field Access (SDL3 changed structure)
// ============================================================================

// In SDL2: event.keysym.sym, event.keysym.mod
// In SDL3: event.key, event.mod
#ifdef USE_SDL3
#define SDL_KEYSYM_SYM(event) ((event).key)
#define SDL_KEYSYM_MOD(event) ((event).mod)
#else
#define SDL_KEYSYM_SYM(event) ((event).keysym.sym)
#define SDL_KEYSYM_MOD(event) ((event).keysym.mod)
#endif

// ============================================================================
// Window Flag Constants
// ============================================================================

#ifdef USE_SDL3
constexpr SDLWindowFlags WINDOW_OPENGL = SDL_WINDOW_OPENGL;
constexpr SDLWindowFlags WINDOW_RESIZABLE = SDL_WINDOW_RESIZABLE;
constexpr SDLWindowFlags WINDOW_HIGH_PIXEL_DENSITY = SDL_WINDOW_HIGH_PIXEL_DENSITY;
constexpr SDLWindowFlags WINDOW_HIDDEN = SDL_WINDOW_HIDDEN;
constexpr SDLWindowFlags WINDOW_FULLSCREEN = SDL_WINDOW_FULLSCREEN;
constexpr SDLWindowFlags WINDOW_MAXIMIZED = SDL_WINDOW_MAXIMIZED;
#else
constexpr SDLWindowFlags WINDOW_OPENGL = SDL_WINDOW_OPENGL;
constexpr SDLWindowFlags WINDOW_RESIZABLE = SDL_WINDOW_RESIZABLE;
constexpr SDLWindowFlags WINDOW_HIGH_PIXEL_DENSITY = SDL_WINDOW_ALLOW_HIGHDPI;
constexpr SDLWindowFlags WINDOW_HIDDEN = SDL_WINDOW_HIDDEN;
constexpr SDLWindowFlags WINDOW_FULLSCREEN = SDL_WINDOW_FULLSCREEN_DESKTOP;
constexpr SDLWindowFlags WINDOW_MAXIMIZED = SDL_WINDOW_MAXIMIZED;
#endif

// ============================================================================
// Event Type Constants
// ============================================================================

#ifdef USE_SDL3
constexpr Uint32 EVENT_QUIT = SDL_EVENT_QUIT;
constexpr Uint32 EVENT_WINDOW_RESIZED = SDL_EVENT_WINDOW_RESIZED;
constexpr Uint32 EVENT_WINDOW_PIXEL_SIZE_CHANGED = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
constexpr Uint32 EVENT_WINDOW_CLOSE_REQUESTED = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
constexpr Uint32 EVENT_WINDOW_DISPLAY_SCALE_CHANGED = SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED;
constexpr Uint32 EVENT_KEYDOWN = SDL_EVENT_KEY_DOWN;
constexpr Uint32 EVENT_KEYUP = SDL_EVENT_KEY_UP;
constexpr Uint32 EVENT_TEXT_INPUT = SDL_EVENT_TEXT_INPUT;
constexpr Uint32 EVENT_MOUSEMOTION = SDL_EVENT_MOUSE_MOTION;
constexpr Uint32 EVENT_MOUSEBUTTONDOWN = SDL_EVENT_MOUSE_BUTTON_DOWN;
constexpr Uint32 EVENT_MOUSEBUTTONUP = SDL_EVENT_MOUSE_BUTTON_UP;
constexpr Uint32 EVENT_MOUSEWHEEL = SDL_EVENT_MOUSE_WHEEL;
#else
constexpr Uint32 EVENT_QUIT = SDL_QUIT;
constexpr Uint32 EVENT_WINDOW_RESIZED = SDL_WINDOWEVENT;  // Will need subtype check
constexpr Uint32 EVENT_WINDOW_PIXEL_SIZE_CHANGED = SDL_WINDOWEVENT;  // Not directly available in SDL2
constexpr Uint32 EVENT_WINDOW_CLOSE_REQUESTED = SDL_WINDOWEVENT;  // Will need subtype check
constexpr Uint32 EVENT_WINDOW_DISPLAY_SCALE_CHANGED = SDL_WINDOWEVENT;  // Will need subtype check (SDL_WINDOWEVENT_DISPLAY_CHANGED)
constexpr Uint32 EVENT_KEYDOWN = SDL_KEYDOWN;
constexpr Uint32 EVENT_KEYUP = SDL_KEYUP;
constexpr Uint32 EVENT_TEXT_INPUT = SDL_TEXTINPUT;
constexpr Uint32 EVENT_MOUSEMOTION = SDL_MOUSEMOTION;
constexpr Uint32 EVENT_MOUSEBUTTONDOWN = SDL_MOUSEBUTTONDOWN;
constexpr Uint32 EVENT_MOUSEBUTTONUP = SDL_MOUSEBUTTONUP;
constexpr Uint32 EVENT_MOUSEWHEEL = SDL_MOUSEWHEEL;
#endif

// ============================================================================
// Initialization & Shutdown
// ============================================================================

inline bool InitSDL(Uint32 flags)
{
#ifdef USE_SDL3
    return SDL_Init(flags);
#else
    return SDL_Init(flags) == 0;
#endif
}

// ============================================================================
// Window Management
// ============================================================================

inline SDL_Window* CreateWindow(const char* title, int x, int y, int w, int h, SDLWindowFlags flags)
{
#ifdef USE_SDL3
    SDL_Window* window = SDL_CreateWindow(title, w, h, flags);
    if (window != nullptr && x != SDL_WINDOWPOS_UNDEFINED && y != SDL_WINDOWPOS_UNDEFINED)
    {
        SDL_SetWindowPosition(window, x, y);
    }
    return window;
#else
    return SDL_CreateWindow(title, x, y, w, h, flags);
#endif
}

inline bool SetWindowFullscreen(SDL_Window* window, bool fullscreen)
{
#ifdef USE_SDL3
    return SDL_SetWindowFullscreen(window, fullscreen);
#else
    return SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0;
#endif
}

// ============================================================================
// OpenGL Context
// ============================================================================

inline bool SDL_CheckSuccess(int result)
{
#ifdef USE_SDL3
    return result;
#else
    return result == 0;
#endif
}

#ifdef USE_SDL3
inline bool SDL_CheckSuccess(bool result)
{
    return result;
}
#endif

// ============================================================================
// Display & DPI
// ============================================================================

inline bool GetDisplayDPI(SDL_Window* window, int displayIndex, float* dpi)
{
#ifdef USE_SDL3
    (void)displayIndex;  // Unused in SDL3
    float scale = SDL_GetWindowDisplayScale(window);
    if (scale > 0.0f)
    {
        *dpi = scale * 96.0f;  // Assume 96 DPI base
        return true;
    }
    return false;
#else
    (void)window;  // Unused in SDL2
    float ddpi, hdpi, vdpi;
    if (SDL_GetDisplayDPI(displayIndex, &ddpi, &hdpi, &vdpi) == 0)
    {
        *dpi = ddpi;
        return true;
    }
    return false;
#endif
}

// ============================================================================
// Cursor Control
// ============================================================================

inline void ShowCursor(bool show)
{
#ifdef USE_SDL3
    if (show)
        SDL_ShowCursor();
    else
        SDL_HideCursor();
#else
    SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
#endif
}

inline bool IsCursorVisible()
{
#ifdef USE_SDL3
    return SDL_CursorVisible();
#else
    return SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE;
#endif
}

// ============================================================================
// Text Input
// ============================================================================

inline void StartTextInput(SDL_Window* window)
{
#ifdef USE_SDL3
    SDL_StartTextInput(window);
#else
    (void)window;  // SDL2 doesn't use window parameter
    SDL_StartTextInput();
#endif
}

inline void StopTextInput(SDL_Window* window)
{
#ifdef USE_SDL3
    SDL_StopTextInput(window);
#else
    (void)window;  // SDL2 doesn't use window parameter
    SDL_StopTextInput();
#endif
}

// ============================================================================
// Clipboard
// ============================================================================

inline bool SetClipboardText(const char* text)
{
#ifdef USE_SDL3
    return SDL_SetClipboardText(text);
#else
    return SDL_SetClipboardText(text) == 0;
#endif
}

inline bool HasClipboardText()
{
#ifdef USE_SDL3
    return SDL_HasClipboardText();
#else
    return SDL_HasClipboardText() == SDL_TRUE;
#endif
}

inline char* GetClipboardText()
{
    return SDL_GetClipboardText();
}

// ============================================================================
// Event Handling
// ============================================================================

inline bool PollEvent(SDL_Event* event)
{
#ifdef USE_SDL3
    return SDL_PollEvent(event);
#else
    return SDL_PollEvent(event) == 1;
#endif
}

// ============================================================================
// Version Info
// ============================================================================

inline void GetSDLVersion(SDL_version* version)
{
#ifdef USE_SDL3
    version->major = SDL_MAJOR_VERSION;
    version->minor = SDL_MINOR_VERSION;
    version->patch = SDL_MICRO_VERSION;
#else
    SDL_GetVersion(version);
#endif
}

// ============================================================================
// Window Size Functions
// ============================================================================

#ifdef USE_SDL3
// SDL3 removed SDL_GL_GetDrawableSize, use SDL_GetWindowSizeInPixels instead
inline void SDL_GL_GetDrawableSize(SDL_Window* window, int* w, int* h)
{
    SDL_GetWindowSizeInPixels(window, w, h);
}
#endif

// ============================================================================
// Message Boxes
// ============================================================================

inline bool ShowSimpleMessageBox(Uint32 flags, const char* title, const char* message, SDL_Window* window)
{
#ifdef USE_SDL3
    return SDL_ShowSimpleMessageBox(flags, title, message, window);
#else
    return SDL_ShowSimpleMessageBox(flags, title, message, window) == 0;
#endif
}

} // namespace celestia::sdl
