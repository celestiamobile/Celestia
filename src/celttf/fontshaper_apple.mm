// fontshaper_apple.mm
//
// Copyright (C) 2026-present, the Celestia Development Team
//
// CoreText-based font engine for macOS and iOS. Default backend on Apple
// platforms: shapes via CTLine/CTRun (locale-aware Han unification and
// system fallback cascade), rasterizes via CTFontDrawGlyphs into a
// CGBitmapContext. An empty primary-font path resolves to the system UI
// font for the requested language; otherwise a TTF/OTF file is loaded as
// the base font and CoreText's cascade still supplies fallback glyphs.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "fontshaper.h"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>

#include <celutil/logger.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

namespace celestia::text
{

namespace
{

using celestia::util::GetLogger;

class CoreTextFontHandle final : public FontHandle
{
public:
    explicit CoreTextFontHandle(CTFontRef font) : m_font(font)
    {
        CFRetain(m_font);
    }
    ~CoreTextFontHandle() override
    {
        CFRelease(m_font);
    }
    CoreTextFontHandle(const CoreTextFontHandle&) = delete;
    CoreTextFontHandle& operator=(const CoreTextFontHandle&) = delete;

    CTFontRef font() const { return m_font; }

private:
    CTFontRef m_font;
};

struct CFRefHash
{
    std::size_t operator()(CFTypeRef ref) const noexcept
    {
        return static_cast<std::size_t>(CFHash(ref));
    }
};

struct CFRefEq
{
    bool operator()(CFTypeRef a, CFTypeRef b) const noexcept
    {
        return CFEqual(a, b) != 0;
    }
};

class CoreTextFontEngine final : public PlatformFontEngine
{
public:
    explicit CoreTextFontEngine(CTFontDescriptorRef descriptor)
        : m_descriptor(descriptor)
    {
        CFRetain(m_descriptor);
    }

    ~CoreTextFontEngine() override
    {
        if (m_sizedBase != nullptr)
            CFRelease(m_sizedBase);
        CFRelease(m_descriptor);
    }

    std::vector<ShapedGlyph> shape(std::u16string_view text,
                                   std::string_view   locale,
                                   float              emSizePx) override;

    std::optional<GlyphBitmap> rasterize(const FontHandle& font,
                                         std::uint32_t     glyphId,
                                         float             emSizePx) override;

    FontMetrics metrics(float emSizePx) override;

private:
    CTFontRef                                sizedBase(float emSizePx);
    std::shared_ptr<CoreTextFontHandle>      handleFor(CTFontRef ctFont);

    CTFontDescriptorRef m_descriptor;

    // Sized CTFontRef cached at the current emSizePx. CTFontRef is
    // size-bound; we recreate when the size changes (which TextureFont only
    // does via a full engine rebuild, but the interface allows per-call
    // size, so we handle it).
    CTFontRef m_sizedBase{ nullptr };
    float     m_sizedBaseSize{ -1.0f };

    // Cache of CTFontRef -> CoreTextFontHandle so identical fonts returned
    // from CoreText's fallback cascade share atlas keys.
    std::unordered_map<CTFontRef,
                       std::shared_ptr<CoreTextFontHandle>,
                       CFRefHash,
                       CFRefEq> m_handleCache;
};

CTFontRef
CoreTextFontEngine::sizedBase(float emSizePx)
{
    if (m_sizedBase != nullptr && m_sizedBaseSize == emSizePx)
        return m_sizedBase;

    if (m_sizedBase != nullptr)
    {
        CFRelease(m_sizedBase);
        m_sizedBase = nullptr;
        // Handles cached at the old size are no longer valid keys.
        m_handleCache.clear();
    }

    m_sizedBase     = CTFontCreateWithFontDescriptor(m_descriptor, static_cast<CGFloat>(emSizePx), nullptr);
    m_sizedBaseSize = emSizePx;
    return m_sizedBase;
}

std::shared_ptr<CoreTextFontHandle>
CoreTextFontEngine::handleFor(CTFontRef ctFont)
{
    if (auto it = m_handleCache.find(ctFont); it != m_handleCache.end())
        return it->second;
    auto handle = std::make_shared<CoreTextFontHandle>(ctFont);
    m_handleCache.emplace(ctFont, handle);
    return handle;
}

std::vector<ShapedGlyph>
CoreTextFontEngine::shape(std::u16string_view text,
                          std::string_view    locale,
                          float               emSizePx)
{
    std::vector<ShapedGlyph> out;
    if (text.empty())
        return out;

    CTFontRef base = sizedBase(emSizePx);
    if (base == nullptr)
        return out;

    CFStringRef cfText = CFStringCreateWithCharacters(
        nullptr,
        reinterpret_cast<const UniChar*>(text.data()),
        static_cast<CFIndex>(text.size()));
    if (cfText == nullptr)
        return out;

    CFMutableAttributedStringRef attr = CFAttributedStringCreateMutable(nullptr, 0);
    CFAttributedStringReplaceString(attr, CFRangeMake(0, 0), cfText);

    CFRange range = CFRangeMake(0, CFStringGetLength(cfText));
    CFAttributedStringSetAttribute(attr, range, kCTFontAttributeName, base);

    if (!locale.empty())
    {
        CFStringRef cfLocale = CFStringCreateWithBytes(
            nullptr,
            reinterpret_cast<const UInt8*>(locale.data()),
            static_cast<CFIndex>(locale.size()),
            kCFStringEncodingUTF8,
            false);
        if (cfLocale != nullptr)
        {
            CFAttributedStringSetAttribute(attr, range, kCTLanguageAttributeName, cfLocale);
            CFRelease(cfLocale);
        }
    }

    CTLineRef line = CTLineCreateWithAttributedString(attr);
    if (line == nullptr)
    {
        CFRelease(attr);
        CFRelease(cfText);
        return out;
    }

    CFArrayRef runs     = CTLineGetGlyphRuns(line);
    CFIndex    runCount = CFArrayGetCount(runs);

    out.reserve(text.size());

    double cumulativeAdvance = 0.0;

    for (CFIndex i = 0; i < runCount; ++i)
    {
        CTRunRef run        = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, i));
        CFIndex  glyphCount = CTRunGetGlyphCount(run);
        if (glyphCount == 0)
            continue;

        CFDictionaryRef runAttrs = CTRunGetAttributes(run);
        CTFontRef       runFont  = static_cast<CTFontRef>(CFDictionaryGetValue(runAttrs, kCTFontAttributeName));
        if (runFont == nullptr)
            continue;

        auto handle = handleFor(runFont);

        std::vector<CGGlyph>  glyphs(static_cast<std::size_t>(glyphCount));
        std::vector<CGPoint>  positions(static_cast<std::size_t>(glyphCount));
        std::vector<CGSize>   advances(static_cast<std::size_t>(glyphCount));

        CTRunGetGlyphs(run, CFRangeMake(0, 0), glyphs.data());
        CTRunGetPositions(run, CFRangeMake(0, 0), positions.data());
        CTRunGetAdvances(run, CFRangeMake(0, 0), advances.data());

        for (CFIndex j = 0; j < glyphCount; ++j)
        {
            ShapedGlyph g;
            g.font     = handle;
            g.glyphId  = static_cast<std::uint32_t>(glyphs[j]);
            g.xAdvance = static_cast<float>(advances[j].width);
            // CTRun positions are absolute within the line; convert to
            // per-glyph offsets relative to the cumulative-advance cursor
            // so the renderer's walk model stays simple. Non-zero offsets
            // arise from shaping (Indic clusters, Arabic ligatures, etc.).
            g.xOffset  = static_cast<float>(positions[j].x - cumulativeAdvance);
            g.yOffset  = static_cast<float>(positions[j].y);
            out.push_back(std::move(g));
            cumulativeAdvance += advances[j].width;
        }
    }

    CFRelease(line);
    CFRelease(attr);
    CFRelease(cfText);

    return out;
}

std::optional<GlyphBitmap>
CoreTextFontEngine::rasterize(const FontHandle& font,
                              std::uint32_t     glyphId,
                              float             /*emSizePx*/)
{
    // CTFontRef on Apple is size-bound, so the handle carries the size;
    // emSizePx is ignored here.
    const auto& handle = static_cast<const CoreTextFontHandle&>(font);
    CTFontRef   ctFont = handle.font();
    CGGlyph     glyph  = static_cast<CGGlyph>(glyphId);

    CGRect rect;
    CTFontGetBoundingRectsForGlyphs(ctFont, kCTFontOrientationDefault, &glyph, &rect, 1);

    if (rect.size.width <= 0.0 || rect.size.height <= 0.0)
    {
        // Empty glyph (space etc.) — return a zero-sized bitmap, the atlas
        // will skip emitting a quad.
        return GlyphBitmap{ 0, 0, 0, 0, {} };
    }

    const int x0 = static_cast<int>(std::floor(rect.origin.x));
    const int y0 = static_cast<int>(std::floor(rect.origin.y));
    const int x1 = static_cast<int>(std::ceil(rect.origin.x + rect.size.width));
    const int y1 = static_cast<int>(std::ceil(rect.origin.y + rect.size.height));

    const int width  = x1 - x0;
    const int height = y1 - y0;
    if (width <= 0 || height <= 0)
        return std::nullopt;

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);

    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef    ctx  = CGBitmapContextCreate(
        pixels.data(),
        static_cast<size_t>(width),
        static_cast<size_t>(height),
        8,
        static_cast<size_t>(width),
        gray,
        kCGImageAlphaNone);
    CGColorSpaceRelease(gray);

    if (ctx == nullptr)
        return std::nullopt;

    // Clear to black, draw glyph in white. The resulting luminance values
    // double as alpha coverage for the atlas.
    CGContextSetGrayFillColor(ctx, 0.0, 1.0);
    CGContextFillRect(ctx, CGRectMake(0, 0, width, height));
    CGContextSetGrayFillColor(ctx, 1.0, 1.0);

    // Translate so the glyph's bounding-box bottom-left sits at the
    // context's user-space origin (0, 0). CG memory row 0 corresponds to
    // the top of the bitmap, which matches FreeType's bitmap-top
    // convention — no post-flip needed.
    const CGPoint origin = CGPointMake(static_cast<CGFloat>(-x0),
                                       static_cast<CGFloat>(-y0));
    CTFontDrawGlyphs(ctFont, &glyph, &origin, 1, ctx);

    CGContextRelease(ctx);

    GlyphBitmap bm;
    bm.width    = width;
    bm.height   = height;
    bm.bearingX = x0;
    bm.bearingY = y1;
    bm.alpha    = std::move(pixels);
    return bm;
}

FontMetrics
CoreTextFontEngine::metrics(float emSizePx)
{
    CTFontRef base = sizedBase(emSizePx);
    if (base == nullptr)
        return { 0, 0 };
    return {
        static_cast<int>(std::ceil(CTFontGetAscent(base))),
        static_cast<int>(std::ceil(CTFontGetDescent(base))),
    };
}

} // anonymous namespace

std::unique_ptr<PlatformFontEngine>
createPlatformFontEngine(const std::filesystem::path& primaryFont, int faceIndex)
{
    CTFontDescriptorRef descriptor = nullptr;

    if (primaryFont.empty())
    {
        // System UI font; size 0 picks the default and the size is
        // overridden per shape() call via CTFontCreateWithFontDescriptor.
        CTFontRef systemFont = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 0.0, nullptr);
        if (systemFont != nullptr)
        {
            descriptor = CTFontCopyFontDescriptor(systemFont);
            CFRelease(systemFont);
        }
    }
    else
    {
        CFStringRef pathStr = CFStringCreateWithCString(
            nullptr,
            primaryFont.string().c_str(),
            kCFStringEncodingUTF8);
        if (pathStr != nullptr)
        {
            CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, pathStr, kCFURLPOSIXPathStyle, false);
            CFRelease(pathStr);
            if (url != nullptr)
            {
                CFArrayRef descriptors = CTFontManagerCreateFontDescriptorsFromURL(url);
                CFRelease(url);
                if (descriptors != nullptr)
                {
                    const CFIndex count = CFArrayGetCount(descriptors);
                    if (count > 0)
                    {
                        const CFIndex idx = (faceIndex >= 0 && faceIndex < count) ? faceIndex : 0;
                        descriptor = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(descriptors, idx));
                        CFRetain(descriptor);
                    }
                    CFRelease(descriptors);
                }
            }
        }
    }

    if (descriptor == nullptr)
    {
        GetLogger()->error("Could not create CoreText font descriptor for: {}\n", primaryFont);
        return nullptr;
    }

    auto engine = std::make_unique<CoreTextFontEngine>(descriptor);
    CFRelease(descriptor); // engine took its own retain
    return engine;
}

} // namespace celestia::text
