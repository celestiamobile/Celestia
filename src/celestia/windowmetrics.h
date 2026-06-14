// windowmetrics.h
//
// Copyright (C) 2023, the Celestia Development Team
//
// Split out from celestiacore.h/celestiacore.cpp
// Copyright (C) 2001-2009, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

namespace celestia
{

enum class LayoutDirection
{
    LeftToRight   = 0,
    RightToLeft   = 1,
};

struct WindowMetrics
{
    int getSafeAreaWidth() const;
    int getSafeAreaHeight() const;
    // Offset is applied towards the safearea
    int getSafeAreaStart(int offset = 0) const;
    int getSafeAreaEnd(int offset = 0) const;
    int getSafeAreaTop(int offset = 0) const;
    int getSafeAreaBottom(int offset = 0) const;

    int width{ 1 };
    int height{ 1 };
    // Physical (rasterization) extents of the output framebuffer. When variable
    // rasterization rate (e.g. ANGLE foveated rendering) is enabled, this is
    // smaller than width/height; otherwise it matches them. Used to size
    // intermediate viewport-effect FBOs to the actual color attachment.
    int physicalWidth{ 0 };
    int physicalHeight{ 0 };
    int insetLeft{ 0 };
    int insetRight{ 0 };
    int insetTop{ 0 };
    int insetBottom{ 0 };
    int screenDpi{ 96 };
    float textScaleFactor{ 1.0f };
    LayoutDirection layoutDirection { LayoutDirection::LeftToRight };
};

}
