// cubemapvirtualtex.h
//
// Copyright (C) 2001-present, Celestia Development Team
//
// Tiled virtual texture whose tile pyramid matches the cube-sphere quadtree:
// six cube faces, each an independent quadtree of square tiles, addressed by
// (face, level, i, j). Unlike the equirectangular VirtualTexture, a cube-map
// tile lines up one-to-one with a cube-sphere chunk, so the cube-sphere renderer
// can stream surface detail without falling back to LODSphereMesh.
//
// The asynchronous tile-load and eviction machinery mirrors VirtualTexture; the
// two should be unified onto a shared tiled-texture base once both are proven.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include <celengine/resourcesystem.h>
#include <celengine/texture.h>
#include <celimage/image.h>

class CubeMapVirtualTexture : public Texture, public celestia::engine::ResourceCacheBase
{
public:
    static constexpr int NumFaces = 6;

    CubeMapVirtualTexture(const std::filesystem::path& _tilePath,
                          unsigned int _baseSplit,
                          unsigned int _tileSize,
                          const std::string& _tilePrefix,
                          const std::string& _tileType,
                          Colorspace _colorspace);
    ~CubeMapVirtualTexture() override;

    // Called on the render thread by TextureTraits::upload once the texture is in
    // the texture cache. Registers it so it gets per-frame ticks.
    void attachToResourceSystem(celestia::engine::ResourceSystem& system);

    // A cube-map virtual texture has no single 2D image; the equirectangular
    // getTile() entry point is unusable. Callers must use getCubeTile().
    TextureTile getTile(int lod, int u, int v) override;
    void bind() override;

    MeshMapping getMeshMapping() const override { return MeshMapping::CubeMap; }
    TextureTile getCubeTile(int face, int level, int i, int j) override;

    void beginUsage() override;
    void endUsage() override;

    // ResourceCacheBase — render thread only.
    std::size_t drainReady(std::size_t byteBudget) override;
    void purgeStale(std::uint64_t graceFrames) override;

private:
    enum class TileState : std::uint8_t
    {
        NotLoaded,
        Loading,
        Loaded,
        Failed,
    };

    struct Tile
    {
        Tile() = default;
        TileState                     state{ TileState::NotLoaded };
        std::uint64_t                 lastUsedFrame{ 0 };
        std::unique_ptr<ImageTexture> tex{ nullptr };
    };

    struct TileQuadtreeNode
    {
        TileQuadtreeNode() = default;
        std::unique_ptr<Tile> tile{ nullptr };
        std::array<std::unique_ptr<TileQuadtreeNode>, 4> children{ nullptr, nullptr, nullptr, nullptr };
    };

    // Decoded payload posted worker -> render. drainReady() turns the Image
    // into an ImageTexture on the render thread.
    struct ReadyTile
    {
        int                                           face{ 0 };
        unsigned int                                  level{ 0 };
        unsigned int                                  i{ 0 };
        unsigned int                                  j{ 0 };
        std::unique_ptr<celestia::engine::Image>      image;
        bool                                          decoded{ false };
    };

    // Shared cross-thread queue, captured by worker lambdas via shared_ptr so
    // an in-flight load can outlive the CubeMapVirtualTexture.
    struct SharedQueue
    {
        std::mutex             mutex;
        std::deque<ReadyTile>  ready;
    };

    void populateTileTree();
    void addTileToTree(std::unique_ptr<Tile> tile, int face,
                       unsigned int level, unsigned int i, unsigned int j);
    void requestTile(Tile* tile, int face,
                     unsigned int level, unsigned int i, unsigned int j);
    Tile* findTile(int face, unsigned int level,
                   unsigned int i, unsigned int j) const noexcept;
    static bool purgeTreeRecursive(TileQuadtreeNode& node,
                                   std::uint64_t now,
                                   std::uint64_t graceFrames);

private:
    std::filesystem::path tilePath;
    std::filesystem::path tileExt;
    std::string tilePrefix;
    unsigned int baseSplit{ 0 };
    unsigned int ticks{ 0 };
    unsigned int nResolutionLevels{ 0 };
    Colorspace colorspace;

    celestia::engine::ResourceSystem* m_system{ nullptr };
    std::shared_ptr<SharedQueue>      m_queue{ std::make_shared<SharedQueue>() };

    std::array<TileQuadtreeNode, NumFaces> tileTree{};
};

std::unique_ptr<CubeMapVirtualTexture>
LoadCubeMapVirtualTexture(const std::filesystem::path& filename, Texture::Colorspace colorspace);
