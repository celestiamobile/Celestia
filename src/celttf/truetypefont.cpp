// truetypefont.cpp
//
// Copyright (C) 2019-2022, Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "truetypefont.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <celcompat/charconv.h>
#include <celengine/glsupport.h>
#include <celengine/render.h>
#include <celengine/texture.h>
#include <celimage/image.h>
#include <celrender/gl/buffer.h>
#include <celrender/gl/vertexobject.h>
#include <celutil/gettext.h>
#include <celutil/logger.h>

#include "fontshaper.h"

using celestia::compat::from_chars;
using celestia::engine::Image;
using celestia::engine::PixelFormat;
using celestia::text::FontHandle;
using celestia::text::FontMetrics;
using celestia::text::GlyphBitmap;
using celestia::text::PlatformFontEngine;
using celestia::text::ShapedGlyph;
using celestia::util::GetLogger;
namespace gl = celestia::gl;

namespace
{

constexpr int kDefaultSize = 12;

struct AtlasKey
{
    const FontHandle* font;
    std::uint32_t     glyphId;

    bool operator==(const AtlasKey& other) const noexcept
    {
        return font == other.font && glyphId == other.glyphId;
    }
};

struct AtlasKeyHash
{
    std::size_t operator()(const AtlasKey& k) const noexcept
    {
        // Combine hash of the FontHandle pointer with glyphId using the
        // boost::hash_combine mix constant.
        std::size_t h1 = std::hash<const FontHandle*>{}(k.font);
        std::size_t h2 = std::hash<std::uint32_t>{}(k.glyphId);
        return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6) + (h1 >> 2));
    }
};

// Per-glyph atlas entry. Owns a shared_ptr<FontHandle> so the engine's font
// cache stays alive as long as the atlas references it.
struct AtlasGlyph
{
    std::shared_ptr<FontHandle> font;
    int                         width{0};
    int                         height{0};
    int                         bearingX{0};
    int                         bearingY{0};
    float                       tx{0.0f};
    float                       ty{0.0f};

    // Held only between rasterization and the next atlas-texture rebuild,
    // then released. Re-rasterized on subsequent rebuilds.
    std::vector<std::uint8_t>   alpha;
};

struct FontDescriptor
{
    std::filesystem::path path;
    int                   index{0};
    int                   pointSize{0};
    float                 scale{1.0f};
    int                   screenDpi{96};
    std::string           locale;
    FontWeight            weight{FontWeight::Regular};
};

// Resolve the current UI locale via gettext: _("LANGUAGE") evaluates to the
// translation if one is loaded, else returns the original pointer. Mirrors
// the pattern used in src/celutil/fsutils.cpp:101.
std::string
currentLocale()
{
    const char* orig = N_("LANGUAGE");
    const char* lang = _(orig);
    if (lang == orig)
        return "en";
    return lang;
}

float
computeEmSizePx(float scale, int pointSize, int screenDpi)
{
    return scale * static_cast<float>(pointSize) * static_cast<float>(screenDpi) / 72.0f;
}

// "fontpath,index,size" parser. Used internally to resolve the format
// strings stored in celestia.cfg before handing them to the font engine.
std::filesystem::path
ParseFontName(const std::filesystem::path& filename, int& index, int& size)
{
    auto fn = filename.string();
    if (auto ps = fn.rfind(','); ps != std::string::npos)
    {
        if (from_chars(&fn[ps + 1], &fn[fn.size()], size).ec == std::errc())
        {
            if (auto pi = fn.rfind(',', ps - 1); pi != std::string::npos)
            {
                if (from_chars(&fn[pi + 1], &fn[pi], index).ec == std::errc())
                    return fn.substr(0, pi);
            }
            return fn.substr(0, ps);
        }
    }
    return filename;
}

} // anonymous namespace

struct TextureFontPrivate
{
    struct FontVertex
    {
        FontVertex(float _x, float _y, float _u, float _v) : x(_x), y(_y), u(_u), v(_v) {}
        float x, y;
        float u, v;
    };

    static_assert(std::is_standard_layout_v<FontVertex>);

    explicit TextureFontPrivate(const Renderer* renderer);
    ~TextureFontPrivate() = default;
    TextureFontPrivate()                                       = delete;
    TextureFontPrivate(const TextureFontPrivate&)              = delete;
    TextureFontPrivate(TextureFontPrivate&&)                   = default;
    TextureFontPrivate& operator=(const TextureFontPrivate&)   = delete;
    TextureFontPrivate& operator=(TextureFontPrivate&&)        = default;

    bool                       loadFont(const std::filesystem::path& filename, int index, int size, FontWeight weight);
    std::pair<float, float>    render(std::u16string_view line, float x, float y);
    int                        getWidth(std::u16string_view line);

    AtlasGlyph*                ensureGlyph(const std::shared_ptr<FontHandle>& font, std::uint32_t glyphId);
    void                       rebuildAtlasTexture();
    CelestiaGLProgram*         getProgram();
    void                       flush();

    const Renderer*    m_renderer;
    CelestiaGLProgram* m_prog{ nullptr };

    std::unique_ptr<PlatformFontEngine> m_engine;
    FontDescriptor                      m_descriptor;
    FontMetrics                         m_metrics{ 0, 0 };
    float                               m_emSizePx{ 0.0f };

    std::unordered_map<AtlasKey, AtlasGlyph, AtlasKeyHash> m_atlas;
    bool                                                   m_atlasDirty{ false };
    int                                                    m_texWidth{ 0 };
    int                                                    m_texHeight{ 0 };
    std::unique_ptr<ImageTexture>                          m_tex;

    Eigen::Matrix4f m_projection{ Eigen::Matrix4f::Identity() };
    Eigen::Matrix4f m_modelView{ Eigen::Matrix4f::Identity() };

    std::vector<FontVertex> m_fontVertices;

    gl::VertexObject m_vao{ gl::VertexObject::Primitive::Triangles };
    gl::Buffer       m_vbo{ gl::Buffer::TargetHint::Array };

    bool m_shaderInUse{ false };

    static constexpr std::size_t MaxVertices = 256; // VBO size 4 kB, must be multiple of 4
    static constexpr std::size_t MaxIndices  = MaxVertices / 4 * 6;
};

TextureFontPrivate::TextureFontPrivate(const Renderer* renderer) : m_renderer(renderer)
{
    m_vao.addVertexBuffer(
        m_vbo,
        CelestiaGLProgram::VertexCoordAttributeIndex,
        2,
        gl::VertexObject::DataType::Float,
        false,
        sizeof(FontVertex),
        offsetof(FontVertex, x));
    m_vao.addVertexBuffer(
        m_vbo,
        CelestiaGLProgram::TextureCoord0AttributeIndex,
        2,
        gl::VertexObject::DataType::Float,
        false,
        sizeof(FontVertex),
        offsetof(FontVertex, u));

    std::vector<std::uint16_t> indexes;
    indexes.reserve(MaxIndices);
    for (std::uint16_t index = 0; index < static_cast<std::uint16_t>(MaxIndices); index += 4)
    {
        indexes.push_back(index + 0);
        indexes.push_back(index + 1);
        indexes.push_back(index + 2);
        indexes.push_back(index + 1);
        indexes.push_back(index + 3);
        indexes.push_back(index + 2);
    }

    m_vao.setIndexBuffer(gl::Buffer(gl::Buffer::TargetHint::ElementArray, indexes), 0, gl::VertexObject::IndexType::UnsignedShort);
}

bool
TextureFontPrivate::loadFont(const std::filesystem::path& filename, int index, int size, FontWeight weight)
{
    m_descriptor.path      = filename;
    m_descriptor.index     = index;
    m_descriptor.pointSize = size > 0 ? size : kDefaultSize;
    m_descriptor.scale     = m_renderer->getTextScaleFactor();
    m_descriptor.screenDpi = m_renderer->getScreenDpi();
    m_descriptor.locale    = currentLocale();
    m_descriptor.weight    = weight;

    m_emSizePx = computeEmSizePx(m_descriptor.scale, m_descriptor.pointSize, m_descriptor.screenDpi);

    m_engine = celestia::text::createPlatformFontEngine(filename, index > 0 ? index : 0, weight == FontWeight::Bold);
    if (m_engine == nullptr)
        return false;

    m_metrics = m_engine->metrics(m_emSizePx);
    return true;
}

AtlasGlyph*
TextureFontPrivate::ensureGlyph(const std::shared_ptr<FontHandle>& font, std::uint32_t glyphId)
{
    AtlasKey key{ font.get(), glyphId };
    if (auto it = m_atlas.find(key); it != m_atlas.end())
        return &it->second;

    auto bm = m_engine->rasterize(*font, glyphId, m_emSizePx);
    if (!bm.has_value())
        return nullptr;

    AtlasGlyph ag;
    ag.font     = font;
    ag.width    = bm->width;
    ag.height   = bm->height;
    ag.bearingX = bm->bearingX;
    ag.bearingY = bm->bearingY;
    ag.alpha    = std::move(bm->alpha);

    m_atlasDirty = true;
    auto [it, _] = m_atlas.emplace(key, std::move(ag));
    return &it->second;
}

void
TextureFontPrivate::rebuildAtlasTexture()
{
    // First pass: measure required atlas dimensions using a simple shelf
    // packer (matches the previous per-glyph algorithm).
    int roww = 0;
    int rowh = 0;
    int w    = 0;
    int h    = 0;

    for (const auto& [_, ag] : m_atlas)
    {
        if (ag.width == 0 || ag.height == 0)
            continue;
        if (roww + ag.width + 1 >= celestia::gl::maxTextureSize)
        {
            w = std::max(w, roww);
            h += rowh;
            roww = 0;
            rowh = 0;
        }
        roww += ag.width + 1;
        rowh = std::max(rowh, ag.height);
    }
    w = std::max(w, roww);
    h += rowh;

    if (w == 0 || h == 0)
    {
        m_tex.reset();
        m_texWidth  = 0;
        m_texHeight = 0;
        m_atlasDirty = false;
        return;
    }

    m_texWidth  = w;
    m_texHeight = h;

    // Re-rasterize any glyph whose alpha buffer was released after the
    // previous atlas build. Done in a second pass keyed by AtlasKey so we
    // pick up the glyphId from the map key.
    for (auto& [key, ag] : m_atlas)
    {
        if (ag.width == 0 || ag.height == 0) continue;
        if (!ag.alpha.empty()) continue;
        if (auto bm = m_engine->rasterize(*ag.font, key.glyphId, m_emSizePx); bm.has_value())
        {
            ag.alpha = std::move(bm->alpha);
        }
    }

    auto img = std::make_unique<Image>(PixelFormat::Luminance, w, h);

    int ox  = 0;
    int oy  = 0;
    rowh = 0;
    for (auto& [_, ag] : m_atlas)
    {
        if (ag.width == 0 || ag.height == 0)
        {
            ag.tx = 0.0f;
            ag.ty = 0.0f;
            continue;
        }
        if (ox + ag.width > w)
        {
            oy += rowh;
            rowh = 0;
            ox   = 0;
        }
        for (int y = 0; y < ag.height; ++y)
        {
            std::uint8_t*       dst = img->getPixelRow(oy + y) + ox * img->getComponents();
            const std::uint8_t* src = ag.alpha.data() + static_cast<std::ptrdiff_t>(y) * ag.width;
            std::memcpy(dst, src, static_cast<std::size_t>(ag.width));
        }
        ag.tx = static_cast<float>(ox) / static_cast<float>(w);
        ag.ty = static_cast<float>(oy) / static_cast<float>(h);
        rowh = std::max(rowh, ag.height);
        ox += ag.width + 1;
    }

    m_tex = std::make_unique<ImageTexture>(*img, Texture::EdgeClamp, Texture::NoMipMaps);

    // Release per-glyph alpha buffers; not needed until the next rebuild.
    for (auto& [_, ag] : m_atlas)
    {
        ag.alpha.clear();
        ag.alpha.shrink_to_fit();
    }

    m_atlasDirty = false;
}

std::pair<float, float>
TextureFontPrivate::render(std::u16string_view line, float x, float y)
{
    if (m_engine == nullptr)
        return { x, y };

    auto shaped = m_engine->shape(line, m_descriptor.locale, m_emSizePx);

    // Pass 1: ensure every shaped glyph has an atlas entry.
    for (const auto& g : shaped)
        ensureGlyph(g.font, g.glyphId);

    if (m_atlasDirty)
    {
        // Existing queued vertices reference the previous atlas layout;
        // draw them before remapping texture coordinates.
        flush();
        rebuildAtlasTexture();
    }

    if (m_tex == nullptr)
    {
        // Atlas is empty (e.g. all glyphs zero-sized) — still advance x.
        for (const auto& g : shaped)
            x += g.xAdvance;
        return { x, y };
    }

    m_tex->bind();

    // Pass 2: emit quads.
    for (const auto& g : shaped)
    {
        AtlasKey key{ g.font.get(), g.glyphId };
        auto it = m_atlas.find(key);
        if (it == m_atlas.end())
        {
            x += g.xAdvance;
            continue;
        }
        const auto& ag = it->second;

        const float x1 = x + g.xOffset + static_cast<float>(ag.bearingX);
        const float y1 = y + g.yOffset + static_cast<float>(ag.bearingY) - static_cast<float>(ag.height);
        const float fw = static_cast<float>(ag.width);
        const float fh = static_cast<float>(ag.height);
        const float x2 = x1 + fw;
        const float y2 = y1 + fh;

        x += g.xAdvance;

        if (ag.width == 0 || ag.height == 0)
            continue;

        const float tx1 = ag.tx;
        const float ty1 = ag.ty;
        const float tx2 = tx1 + fw / static_cast<float>(m_texWidth);
        const float ty2 = ty1 + fh / static_cast<float>(m_texHeight);

        m_fontVertices.emplace_back(x1, y1, tx1, ty2);
        m_fontVertices.emplace_back(x2, y1, tx2, ty2);
        m_fontVertices.emplace_back(x1, y2, tx1, ty1);
        m_fontVertices.emplace_back(x2, y2, tx2, ty1);

        if (m_fontVertices.size() == MaxVertices) flush();
    }

    return { x, y };
}

int
TextureFontPrivate::getWidth(std::u16string_view line)
{
    if (m_engine == nullptr)
        return 0;
    auto shaped = m_engine->shape(line, m_descriptor.locale, m_emSizePx);
    float width = 0.0f;
    for (const auto& g : shaped)
        width += g.xAdvance;
    return static_cast<int>(width);
}

CelestiaGLProgram*
TextureFontPrivate::getProgram()
{
    if (m_prog == nullptr)
        m_prog = m_renderer->getShaderManager().getShader(StaticShader::Text);
    return m_prog;
}

void
TextureFontPrivate::flush()
{
    if (m_fontVertices.size() < 4)
        return;

    m_vbo.invalidateData().setData(m_fontVertices, gl::Buffer::BufferUsage::StreamDraw);
    m_vao.getIndexBuffer().bind();
    m_vao.draw(static_cast<GLsizei>(m_fontVertices.size() / 4 * 6));
    m_vbo.unbind();
    m_vao.getIndexBuffer().unbind();

    m_fontVertices.clear();
}

// ---------------------------------------------------------------------------
// TextureFont public surface
// ---------------------------------------------------------------------------

TextureFont::TextureFont(const Renderer* renderer) :
    impl(std::make_unique<TextureFontPrivate>(renderer))
{
}

// Needs visible TextureFontPrivate definition.
TextureFont::~TextureFont() = default;

std::pair<float, float>
TextureFont::render(std::u16string_view line, float xoffset, float yoffset) const
{
    return impl->render(line, xoffset, yoffset);
}

int
TextureFont::getWidth(std::u16string_view line) const
{
    return impl->getWidth(line);
}

int
TextureFont::getHeight() const
{
    return impl->m_metrics.maxAscent + impl->m_metrics.maxDescent;
}

int
TextureFont::getMaxAscent() const
{
    return impl->m_metrics.maxAscent;
}

void
TextureFont::setMaxAscent(int v)
{
    impl->m_metrics.maxAscent = v;
}

int
TextureFont::getMaxDescent() const
{
    return impl->m_metrics.maxDescent;
}

void
TextureFont::setMaxDescent(int v)
{
    impl->m_metrics.maxDescent = v;
}

void
TextureFont::bind()
{
    auto* prog = impl->getProgram();
    if (prog == nullptr)
        return;

    if (impl->m_atlasDirty)
    {
        impl->flush();
        impl->rebuildAtlasTexture();
    }

    if (impl->m_tex == nullptr)
        return;

    glActiveTexture(GL_TEXTURE0);
    impl->m_tex->bind();
    prog->use();
    prog->samplerParam("atlasTex") = 0;
    prog->setMVPMatrices(impl->m_projection, impl->m_modelView);
    impl->m_shaderInUse = true;
}

void
TextureFont::setMVPMatrices(const Eigen::Matrix4f& p, const Eigen::Matrix4f& m)
{
    impl->m_projection = p;
    impl->m_modelView  = m;
    auto* prog         = impl->getProgram();
    if (prog != nullptr && impl->m_shaderInUse)
    {
        flush();
        prog->setMVPMatrices(p, m);
    }
}

void
TextureFont::unbind()
{
    flush();
    impl->m_shaderInUse = false;
}

void
TextureFont::flush()
{
    impl->flush();
}

bool
TextureFont::update()
{
    const int         currentDpi    = impl->m_descriptor.screenDpi;
    const float       currentScale  = impl->m_descriptor.scale;
    const std::string currentLocale = impl->m_descriptor.locale;

    const int         newDpi    = impl->m_renderer->getScreenDpi();
    const float       newScale  = impl->m_renderer->getTextScaleFactor();
    const std::string newLocale = ::currentLocale();

    if (currentDpi == newDpi && currentScale == newScale && currentLocale == newLocale)
        return false;

    auto newImpl = std::make_unique<TextureFontPrivate>(impl->m_renderer);
    if (newImpl->loadFont(impl->m_descriptor.path, impl->m_descriptor.index, impl->m_descriptor.pointSize, impl->m_descriptor.weight))
    {
        impl = std::move(newImpl);
        return true;
    }

    GetLogger()->warn("Could not update font for dpi/scale/locale change\n");
    return false;
}

// ---------------------------------------------------------------------------
// LoadTextureFont
// ---------------------------------------------------------------------------

struct FontCacheKey
{
    std::filesystem::path filename;
    int                   index;
    int                   size;
    FontWeight            weight;

    bool operator==(const FontCacheKey& other) const
    {
        return filename == other.filename
               && index == other.index
               && size == other.size
               && weight == other.weight;
    }
};

template<> struct std::hash<FontCacheKey>
{
    std::size_t operator()(const FontCacheKey& k) const
    {
        return std::hash<std::string>()(k.filename.string())
               ^ std::hash<int>()(k.index)
               ^ std::hash<int>()(k.size)
               ^ std::hash<int>()(static_cast<int>(k.weight));
    }
};

using FontCache = std::unordered_map<FontCacheKey, std::weak_ptr<TextureFont>>;

std::shared_ptr<TextureFont>
LoadTextureFont(const Renderer*               r,
                const std::filesystem::path&  filename,
                std::optional<int>            index,
                std::optional<int>            size,
                FontWeight                    weight)
{
    static FontCache* fontCache = nullptr;
    if (fontCache == nullptr)
        fontCache = new FontCache;

    // Parse "fontpath,index,size" form; caller-provided optionals override
    // whatever's embedded in the filename. An empty path is passed through
    // unmodified so the platform engine can pick its system default.
    int  parsedIndex = 0;
    int  parsedSize  = kDefaultSize;
    auto path        = filename.empty() ? filename : ParseFontName(filename, parsedIndex, parsedSize);

    const int finalIndex = index.value_or(parsedIndex);
    const int finalSize  = size.value_or(parsedSize);

    std::weak_ptr<TextureFont>&  font = (*fontCache)[{ path, finalIndex, finalSize, weight }];
    std::shared_ptr<TextureFont> ret  = font.lock();
    if (ret == nullptr)
    {
        ret = std::make_shared<TextureFont>(r);
        if (!ret->impl->loadFont(path, finalIndex, finalSize, weight))
        {
            GetLogger()->error("Could not load font at path: {} index: {} size: {}\n", path, finalIndex, finalSize);
            return nullptr;
        }
        font = ret;
    }
    return ret;
}
