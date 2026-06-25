// cubemapvirtualtex.cpp
//
// Copyright (C) 2001-present, Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.


#include "cubemapvirtualtex.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include <celutil/filetype.h>
#include <celutil/logger.h>
#include <celutil/parser.h>
#include <celutil/tokenizer.h>

namespace util = celestia::util;

using celestia::util::GetLogger;
using celestia::engine::Image;

namespace
{

constexpr int MaxResolutionLevels = 13;

// Per-face subdirectory names, indexed to match the cube-sphere faceBases order
// (+X, -X, +Y, -Y, +Z, -Z).
constexpr std::array<std::string_view, CubeMapVirtualTexture::NumFaces> FaceDirNames = {
    "pos_x", "neg_x", "pos_y", "neg_y", "pos_z", "neg_z",
};

constexpr bool
isPow2(int x)
{
    return ((x & (x - 1)) == 0);
}

std::unique_ptr<CubeMapVirtualTexture>
CreateCubeMapVirtualTexture(const util::AssociativeArray* texParams,
                            const std::filesystem::path& path,
                            Texture::Colorspace colorspace)
{
    const std::string* imageDirectory = texParams->getString("ImageDirectory");
    if (imageDirectory == nullptr)
    {
        GetLogger()->error("ImageDirectory missing in cube-map virtual texture.\n");
        return nullptr;
    }

    // BaseSplit is optional for a cube-map texture (level 0 is a single tile per
    // face by default); when present it pre-splits level 0 into 2^BaseSplit tiles.
    unsigned int baseSplit = 0;
    if (std::optional<double> baseSplitVal = texParams->getNumber<double>("BaseSplit");
        baseSplitVal.has_value())
    {
        if (*baseSplitVal < 0.0 || *baseSplitVal != floor(*baseSplitVal))
        {
            GetLogger()->error("BaseSplit in cube-map virtual texture has bad value\n");
            return nullptr;
        }
        baseSplit = static_cast<unsigned int>(*baseSplitVal);
    }

    std::optional<double> tileSize = texParams->getNumber<double>("TileSize");
    if (!tileSize.has_value())
    {
        GetLogger()->error("TileSize is missing from cube-map virtual texture\n");
        return nullptr;
    }

    if (*tileSize != floor(*tileSize) ||
        *tileSize < 64.0 ||
        !isPow2((int) *tileSize))
    {
        GetLogger()->error("Cube-map virtual texture tile size must be a power of two >= 64\n");
        return nullptr;
    }

    std::string tileType = "dds";
    if (const std::string* tileTypeVal = texParams->getString("TileType"); tileTypeVal != nullptr)
    {
        tileType = *tileTypeVal;
    }

    std::string tilePrefix = "tx_";
    if (const std::string* tilePrefixVal = texParams->getString("TilePrefix"); tilePrefixVal != nullptr)
    {
        tilePrefix = *tilePrefixVal;
    }

    // If absolute directory notation for ImageDirectory is used, don't prepend
    // the current add-on path.
    std::filesystem::path directory(*imageDirectory);

    if (directory.is_relative())
        directory = path / directory;
    return std::make_unique<CubeMapVirtualTexture>(directory,
                                                   baseSplit,
                                                   (unsigned int) *tileSize,
                                                   tilePrefix,
                                                   tileType,
                                                   colorspace);
}


std::unique_ptr<CubeMapVirtualTexture>
LoadCubeMapVirtualTexture(std::istream& in, const std::filesystem::path& path, Texture::Colorspace colorspace)
{
    util::Tokenizer tokenizer(in);
    util::Parser parser(&tokenizer);

    tokenizer.nextToken();
    if (auto tokenValue = tokenizer.getNameValue(); tokenValue != "CubeMapVirtualTexture")
    {
        return nullptr;
    }

    const util::Value texParamsValue = parser.readValue();
    const util::AssociativeArray* texParams = texParamsValue.getHash();
    if (texParams == nullptr)
    {
        GetLogger()->error("Error parsing cube-map virtual texture\n");
        return nullptr;
    }

    return CreateCubeMapVirtualTexture(texParams, path, colorspace);
}

} // end unnamed namespace


// A cube-map virtual texture consists of six independent face quadtrees. Each
// face is split into one or more levels of detail; level L has (2^L)^2 square
// tiles (times 2^baseSplit per edge when a base split is configured). Tiles are
// loaded from disk as they become visible and evicted when they fall out of use.
CubeMapVirtualTexture::CubeMapVirtualTexture(const std::filesystem::path& _tilePath,
                                             unsigned int _baseSplit,
                                             unsigned int _tileSize,
                                             const std::string& _tilePrefix,
                                             const std::string& _tileType,
                                             Colorspace _colorspace) :
    Texture(_tileSize, _tileSize),
    tilePath(_tilePath),
    tilePrefix(_tilePrefix),
    baseSplit(_baseSplit),
    colorspace(_colorspace)
{
    assert(_tileSize != 0 && isPow2(_tileSize));
    tileExt = fmt::format(".{:s}", _tileType);
    populateTileTree();

    if (DetermineFileType(tileExt, true) == ContentType::DXT5NormalMap)
        setFormatOptions(Texture::DXT5NormalMap);
}


CubeMapVirtualTexture::~CubeMapVirtualTexture()
{
    // Unregister first so we stop getting per-frame ticks. In-flight workers
    // still hold m_queue via shared_ptr; their payloads are dropped once the
    // last reference goes away.
    if (m_system != nullptr)
        m_system->unregisterCache(this);
}


void
CubeMapVirtualTexture::attachToResourceSystem(celestia::engine::ResourceSystem& system)
{
    assert(m_system == nullptr);
    m_system = &system;
    m_system->registerCache(this);
}


TextureTile
CubeMapVirtualTexture::getTile(int /*lod*/, int /*u*/, int /*v*/)
{
    // A cube-map virtual texture has no equirectangular projection; the cube-
    // sphere renderer must use getCubeTile() instead.
    return TextureTile(0);
}


TextureTile
CubeMapVirtualTexture::getCubeTile(int face, int level, int i, int j)
{
    assert(m_system != nullptr);

    if (face < 0 || face >= NumFaces)
        return TextureTile(0);

    // A request deeper than the deepest on-disk level is expected once the
    // cube-sphere refines past the pyramid: the quadtree walk below stops at the
    // deepest existing node and the coarser-ancestor fallback returns a sub-rect,
    // so don't reject treeLevel >= nResolutionLevels here (that left a black hole
    // when zoomed in past the last level).
    int treeLevel = level + static_cast<int>(baseSplit);
    if (treeLevel < 0 ||
        i < 0 || i >= (1 << treeLevel) ||
        j < 0 || j >= (1 << treeLevel))
    {
        return TextureTile(0);
    }

    // Walk the face quadtree toward (treeLevel, i, j), tracking the deepest
    // existing tile and the deepest one that's already Loaded.
    const TileQuadtreeNode* node = &tileTree[face];
    Tile* deepestExisting = node->tile.get();
    unsigned int deepestExistingLevel = 0;
    Tile* bestResident = nullptr;
    unsigned int bestResidentLevel = 0;

    auto consider = [&](Tile* t, unsigned int n)
    {
        if (t == nullptr)
            return;
        deepestExisting      = t;
        deepestExistingLevel = n;
        if (t->state == TileState::Loaded)
        {
            bestResident      = t;
            bestResidentLevel = n;
        }
    };
    consider(deepestExisting, 0);

    for (int n = 0; n < treeLevel; n++)
    {
        unsigned int mask = 1u << (treeLevel - n - 1);
        unsigned int child = (((static_cast<unsigned int>(j) & mask) << 1)
                              | (static_cast<unsigned int>(i) & mask)) >> (treeLevel - n - 1);
        if (!node->children[child])
            break;

        node = node->children[child].get();
        consider(node->tile.get(), n + 1);
    }

    // No tile exists here at all - transparent.
    if (deepestExisting == nullptr)
        return TextureTile(0);

    // Kick off the async load if it hasn't been requested (or was evicted).
    if (deepestExisting->state == TileState::NotLoaded)
    {
        unsigned int tileI = static_cast<unsigned int>(i) >> (treeLevel - deepestExistingLevel);
        unsigned int tileJ = static_cast<unsigned int>(j) >> (treeLevel - deepestExistingLevel);
        requestTile(deepestExisting, face, deepestExistingLevel, tileI, tileJ);
    }

    // While it decodes, fall back to the coarsest resident ancestor.
    if (bestResident == nullptr)
        return TextureTile(0);

    bestResident->lastUsedFrame = m_system->currentFrame();

    unsigned int levelDiff = static_cast<unsigned int>(treeLevel) - bestResidentLevel;
    float texDU = 1.0f / (float) (1u << levelDiff);
    float texDV = texDU;
    float texU = (static_cast<unsigned int>(i) & ((1u << levelDiff) - 1)) * texDU;
    float texV = (static_cast<unsigned int>(j) & ((1u << levelDiff) - 1)) * texDV;

    return TextureTile(bestResident->tex->getName(), texU, texV, texDU, texDV);
}


void
CubeMapVirtualTexture::bind()
{
    // Treating a virtual texture like an ordinary one will not work; this is a
    // weakness in the class hierarchy.
}


void
CubeMapVirtualTexture::beginUsage()
{
    ticks++;
}


void
CubeMapVirtualTexture::endUsage()
{
}


void
CubeMapVirtualTexture::requestTile(Tile* tile, int face,
                                   unsigned int level, unsigned int i, unsigned int j)
{
    assert(tile != nullptr);
    assert(tile->state == TileState::NotLoaded);
    assert(m_system != nullptr);

    tile->state = TileState::Loading;

    unsigned int diskLevel = level - baseSplit;
    auto filename = std::filesystem::u8path(fmt::format("{}{}_{}", tilePrefix, i, j));
    filename += tileExt;
    auto path = tilePath
                / std::filesystem::u8path(std::string{ FaceDirNames[face] })
                / fmt::format("level{:d}", diskLevel)
                / filename;

    // Capture the queue by shared_ptr so an in-flight worker can outlive this
    // CubeMapVirtualTexture; no raw CubeMapVirtualTexture* is captured.
    auto queue = m_queue;
    Colorspace cs = colorspace;
    m_system->submit([queue, path = std::move(path), face, level, i, j, cs]()
    {
        ReadyTile ready;
        ready.face = face;
        ready.level = level;
        ready.i = i;
        ready.j = j;
        ready.image = celestia::engine::Image::load(path);
        ready.decoded = ready.image != nullptr;
        if (ready.decoded && cs == Texture::Colorspace::LinearColorspace)
            ready.image->forceLinear();

        std::scoped_lock lock(queue->mutex);
        queue->ready.push_back(std::move(ready));
    });
}


CubeMapVirtualTexture::Tile*
CubeMapVirtualTexture::findTile(int face, unsigned int level,
                                unsigned int i, unsigned int j) const noexcept
{
    const TileQuadtreeNode* node = &tileTree[face];
    for (unsigned int n = 0; n < level; n++)
    {
        unsigned int mask = 1u << (level - n - 1);
        unsigned int child = (((j & mask) << 1) | (i & mask)) >> (level - n - 1);
        if (!node->children[child])
            return nullptr;
        node = node->children[child].get();
    }
    return node->tile.get();
}


std::size_t
CubeMapVirtualTexture::drainReady(std::size_t byteBudget)
{
    assert(m_system != nullptr);

    std::size_t bytes = 0;
    while (bytes < byteBudget)
    {
        ReadyTile ready;
        {
            std::scoped_lock lock(m_queue->mutex);
            if (m_queue->ready.empty())
                break;
            ready = std::move(m_queue->ready.front());
            m_queue->ready.pop_front();
        }

        Tile* tile = findTile(ready.face, ready.level, ready.i, ready.j);
        // Drop the result if the tile was evicted or the node is gone.
        if (tile == nullptr || tile->state != TileState::Loading)
            continue;

        if (!ready.decoded || !ready.image)
        {
            tile->state = TileState::Failed;
            continue;
        }

        if (!isPow2(ready.image->getWidth()) || !isPow2(ready.image->getHeight()))
        {
            tile->state = TileState::Failed;
            continue;
        }

        unsigned int diskLevel = ready.level - baseSplit;
        MipMapMode mipMapMode = diskLevel == 0 ? DefaultMipMaps : NoMipMaps;
        tile->tex = std::make_unique<ImageTexture>(*ready.image, EdgeClamp, mipMapMode);
        tile->state = TileState::Loaded;
        tile->lastUsedFrame = m_system->currentFrame();
        bytes += static_cast<std::size_t>(ready.image->getSize());
    }
    return bytes;
}


bool
CubeMapVirtualTexture::purgeTreeRecursive(TileQuadtreeNode& node,
                                          std::uint64_t now,
                                          std::uint64_t graceFrames)
{
    // Recurse first so we know whether any finer descendant is still resident.
    bool subtreeLoaded = false;
    for (const auto& child : node.children)
    {
        if (child)
            subtreeLoaded |= purgeTreeRecursive(*child, now, graceFrames);
    }

    if (node.tile && node.tile->state == TileState::Loaded)
    {
        // Keep a coarser tile while a finer descendant is still loaded: it's a
        // cheap fallback and avoids blank tiles when zooming back out.
        if (!subtreeLoaded && (now - node.tile->lastUsedFrame) > graceFrames)
        {
            // Drop the GPU texture; reset so a later getCubeTile() reloads it.
            node.tile->tex.reset();
            node.tile->state = TileState::NotLoaded;
        }
        else
        {
            subtreeLoaded = true;
        }
    }

    return subtreeLoaded;
}


void
CubeMapVirtualTexture::purgeStale(std::uint64_t graceFrames)
{
    assert(m_system != nullptr);
    std::uint64_t now = m_system->currentFrame();
    for (auto& root : tileTree)
        purgeTreeRecursive(root, now, graceFrames);
}


void
CubeMapVirtualTexture::populateTileTree()
{
    unsigned int maxLevel = 0;

    for (int face = 0; face < NumFaces; ++face)
    {
        std::filesystem::path faceDir =
            tilePath / std::filesystem::u8path(std::string{ FaceDirNames[face] });

        for (int diskLevel = 0; diskLevel < MaxResolutionLevels; ++diskLevel)
        {
            std::filesystem::path path = faceDir / fmt::format("level{:d}", diskLevel);
            std::error_code ec;
            if (!std::filesystem::is_directory(path, ec))
                continue;

            unsigned int treeLevel = static_cast<unsigned int>(diskLevel) + baseSplit;
            if (treeLevel > maxLevel)
                maxLevel = treeLevel;
            int limit = 1 << treeLevel;

            for (const auto& d : std::filesystem::directory_iterator(path, ec))
            {
                if (!std::filesystem::is_regular_file(d, ec))
                    continue;

                int i = -1;
                int j = -1;
                if (auto filename = d.path().filename().string();
                    filename.size() < tilePrefix.size()
                    || std::string_view{filename.data(), tilePrefix.size()} != tilePrefix
                    || std::sscanf(filename.c_str() + tilePrefix.size(), "%d_%d.", &i, &j) != 2
                    || i < 0 || i >= limit
                    || j < 0 || j >= limit)
                    continue;

                addTileToTree(std::make_unique<Tile>(), face, treeLevel,
                              (unsigned int) i, (unsigned int) j);
            }
        }
    }

    nResolutionLevels = maxLevel + 1;
}


void
CubeMapVirtualTexture::addTileToTree(std::unique_ptr<Tile> tile, int face,
                                     unsigned int level, unsigned int i, unsigned int j)
{
    TileQuadtreeNode* node = &tileTree[face];

    for (unsigned int n = 0; n < level; n++)
    {
        unsigned int mask = 1u << (level - n - 1);
        unsigned int child = (((j & mask) << 1) | (i & mask)) >> (level - n - 1);
        if (!node->children[child])
            node->children[child] = std::make_unique<TileQuadtreeNode>();
        node = node->children[child].get();
    }

    // Verify that the tile doesn't already exist
    if (!node->tile)
        node->tile = std::move(tile);
}


std::unique_ptr<CubeMapVirtualTexture>
LoadCubeMapVirtualTexture(const std::filesystem::path& filename, Texture::Colorspace colorspace)
{
    std::ifstream in(filename, std::ios::in);

    if (!in.good())
    {
        GetLogger()->error("Error opening cube-map virtual texture file: {}\n", filename);
        return nullptr;
    }

    return LoadCubeMapVirtualTexture(in, filename.parent_path(), colorspace);
}
