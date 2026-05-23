// fontshaper.h
//
// Copyright (C) 2026-present, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace celestia::text
{

// Opaque per-platform font identity. Backed by:
//   Apple   - CTFontRef
//   Windows - IDWriteFontFace*
//   Other   - FT_Face* + size state
//
// Stable pointer identity is used as part of the glyph atlas key, so the
// engine must return the same FontHandle instance for the same underlying
// platform font across shape() calls (i.e. cache them internally).
class FontHandle
{
public:
    virtual ~FontHandle() = default;
};

struct ShapedGlyph
{
    std::shared_ptr<FontHandle> font;
    std::uint32_t               glyphId;
    float                       xAdvance;
    float                       xOffset;
    float                       yOffset;
};

struct GlyphBitmap
{
    int                       width;
    int                       height;
    int                       bearingX;     // FreeType bitmap_left equivalent
    int                       bearingY;     // FreeType bitmap_top equivalent
    std::vector<std::uint8_t> alpha;        // width * height, single-channel
};

struct FontMetrics
{
    int maxAscent;
    int maxDescent;
};

// Platform font engine. Owns the per-platform shaping + rasterization state
// and the font cache that backs FontHandle identity.
//
// One engine instance per TextureFont. Recreated when DPI, text-scale, or
// UI locale changes (current truetypefont.cpp already rebuilds on the first
// two; locale will join them).
class PlatformFontEngine
{
public:
    virtual ~PlatformFontEngine() = default;

    // Shape text. emSizePx is the target em size in pixels (caller has
    // already applied DPI and text-scale).
    //
    // locale is a BCP 47 tag (e.g. "ja", "zh-Hans"). The empty string falls
    // back to the system locale — avoid in production calls.
    virtual std::vector<ShapedGlyph> shape(std::u16string_view text,
                                           std::string_view   locale,
                                           float              emSizePx) = 0;

    // Rasterize a single glyph from a font handle returned by shape().
    // Returns nullopt only for invalid glyph IDs; the atlas treats this as
    // a zero-size glyph and skips it.
    virtual std::optional<GlyphBitmap> rasterize(const FontHandle& font,
                                                 std::uint32_t     glyphId,
                                                 float             emSizePx) = 0;

    // Metrics for the primary font at this size, used for line height.
    virtual FontMetrics metrics(float emSizePx) = 0;
};

// Construct the engine for the current platform. primaryFont selects the
// base font from which the platform's fallback cascade extends (CoreText
// CTFontCreateForString base font, DirectWrite text-format default, or the
// primary FT_Face on FreeType-only platforms).
//
// When bold is true the backend resolves a bold variant: CoreText uses
// kCTFontUIFontEmphasizedSystem (empty path) or
// CTFontDescriptorCreateCopyWithSymbolicTraits with kCTFontBoldTrait
// (file path); DirectWrite will pass DWRITE_FONT_WEIGHT_BOLD; the embedded
// FreeType backend ignores the flag (caller supplies a bold file directly,
// e.g. "DejaVuSans-Bold.ttf").
std::unique_ptr<PlatformFontEngine>
createPlatformFontEngine(const std::filesystem::path& primaryFont,
                         int                          faceIndex,
                         bool                         bold = false);

} // namespace celestia::text
