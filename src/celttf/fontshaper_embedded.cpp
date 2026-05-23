// fontshaper_embedded.cpp
//
// Copyright (C) 2026-present, the Celestia Development Team
//
// FreeType fallback backend. Used on Linux, Android <29, and as the
// last-resort path when no platform-native shaper is built in. Does not
// perform itemization, bidi, or font fallback: every character is looked up
// in the single primary font, with U+003F substituted on miss. This matches
// the pre-refactor behavior of truetypefont.cpp.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "fontshaper.h"

#include <cstring>
#include <utility>

#include <celutil/logger.h>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace celestia::text
{

namespace
{

using celestia::util::GetLogger;

class FreeTypeFontHandle final : public FontHandle
{
public:
    explicit FreeTypeFontHandle(FT_Face face) : m_face(face) {}
    ~FreeTypeFontHandle() override
    {
        if (m_face != nullptr)
            FT_Done_Face(m_face);
    }
    FreeTypeFontHandle(const FreeTypeFontHandle&) = delete;
    FreeTypeFontHandle& operator=(const FreeTypeFontHandle&) = delete;

    FT_Face face() const { return m_face; }

private:
    FT_Face m_face;
};

class EmbeddedFontEngine final : public PlatformFontEngine
{
public:
    EmbeddedFontEngine(std::shared_ptr<FreeTypeFontHandle> primary)
        : m_primary(std::move(primary))
    {}

    std::vector<ShapedGlyph> shape(std::u16string_view text,
                                   std::string_view   /*locale*/,
                                   float              emSizePx) override;

    std::optional<GlyphBitmap> rasterize(const FontHandle& font,
                                         std::uint32_t     glyphId,
                                         float             emSizePx) override;

    FontMetrics metrics(float emSizePx) override;

private:
    void ensureSize(float emSizePx);

    std::shared_ptr<FreeTypeFontHandle> m_primary;
    float                               m_currentSize{ -1.0f };
};

void
EmbeddedFontEngine::ensureSize(float emSizePx)
{
    if (m_currentSize == emSizePx)
        return;
    FT_Set_Pixel_Sizes(m_primary->face(), 0, static_cast<FT_UInt>(emSizePx + 0.5f));
    m_currentSize = emSizePx;
}

std::vector<ShapedGlyph>
EmbeddedFontEngine::shape(std::u16string_view text,
                          std::string_view    /*locale*/,
                          float               emSizePx)
{
    ensureSize(emSizePx);
    FT_Face face = m_primary->face();

    std::vector<ShapedGlyph> out;
    out.reserve(text.size());

    std::u16string_view::size_type i = 0;
    while (i < text.size())
    {
        std::uint32_t ch;
        if (text[i] < 0xd800 || text[i] >= 0xe000)
        {
            ch = text[i];
            ++i;
        }
        else if (text[i] < 0xdc00
                 && (i + 1) < text.size()
                 && text[i + 1] >= 0xdc00
                 && text[i + 1] < 0xe000)
        {
            // U+10000 + (high - 0xD800) * 0x400 + (low - 0xDC00)
            ch = 0x10000u
                 + ((static_cast<std::uint32_t>(text[i])     - 0xd800u) << 10)
                 +  (static_cast<std::uint32_t>(text[i + 1]) - 0xdc00u);
            i += 2;
        }
        else
        {
            // Lone surrogate, skip.
            ++i;
            continue;
        }

        FT_UInt glyphId = FT_Get_Char_Index(face, ch);
        if (glyphId == 0)
            glyphId = FT_Get_Char_Index(face, u'?');

        if (FT_Load_Glyph(face, glyphId, FT_LOAD_DEFAULT) != 0)
            continue;

        ShapedGlyph g;
        g.font     = m_primary;
        g.glyphId  = glyphId;
        g.xAdvance = static_cast<float>(face->glyph->advance.x >> 6);
        g.xOffset  = 0.0f;
        g.yOffset  = 0.0f;
        out.push_back(std::move(g));
    }

    return out;
}

std::optional<GlyphBitmap>
EmbeddedFontEngine::rasterize(const FontHandle& font,
                              std::uint32_t     glyphId,
                              float             emSizePx)
{
    // The engine only ever produces FreeTypeFontHandle instances, so the
    // downcast is unconditional. Same-engine pairing of shape/rasterize is a
    // contract documented on PlatformFontEngine.
    const auto& ftHandle = static_cast<const FreeTypeFontHandle&>(font);
    FT_Face     face     = ftHandle.face();

    ensureSize(emSizePx);

    if (FT_Load_Glyph(face, glyphId, FT_LOAD_RENDER) != 0)
        return std::nullopt;

    FT_GlyphSlot slot = face->glyph;

    GlyphBitmap bm;
    bm.width    = static_cast<int>(slot->bitmap.width);
    bm.height   = static_cast<int>(slot->bitmap.rows);
    bm.bearingX = slot->bitmap_left;
    bm.bearingY = slot->bitmap_top;

    if (bm.width > 0 && bm.height > 0 && slot->bitmap.buffer != nullptr)
    {
        bm.alpha.resize(static_cast<std::size_t>(bm.width) * static_cast<std::size_t>(bm.height));
        // FreeType row pitch can exceed width; copy row-by-row.
        for (int y = 0; y < bm.height; ++y)
        {
            std::memcpy(bm.alpha.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(bm.width),
                        slot->bitmap.buffer + static_cast<std::ptrdiff_t>(y) * slot->bitmap.pitch,
                        static_cast<std::size_t>(bm.width));
        }
    }

    return bm;
}

FontMetrics
EmbeddedFontEngine::metrics(float emSizePx)
{
    ensureSize(emSizePx);
    FT_Face face = m_primary->face();
    return {
        static_cast<int>(face->size->metrics.ascender  >> 6),
        static_cast<int>(-face->size->metrics.descender >> 6),
    };
}

FT_Library
ftLibrary()
{
    static FT_Library ftlib = nullptr;
    if (ftlib == nullptr && FT_Init_FreeType(&ftlib) != 0)
    {
        GetLogger()->error("Could not init freetype library\n");
        return nullptr;
    }
    return ftlib;
}

} // anonymous namespace

std::unique_ptr<PlatformFontEngine>
createPlatformFontEngine(const std::filesystem::path& primaryFont, int faceIndex)
{
    // Embedded backend has no concept of "system default font"; the caller
    // is expected to chain to a bundled font (DejaVuSans) when we return
    // nullptr for an empty path.
    if (primaryFont.empty())
        return nullptr;

    FT_Library ftlib = ftLibrary();
    if (ftlib == nullptr)
        return nullptr;

    FT_Face face = nullptr;
    if (FT_New_Face(ftlib, primaryFont.string().c_str(), faceIndex, &face) != 0)
    {
        GetLogger()->error("Could not open font {}\n", primaryFont);
        return nullptr;
    }

    if (!FT_IS_SCALABLE(face))
    {
        GetLogger()->error("Font is not scalable: {}\n", primaryFont);
        FT_Done_Face(face);
        return nullptr;
    }

    auto handle = std::make_shared<FreeTypeFontHandle>(face);
    return std::make_unique<EmbeddedFontEngine>(std::move(handle));
}

} // namespace celestia::text
