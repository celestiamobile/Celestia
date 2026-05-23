// fontshaper_android.cpp
//
// Copyright (C) 2026-present, the Celestia Development Team
//
// Android font engine. On API 29+ with no explicit font specified,
// AFontMatcher resolves per-run fonts from the system font catalog and
// FreeType rasterizes the matched file. On older API levels (or when an
// explicit font file is supplied) the backend uses a single FT_Face with
// character-by-character glyph lookup, matching the embedded backend.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

// Tell the NDK to expand __INTRODUCED_IN(N) as a weak reference instead of
// emitting a hard "unavailable" error when the build target API is lower
// than N. The AFontMatcherEngine class is annotated with the same
// availability so clang allows the API 29+ calls inside its methods, and
// the factory's __builtin_available check gates construction at runtime.
#define __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__

#include "fontshaper.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

#include <celutil/logger.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include <android/font.h>
#include <android/font_matcher.h>

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

std::shared_ptr<FreeTypeFontHandle>
openFace(const std::filesystem::path& path, int faceIndex)
{
    FT_Library ftlib = ftLibrary();
    if (ftlib == nullptr)
        return nullptr;

    FT_Face face = nullptr;
    if (FT_New_Face(ftlib, path.string().c_str(), faceIndex, &face) != 0)
    {
        GetLogger()->error("Could not open font {}\n", path);
        return nullptr;
    }
    if (!FT_IS_SCALABLE(face))
    {
        FT_Done_Face(face);
        return nullptr;
    }
    return std::make_shared<FreeTypeFontHandle>(face);
}

void
ensureSize(FT_Face face, float emSizePx, float& currentSize)
{
    if (currentSize == emSizePx)
        return;
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(emSizePx + 0.5f));
    currentSize = emSizePx;
}

// Common rasterization path shared by both engines. FT_Face must already
// be sized to emSizePx before calling.
std::optional<GlyphBitmap>
rasterizeWithFreeType(FT_Face face, std::uint32_t glyphId)
{
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
        for (int y = 0; y < bm.height; ++y)
        {
            std::memcpy(bm.alpha.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(bm.width),
                        slot->bitmap.buffer + static_cast<std::ptrdiff_t>(y) * slot->bitmap.pitch,
                        static_cast<std::size_t>(bm.width));
        }
    }
    return bm;
}

// Decode one Unicode codepoint from a UTF-16 view at position i. Returns
// the codepoint and advances i by 1 or 2. Returns 0 for lone surrogates,
// caller should skip those.
std::uint32_t
decodeUtf16(std::u16string_view text, std::size_t& i)
{
    char16_t u = text[i];
    if (u < 0xd800 || u >= 0xe000)
    {
        ++i;
        return u;
    }
    if (u < 0xdc00 && (i + 1) < text.size()
        && text[i + 1] >= 0xdc00 && text[i + 1] < 0xe000)
    {
        std::uint32_t cp = 0x10000u
                           + ((static_cast<std::uint32_t>(u) - 0xd800u) << 10)
                           +  (static_cast<std::uint32_t>(text[i + 1]) - 0xdc00u);
        i += 2;
        return cp;
    }
    ++i;
    return 0;
}

// ---------------------------------------------------------------------------
// FallbackEngine — single FT_Face, char-by-char (mirrors fontshaper_embedded)
// ---------------------------------------------------------------------------

class FallbackEngine final : public PlatformFontEngine
{
public:
    explicit FallbackEngine(std::shared_ptr<FreeTypeFontHandle> primary)
        : m_primary(std::move(primary))
    {}

    std::vector<ShapedGlyph> shape(std::u16string_view text,
                                   std::string_view   /*locale*/,
                                   float              emSizePx) override
    {
        ensureSize(m_primary->face(), emSizePx, m_currentSize);
        FT_Face face = m_primary->face();

        std::vector<ShapedGlyph> out;
        out.reserve(text.size());

        std::size_t i = 0;
        while (i < text.size())
        {
            std::uint32_t ch = decodeUtf16(text, i);
            if (ch == 0)
                continue;

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

    std::optional<GlyphBitmap> rasterize(const FontHandle& font,
                                         std::uint32_t     glyphId,
                                         float             emSizePx) override
    {
        const auto& ftHandle = static_cast<const FreeTypeFontHandle&>(font);
        FT_Face     face     = ftHandle.face();
        ensureSize(face, emSizePx, m_currentSize);
        return rasterizeWithFreeType(face, glyphId);
    }

    FontMetrics metrics(float emSizePx) override
    {
        FT_Face face = m_primary->face();
        ensureSize(face, emSizePx, m_currentSize);
        return {
            static_cast<int>(face->size->metrics.ascender  >> 6),
            static_cast<int>(-face->size->metrics.descender >> 6),
        };
    }

private:
    std::shared_ptr<FreeTypeFontHandle> m_primary;
    float                               m_currentSize{ -1.0f };
};

// ---------------------------------------------------------------------------
// AFontMatcherEngine — API 29+ system font cascade
// ---------------------------------------------------------------------------

// The entire class is API 29+. The factory only constructs an instance
// inside an __builtin_available(android 29, *) branch, which satisfies the
// availability check at the call site; the annotation lets clang allow
// AFontMatcher_/AFont_ calls inside the class's methods without per-call
// guards.
class __attribute__((availability(android, introduced=29)))
    AFontMatcherEngine final : public PlatformFontEngine
{
public:
    explicit AFontMatcherEngine(bool bold)
        : m_weight(bold ? static_cast<uint16_t>(700)
                        : static_cast<uint16_t>(400))
    {
        m_matcher = AFontMatcher_create();
        if (m_matcher != nullptr)
        {
            // AFONT_WEIGHT_NORMAL = 400, AFONT_WEIGHT_BOLD = 700. The
            // NDK uses uint16_t weights on the same CSS-like scale as
            // DirectWrite.
            AFontMatcher_setStyle(m_matcher, m_weight, false /* italic */);
        }
    }

    ~AFontMatcherEngine() override
    {
        if (m_matcher != nullptr)
            AFontMatcher_destroy(m_matcher);
    }

    std::vector<ShapedGlyph> shape(std::u16string_view text,
                                   std::string_view   locale,
                                   float              emSizePx) override;

    std::optional<GlyphBitmap> rasterize(const FontHandle& font,
                                         std::uint32_t     glyphId,
                                         float             emSizePx) override
    {
        const auto& ftHandle = static_cast<const FreeTypeFontHandle&>(font);
        FT_Face     face     = ftHandle.face();
        // Per-face size state — AFontMatcher returns different files for
        // different runs, so each handle tracks its own current size in
        // a sidecar map.
        float& cur = m_faceSize[face];
        ensureSize(face, emSizePx, cur);

        // Synthetic bold: AFontMatcher_setStyle doesn't always return a
        // real bold variant (devices with stripped font catalogs return
        // Roboto-Regular for any weight request). When the engine was
        // asked for bold but the matched face isn't intrinsically bold,
        // FreeType-embolden the glyph after loading the outline.
        const bool synthBold = m_weight >= 600
                            && (face->style_flags & FT_STYLE_FLAG_BOLD) == 0;

        if (FT_Load_Glyph(face, glyphId,
                          synthBold ? FT_LOAD_DEFAULT : FT_LOAD_RENDER) != 0)
            return std::nullopt;

        if (synthBold)
        {
            FT_GlyphSlot_Embolden(face->glyph);
            if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0)
                return std::nullopt;
        }

        FT_GlyphSlot slot = face->glyph;
        GlyphBitmap bm;
        bm.width    = static_cast<int>(slot->bitmap.width);
        bm.height   = static_cast<int>(slot->bitmap.rows);
        bm.bearingX = slot->bitmap_left;
        bm.bearingY = slot->bitmap_top;
        if (bm.width > 0 && bm.height > 0 && slot->bitmap.buffer != nullptr)
        {
            bm.alpha.resize(static_cast<std::size_t>(bm.width) * static_cast<std::size_t>(bm.height));
            for (int y = 0; y < bm.height; ++y)
            {
                std::memcpy(bm.alpha.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(bm.width),
                            slot->bitmap.buffer + static_cast<std::ptrdiff_t>(y) * slot->bitmap.pitch,
                            static_cast<std::size_t>(bm.width));
            }
        }
        return bm;
    }

    FontMetrics metrics(float emSizePx) override;

private:
    std::shared_ptr<FreeTypeFontHandle> faceForFile(const char* path, std::size_t collectionIndex);
    std::shared_ptr<FreeTypeFontHandle> systemBase();

    AFontMatcher* m_matcher{ nullptr };
    uint16_t      m_weight{ 400 };

    // (file path + ":" + collection index) -> FT face. Stable identity
    // is required because AtlasKey uses the FontHandle pointer.
    std::unordered_map<std::string, std::shared_ptr<FreeTypeFontHandle>> m_faceCache;

    // Per-face current size so we don't redundantly FT_Set_Pixel_Sizes
    // when the engine bounces between fallback fonts.
    std::unordered_map<FT_Face, float> m_faceSize;

    // Cached system "base" font (used for metrics + as the line-height
    // anchor). Resolved lazily on first metrics() call.
    std::shared_ptr<FreeTypeFontHandle> m_systemBase;
};

std::shared_ptr<FreeTypeFontHandle>
AFontMatcherEngine::faceForFile(const char* path, std::size_t collectionIndex)
{
    std::string key = std::string(path) + ":" + std::to_string(collectionIndex);
    if (auto it = m_faceCache.find(key); it != m_faceCache.end())
        return it->second;

    auto handle = openFace(path, static_cast<int>(collectionIndex));
    if (handle == nullptr)
        return nullptr;
    m_faceCache.emplace(std::move(key), handle);
    return handle;
}

std::shared_ptr<FreeTypeFontHandle>
AFontMatcherEngine::systemBase()
{
    if (m_systemBase != nullptr)
        return m_systemBase;
    if (m_matcher == nullptr)
        return nullptr;

    // Ask AFontMatcher to match a single space — gives us the system's
    // default base font for line-height purposes.
    static const uint16_t kSpace = u' ';
    uint32_t              runLen = 0;
    AFont*                font   = AFontMatcher_match(m_matcher, "sans-serif", &kSpace, 1, &runLen);
    if (font == nullptr)
        return nullptr;

    std::string path(AFont_getFontFilePath(font));
    std::size_t idx = AFont_getCollectionIndex(font);
    AFont_close(font);

    m_systemBase = faceForFile(path.c_str(), idx);
    return m_systemBase;
}

std::vector<ShapedGlyph>
AFontMatcherEngine::shape(std::u16string_view text,
                          std::string_view    locale,
                          float               emSizePx)
{
    std::vector<ShapedGlyph> out;
    if (text.empty() || m_matcher == nullptr)
        return out;

    // AFontMatcher expects comma-separated BCP 47 tags. gettext gives us
    // POSIX-style locales (ja_JP, zh_CN); a minimal normalization swaps
    // the underscore for a dash, which covers the common cases. Full
    // POSIX→BCP-47 conversion (zh_CN→zh-Hans, etc.) is left as future
    // work.
    std::string normalizedLocale(locale.empty() ? "en" : std::string(locale));
    for (char& c : normalizedLocale)
        if (c == '_') c = '-';
    AFontMatcher_setLocales(m_matcher, normalizedLocale.c_str());

    out.reserve(text.size());

    std::size_t pos = 0;
    while (pos < text.size())
    {
        uint32_t runLength = 0;
        AFont*   font      = AFontMatcher_match(
            m_matcher,
            "sans-serif" /* family chain that respects weight selection */,
            reinterpret_cast<const uint16_t*>(text.data() + pos),
            static_cast<uint32_t>(text.size() - pos),
            &runLength);

        if (font == nullptr || runLength == 0)
        {
            ++pos;
            continue;
        }

        std::string path(AFont_getFontFilePath(font));
        std::size_t idx = AFont_getCollectionIndex(font);
        AFont_close(font);

        auto handle = faceForFile(path.c_str(), idx);
        if (handle == nullptr)
        {
            pos += runLength;
            continue;
        }

        FT_Face face = handle->face();
        float&  cur  = m_faceSize[face];
        ensureSize(face, emSizePx, cur);

        // Mirror the rasterize-time synthetic-bold decision so the
        // advance widths reported here match the wider emboldened
        // bitmaps the atlas will render.
        const bool synthBold = m_weight >= 600
                            && (face->style_flags & FT_STYLE_FLAG_BOLD) == 0;

        const std::size_t runEnd = pos + runLength;
        while (pos < runEnd && pos < text.size())
        {
            std::uint32_t ch = decodeUtf16(text, pos);
            if (ch == 0)
                continue;

            FT_UInt glyphId = FT_Get_Char_Index(face, ch);
            if (glyphId == 0)
                continue;
            if (FT_Load_Glyph(face, glyphId, FT_LOAD_DEFAULT) != 0)
                continue;
            if (synthBold)
                FT_GlyphSlot_Embolden(face->glyph);

            ShapedGlyph g;
            g.font     = handle;
            g.glyphId  = glyphId;
            g.xAdvance = static_cast<float>(face->glyph->advance.x >> 6);
            g.xOffset  = 0.0f;
            g.yOffset  = 0.0f;
            out.push_back(std::move(g));
        }
    }

    return out;
}

FontMetrics
AFontMatcherEngine::metrics(float emSizePx)
{
    auto base = systemBase();
    if (base == nullptr)
        return { 0, 0 };
    FT_Face face = base->face();
    float&  cur  = m_faceSize[face];
    ensureSize(face, emSizePx, cur);
    return {
        static_cast<int>(face->size->metrics.ascender  >> 6),
        static_cast<int>(-face->size->metrics.descender >> 6),
    };
}

} // anonymous namespace

std::unique_ptr<PlatformFontEngine>
createPlatformFontEngine(const std::filesystem::path& primaryFont, int faceIndex, bool bold)
{
    // Empty path: ask the system. On API 29+ we get AFontMatcher's
    // catalog-driven fallback cascade. On older devices we have no
    // system text engine to lean on, so we return null and let the
    // caller chain to the bundled DejaVuSans fonts.
    if (primaryFont.empty())
    {
        if (__builtin_available(android 29, *))
            return std::make_unique<AFontMatcherEngine>(bold);
        return nullptr;
    }

    // Explicit font file: bypass AFontMatcher entirely. The user picked
    // this font; running it through the system catalog would not produce
    // a better result and would silently ignore their choice. The bold
    // flag is ignored — caller supplies a bold file path directly.
    auto handle = openFace(primaryFont, faceIndex);
    if (handle == nullptr)
        return nullptr;
    return std::make_unique<FallbackEngine>(std::move(handle));
}

} // namespace celestia::text
