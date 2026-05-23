// fontshaper_windows.cpp
//
// Copyright (C) 2026-present, the Celestia Development Team
//
// DirectWrite font engine for Windows. Two modes:
//
// - Empty primary-font path: DWriteSystemEngine uses IDWriteTextLayout +
//   a custom IDWriteTextRenderer to extract DWRITE_GLYPH_RUNs. The text
//   format carries weight (DWRITE_FONT_WEIGHT_BOLD/NORMAL) and locale so
//   the system font fallback cascade picks locale-appropriate faces.
//
// - Explicit path: DWriteFileEngine loads the file via CreateFontFace and
//   does character-by-character glyph lookup against that single face,
//   skipping the cascade (the caller picked this font explicitly).
//
// Both engines rasterize through IDWriteGlyphRunAnalysis with the
// CLEARTYPE_3x1 texture mode, averaging RGB to single-channel grayscale
// for the existing alpha-keyed atlas.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "fontshaper.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

#include <celutil/logger.h>

#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace celestia::text
{

namespace
{

using celestia::util::GetLogger;
using Microsoft::WRL::ComPtr;

// FontHandle wrapping IDWriteFontFace*. AddRef on construction, Release on
// destruction — symmetrical regardless of where the face came from
// (CreateFontFace gives us a ref we hand off, layout-extracted faces are
// borrowed and we add our own ref).
class DWriteFontHandle final : public FontHandle
{
public:
    explicit DWriteFontHandle(IDWriteFontFace* face) : m_face(face)
    {
        if (m_face != nullptr) m_face->AddRef();
    }
    ~DWriteFontHandle() override
    {
        if (m_face != nullptr) m_face->Release();
    }
    DWriteFontHandle(const DWriteFontHandle&) = delete;
    DWriteFontHandle& operator=(const DWriteFontHandle&) = delete;

    IDWriteFontFace* face() const { return m_face; }

private:
    IDWriteFontFace* m_face;
};

IDWriteFactory*
getDWriteFactory()
{
    static IDWriteFactory* s_factory = nullptr;
    if (s_factory == nullptr)
    {
        HRESULT hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&s_factory));
        if (FAILED(hr))
        {
            GetLogger()->error("DirectWrite: DWriteCreateFactory failed (hr=0x{:x})\n",
                               static_cast<unsigned>(hr));
            return nullptr;
        }
    }
    return s_factory;
}

// sRGB → linear LUT used by the CLEARTYPE_3x1 → grayscale averaging.
// Averaging in gamma space underweights stem coverage and is why
// CLEARTYPE_3x1 collapsed to (r+g+b)/3 looks thinner than native
// grayscale output from FreeType / CoreText.
constexpr float kInvGamma = 1.0f / 2.2f;
const std::array<float, 256>& srgbToLinearLUT()
{
    static const std::array<float, 256> table = []() {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i)
            t[i] = std::pow(static_cast<float>(i) / 255.0f, 2.2f);
        return t;
    }();
    return table;
}

// Rasterise a single glyph via IDWriteGlyphRunAnalysis. CLEARTYPE_3x1
// produces an RGB-subpixel texture; we gamma-correctly average the
// channels into the single-channel alpha buffer the atlas wants.
//
// GDI_NATURAL rendering mode pairs with NATURAL measuring to give
// GDI-style stem snapping, which matches the visual heaviness of the
// FreeType/CoreText backends — without it, NATURAL leaves stems
// unsnapped and DirectWrite output looks noticeably lighter.
std::optional<GlyphBitmap>
rasterizeWithDWrite(IDWriteFontFace* face, std::uint32_t glyphId, float emSizePx)
{
    IDWriteFactory* factory = getDWriteFactory();
    if (factory == nullptr) return std::nullopt;

    UINT16              idx     = static_cast<UINT16>(glyphId);
    FLOAT               advance = 0.0f;
    DWRITE_GLYPH_OFFSET offset  = { 0.0f, 0.0f };

    DWRITE_GLYPH_RUN run = {};
    run.fontFace      = face;
    run.fontEmSize    = emSizePx;
    run.glyphCount    = 1;
    run.glyphIndices  = &idx;
    run.glyphAdvances = &advance;
    run.glyphOffsets  = &offset;
    run.isSideways    = FALSE;
    run.bidiLevel     = 0;

    ComPtr<IDWriteGlyphRunAnalysis> analysis;
    HRESULT hr = factory->CreateGlyphRunAnalysis(
        &run, 1.0f, nullptr,
        DWRITE_RENDERING_MODE_GDI_NATURAL,
        DWRITE_MEASURING_MODE_NATURAL,
        0.0f, 0.0f, &analysis);
    if (FAILED(hr)) return std::nullopt;

    RECT bounds = {};
    hr = analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds);
    if (FAILED(hr)) return std::nullopt;

    const int width  = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0)
    {
        // Empty glyph (e.g. space) — zero-sized bitmap, atlas skips emission.
        return GlyphBitmap{ 0, 0, 0, 0, {} };
    }

    std::vector<BYTE> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    hr = analysis->CreateAlphaTexture(
        DWRITE_TEXTURE_CLEARTYPE_3x1,
        &bounds,
        rgb.data(),
        static_cast<UINT32>(rgb.size()));
    if (FAILED(hr)) return std::nullopt;

    GlyphBitmap bm;
    bm.width    = width;
    bm.height   = height;
    bm.bearingX = bounds.left;
    // DirectWrite uses y-down for bounds (bounds.top is negative for glyphs
    // extending above the baseline); FreeType bearingY is "distance from
    // baseline to top of bitmap, positive upward".
    bm.bearingY = -bounds.top;
    bm.alpha.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    const auto& lut = srgbToLinearLUT();
    for (int i = 0; i < width * height; ++i)
    {
        const auto base = static_cast<std::size_t>(i) * 3;
        const float linear = (lut[rgb[base + 0]]
                            + lut[rgb[base + 1]]
                            + lut[rgb[base + 2]]) / 3.0f;
        const float srgb   = std::pow(linear, kInvGamma) * 255.0f;
        const int   v      = static_cast<int>(srgb + 0.5f);
        bm.alpha[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    return bm;
}

// IDWriteTextRenderer impl that accumulates the runs DirectWrite emits.
// Used as a stack object inside shape() — refcounting is a no-op.
class GlyphRunCapture final : public IDWriteTextRenderer
{
public:
    struct Run
    {
        IDWriteFontFace*                 face;
        float                            emSize;
        std::vector<UINT16>              indices;
        std::vector<FLOAT>               advances;
        std::vector<DWRITE_GLYPH_OFFSET> offsets;
    };

    std::vector<Run> runs;

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void*                              /*clientCtx*/,
        FLOAT                              /*baselineX*/,
        FLOAT                              /*baselineY*/,
        DWRITE_MEASURING_MODE              /*measuring*/,
        const DWRITE_GLYPH_RUN*            glyphRun,
        const DWRITE_GLYPH_RUN_DESCRIPTION*/*description*/,
        IUnknown*                          /*effect*/) override
    {
        if (glyphRun == nullptr || glyphRun->glyphCount == 0)
            return S_OK;

        Run r;
        r.face   = glyphRun->fontFace;
        r.emSize = glyphRun->fontEmSize;
        r.indices.assign(glyphRun->glyphIndices,
                         glyphRun->glyphIndices + glyphRun->glyphCount);
        r.advances.assign(glyphRun->glyphAdvances,
                          glyphRun->glyphAdvances + glyphRun->glyphCount);
        if (glyphRun->glyphOffsets != nullptr)
            r.offsets.assign(glyphRun->glyphOffsets,
                             glyphRun->glyphOffsets + glyphRun->glyphCount);
        else
            r.offsets.resize(glyphRun->glyphCount);
        runs.push_back(std::move(r));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override
    {
        *disabled = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override
    {
        *transform = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixelsPerDip) override
    {
        *pixelsPerDip = 1.0f;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** obj) override
    {
        if (iid == __uuidof(IUnknown)
            || iid == __uuidof(IDWritePixelSnapping)
            || iid == __uuidof(IDWriteTextRenderer))
        {
            *obj = this;
            return S_OK;
        }
        *obj = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};

// Query Windows for the family name of the current UI text font. Reads
// NONCLIENTMETRICSW::lfMessageFont, which is what the OS itself uses for
// dialogs and chrome — picks up locale-specific UI fonts (Yu Gothic UI
// on JP Windows, Microsoft YaHei UI on CN, etc.) and tracks future
// Windows releases that change the default away from Segoe UI without
// us having to chase the name.
std::wstring
querySystemUIFontFamily()
{
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(NONCLIENTMETRICSW);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &ncm, 0))
        return ncm.lfMessageFont.lfFaceName;
    return L"Segoe UI"; // last-resort default
}

// Locale string conversion: gettext gives POSIX (ja_JP, zh_CN); DirectWrite
// is stricter and wants BCP-47 (ja-JP). Swap underscore for dash.
std::wstring
normalizeLocale(std::string_view posix)
{
    std::wstring out;
    out.reserve(posix.size());
    for (char c : posix)
        out.push_back(static_cast<wchar_t>(c == '_' ? '-' : c));
    if (out.empty()) out = L"en-US";
    return out;
}

// ---------------------------------------------------------------------------
// DWriteSystemEngine — empty path, uses TextLayout + system fallback cascade
// ---------------------------------------------------------------------------

class DWriteSystemEngine final : public PlatformFontEngine
{
public:
    DWriteSystemEngine(std::wstring family, DWRITE_FONT_WEIGHT weight)
        : m_family(std::move(family)), m_weight(weight) {}

    std::vector<ShapedGlyph> shape(std::u16string_view text,
                                   std::string_view   locale,
                                   float              emSizePx) override;

    std::optional<GlyphBitmap> rasterize(const FontHandle& font,
                                         std::uint32_t     glyphId,
                                         float             emSizePx) override
    {
        const auto& h = static_cast<const DWriteFontHandle&>(font);
        return rasterizeWithDWrite(h.face(), glyphId, emSizePx);
    }

    FontMetrics metrics(float emSizePx) override;

private:
    std::shared_ptr<DWriteFontHandle> handleFor(IDWriteFontFace* face);
    ComPtr<IDWriteFontFace>           baseFace();

    std::wstring                                                         m_family;
    DWRITE_FONT_WEIGHT                                                   m_weight;
    std::unordered_map<IDWriteFontFace*, std::shared_ptr<DWriteFontHandle>> m_handleCache;
    ComPtr<IDWriteFontFace>                                              m_baseFace;
};

std::shared_ptr<DWriteFontHandle>
DWriteSystemEngine::handleFor(IDWriteFontFace* face)
{
    if (face == nullptr) return nullptr;
    if (auto it = m_handleCache.find(face); it != m_handleCache.end())
        return it->second;
    auto handle = std::make_shared<DWriteFontHandle>(face);
    m_handleCache.emplace(face, handle);
    return handle;
}

ComPtr<IDWriteFontFace>
DWriteSystemEngine::baseFace()
{
    if (m_baseFace) return m_baseFace;

    IDWriteFactory* factory = getDWriteFactory();
    if (factory == nullptr) return {};

    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(factory->GetSystemFontCollection(&collection, FALSE))) return {};

    UINT32 idx    = 0;
    BOOL   exists = FALSE;
    if (FAILED(collection->FindFamilyName(m_family.c_str(), &idx, &exists)) || !exists)
        return {};

    ComPtr<IDWriteFontFamily> family;
    if (FAILED(collection->GetFontFamily(idx, &family))) return {};

    ComPtr<IDWriteFont> font;
    if (FAILED(family->GetFirstMatchingFont(m_weight,
                                            DWRITE_FONT_STRETCH_NORMAL,
                                            DWRITE_FONT_STYLE_NORMAL,
                                            &font)))
        return {};

    if (FAILED(font->CreateFontFace(&m_baseFace))) return {};
    return m_baseFace;
}

std::vector<ShapedGlyph>
DWriteSystemEngine::shape(std::u16string_view text,
                          std::string_view    locale,
                          float               emSizePx)
{
    std::vector<ShapedGlyph> out;
    if (text.empty()) return out;

    IDWriteFactory* factory = getDWriteFactory();
    if (factory == nullptr) return out;

    const std::wstring wlocale = normalizeLocale(locale);

    ComPtr<IDWriteTextFormat> format;
    if (FAILED(factory->CreateTextFormat(
            m_family.c_str(),
            nullptr,
            m_weight,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            emSizePx,
            wlocale.c_str(),
            &format)))
        return out;

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(
            reinterpret_cast<const WCHAR*>(text.data()),
            static_cast<UINT32>(text.size()),
            format.Get(),
            // Arbitrary large layout box; we only care about the glyph
            // runs, not line wrapping.
            1.0e6f, 1.0e6f,
            &layout)))
        return out;

    GlyphRunCapture capture;
    if (FAILED(layout->Draw(nullptr, &capture, 0.0f, 0.0f)))
        return out;

    out.reserve(text.size());
    for (auto& run : capture.runs)
    {
        auto handle = handleFor(run.face);
        if (handle == nullptr) continue;

        const std::size_t n = run.indices.size();
        for (std::size_t i = 0; i < n; ++i)
        {
            ShapedGlyph g;
            g.font     = handle;
            g.glyphId  = run.indices[i];
            g.xAdvance = run.advances[i];
            // DirectWrite ascenderOffset is positive towards the top
            // (y-up baseline-relative); the renderer's yOffset is added
            // to a y-down cursor, so flip the sign.
            g.xOffset  = run.offsets[i].advanceOffset;
            g.yOffset  = -run.offsets[i].ascenderOffset;
            out.push_back(std::move(g));
        }
    }
    return out;
}

FontMetrics
DWriteSystemEngine::metrics(float emSizePx)
{
    auto face = baseFace();
    if (!face) return { 0, 0 };

    DWRITE_FONT_METRICS m;
    face->GetMetrics(&m);
    const float scale = emSizePx / static_cast<float>(m.designUnitsPerEm);
    return {
        static_cast<int>(std::ceil(static_cast<float>(m.ascent) * scale)),
        static_cast<int>(std::ceil(static_cast<float>(m.descent) * scale)),
    };
}

// ---------------------------------------------------------------------------
// DWriteFileEngine — explicit path, single face, no cascade
// ---------------------------------------------------------------------------

class DWriteFileEngine final : public PlatformFontEngine
{
public:
    explicit DWriteFileEngine(std::shared_ptr<DWriteFontHandle> handle)
        : m_handle(std::move(handle)) {}

    std::vector<ShapedGlyph> shape(std::u16string_view text,
                                   std::string_view   /*locale*/,
                                   float              emSizePx) override;

    std::optional<GlyphBitmap> rasterize(const FontHandle& font,
                                         std::uint32_t     glyphId,
                                         float             emSizePx) override
    {
        const auto& h = static_cast<const DWriteFontHandle&>(font);
        return rasterizeWithDWrite(h.face(), glyphId, emSizePx);
    }

    FontMetrics metrics(float emSizePx) override
    {
        if (!m_handle) return { 0, 0 };
        DWRITE_FONT_METRICS m;
        m_handle->face()->GetMetrics(&m);
        const float scale = emSizePx / static_cast<float>(m.designUnitsPerEm);
        return {
            static_cast<int>(std::ceil(static_cast<float>(m.ascent) * scale)),
            static_cast<int>(std::ceil(static_cast<float>(m.descent) * scale)),
        };
    }

private:
    std::shared_ptr<DWriteFontHandle> m_handle;
};

std::vector<ShapedGlyph>
DWriteFileEngine::shape(std::u16string_view text,
                        std::string_view    /*locale*/,
                        float               emSizePx)
{
    std::vector<ShapedGlyph> out;
    if (text.empty() || !m_handle) return out;

    IDWriteFontFace* face = m_handle->face();

    DWRITE_FONT_METRICS fm;
    face->GetMetrics(&fm);
    const float scale = emSizePx / static_cast<float>(fm.designUnitsPerEm);

    out.reserve(text.size());

    std::size_t i = 0;
    while (i < text.size())
    {
        UINT32 cp;
        if (text[i] < 0xd800 || text[i] >= 0xe000)
        {
            cp = text[i];
            ++i;
        }
        else if (text[i] < 0xdc00
                 && (i + 1) < text.size()
                 && text[i + 1] >= 0xdc00
                 && text[i + 1] < 0xe000)
        {
            cp = 0x10000u
                 + ((static_cast<UINT32>(text[i])     - 0xd800u) << 10)
                 +  (static_cast<UINT32>(text[i + 1]) - 0xdc00u);
            i += 2;
        }
        else
        {
            ++i;
            continue;
        }

        UINT16 glyphIdx = 0;
        if (FAILED(face->GetGlyphIndices(&cp, 1, &glyphIdx)) || glyphIdx == 0)
        {
            // Tofu fallback: try '?'.
            UINT32 q = static_cast<UINT32>(u'?');
            if (FAILED(face->GetGlyphIndices(&q, 1, &glyphIdx)) || glyphIdx == 0)
                continue;
        }

        DWRITE_GLYPH_METRICS gm;
        if (FAILED(face->GetDesignGlyphMetrics(&glyphIdx, 1, &gm, FALSE)))
            continue;

        ShapedGlyph g;
        g.font     = m_handle;
        g.glyphId  = glyphIdx;
        g.xAdvance = static_cast<float>(gm.advanceWidth) * scale;
        g.xOffset  = 0.0f;
        g.yOffset  = 0.0f;
        out.push_back(std::move(g));
    }

    return out;
}

} // anonymous namespace

std::unique_ptr<PlatformFontEngine>
createPlatformFontEngine(const std::filesystem::path& primaryFont, int faceIndex, bool bold)
{
    const DWRITE_FONT_WEIGHT weight = bold ? DWRITE_FONT_WEIGHT_BOLD
                                           : DWRITE_FONT_WEIGHT_NORMAL;

    if (primaryFont.empty())
    {
        // Query the OS for the current UI font family rather than
        // hardcoding "Segoe UI" — that name is the present-day default
        // but changes by locale (Yu Gothic UI, Microsoft YaHei UI, ...)
        // and may shift in future Windows releases. DirectWrite's font
        // matcher resolves a bold variant from whatever family the OS
        // hands back, and synthesises bold otherwise.
        return std::make_unique<DWriteSystemEngine>(querySystemUIFontFamily(), weight);
    }

    IDWriteFactory* factory = getDWriteFactory();
    if (factory == nullptr) return nullptr;

    const std::wstring wpath = primaryFont.wstring();

    ComPtr<IDWriteFontFile> file;
    if (FAILED(factory->CreateFontFileReference(wpath.c_str(), nullptr, &file)))
    {
        GetLogger()->error("DirectWrite: could not open font file: {}\n", primaryFont);
        return nullptr;
    }

    BOOL                  isSupported = FALSE;
    DWRITE_FONT_FILE_TYPE fileType    = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    DWRITE_FONT_FACE_TYPE faceType    = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    UINT32                numFaces    = 0;
    if (FAILED(file->Analyze(&isSupported, &fileType, &faceType, &numFaces)) || !isSupported)
    {
        GetLogger()->error("DirectWrite: unsupported font file: {}\n", primaryFont);
        return nullptr;
    }

    const UINT32 idx = (faceIndex >= 0 && static_cast<UINT32>(faceIndex) < numFaces)
                       ? static_cast<UINT32>(faceIndex)
                       : 0u;

    IDWriteFontFile* fileArr[1] = { file.Get() };
    IDWriteFontFace* face       = nullptr;
    if (FAILED(factory->CreateFontFace(faceType, 1, fileArr, idx,
                                       DWRITE_FONT_SIMULATIONS_NONE, &face))
        || face == nullptr)
    {
        GetLogger()->error("DirectWrite: could not create font face: {}\n", primaryFont);
        return nullptr;
    }

    // DWriteFontHandle AddRef's, so balance the CreateFontFace ref.
    auto handle = std::make_shared<DWriteFontHandle>(face);
    face->Release();
    return std::make_unique<DWriteFileEngine>(std::move(handle));
}

} // namespace celestia::text
