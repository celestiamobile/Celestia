// cubespheremesh.h
//
// Copyright (C) 2001-present, Celestia Development Team
//
// Chunked-LOD cube-sphere planet mesh. Each of the six cube faces is the root
// of a quadtree; every node ("chunk") is a fixed-resolution CHUNK_RES x CHUNK_RES
// grid covering a square sub-region of the face, projected to the unit sphere.
// The tree is descended each frame and refined toward the camera by screen-space
// error, so the mesh density stays roughly constant on screen from orbit down to
// the surface. Cracks between chunks of differing depth are removed by stitching
// the higher-resolution edge down to its coarser neighbour (watertight, so it
// also works for the translucent cloud and atmosphere shells).
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Eigen/Core>

#include <celengine/glsupport.h>
#include <celrender/gl/buffer.h>

// Set to 1 to draw the cube-sphere tessellation as a wireframe.
#define CUBESPHERE_WIREFRAME 0

class Texture;
class CelestiaGLProgram;

namespace celestia::math
{
class Frustum;
}

class CubeSphereMesh
{
public:
    static constexpr std::size_t MAX_TEXTURES = 6;

    enum
    {
        Normals  = 0x01,
        Tangents = 0x02,
    };

    CubeSphereMesh() = default;
    ~CubeSphereMesh();

    CubeSphereMesh(const CubeSphereMesh&) = delete;
    CubeSphereMesh& operator=(const CubeSphereMesh&) = delete;
    CubeSphereMesh(CubeSphereMesh&&) = delete;
    CubeSphereMesh& operator=(CubeSphereMesh&&) = delete;

    // Drop-in for LODSphereMesh::render. eyePos is the eye in object space
    // (per-axis normalized so an ellipsoid maps to the unit sphere) for culling
    // and LOD; pixWidth is the planet's apparent disc size in pixels; pixelSize
    // is the projected world size per pixel at unit distance (≈ radians/pixel),
    // which drives the screen-space-error refinement. enableHorizonCull discards
    // patches behind the horizon; disable it for shells drawn inside-out (e.g. the
    // atmosphere), whose visible side is the far hemisphere a horizon test removes.
    void render(unsigned int attributes,
                const celestia::math::Frustum& frustum,
                const Eigen::Vector3f& eyePos,
                float pixWidth,
                float pixelSize,
                Texture** tex,
                int nTextures,
                CelestiaGLProgram* program,
                bool enableHorizonCull = true);

private:
    // Identifies a quadtree node: at the given depth a face is split into
    // (1<<depth) cells per edge, (i,j) selects the cell. vertexSize is part of
    // the key because the shared mesh serves planets with differing layouts.
    struct ChunkKey
    {
        int face;
        int depth;
        std::uint32_t i;
        std::uint32_t j;
        int vertexSize;

        bool operator==(const ChunkKey& o) const
        {
            return face == o.face && depth == o.depth && i == o.i && j == o.j
                   && vertexSize == o.vertexSize;
        }
    };

    struct ChunkKeyHash
    {
        std::size_t operator()(const ChunkKey& k) const
        {
            std::size_t h = static_cast<std::size_t>(k.face);
            h = h * 1000003u + static_cast<std::size_t>(k.depth);
            h = h * 1000003u + k.i;
            h = h * 1000003u + k.j;
            h = h * 1000003u + static_cast<std::size_t>(k.vertexSize);
            return h;
        }
    };

    // A generated chunk mesh, held CPU-side. The triangle indices live in shared
    // stitch templates (see stitchTemplate), not here, since every chunk has the
    // same grid topology; only the vertices differ. The vertices are kept on the
    // CPU (not in a per-chunk VBO) so every visible chunk can be concatenated into
    // one batch buffer and the whole cube-sphere drawn in a single draw call.
    // lastUsed records the frame the chunk was last drawn so the cache can evict
    // cold chunks.
    struct ChunkMesh
    {
        std::vector<float> vertices;
        std::uint64_t lastUsed{ 0 };
    };

    void ensureBuffers();
    void ensureStitchTemplates();
    ChunkMesh* getOrCreateChunk(const ChunkKey& key, unsigned int attributes);
    // Append one chunk's vertices to the batch vertex buffer and its stitch
    // template (selected by edgeMask, with each index offset to the chunk's base
    // vertex) to the batch index buffer.
    void appendChunk(const ChunkMesh& chunk, unsigned int edgeMask);
    // Drop chunks not drawn this frame once the cache exceeds its budget,
    // evicting the least recently used first.
    void evictColdChunks();
    bool shouldSplit(int face, int depth, std::uint32_t i, std::uint32_t j,
                     const Eigen::Vector3f& eyePos) const;
    // Pass 1: descend the quadtree by screen-space error, recording the active
    // leaves (both as a draw list and as a membership set for neighbour queries).
    void collectLeaves(int face, int depth, std::uint32_t i, std::uint32_t j,
                       const Eigen::Vector3f& eyePos);
    // Depth of the leaf covering a same-face cell, or -1 if that region is split
    // finer than the cell (no single covering leaf).
    int coveringDepth(int face, int depth, std::uint32_t i, std::uint32_t j) const;
    // Pass 1b: restricted-quadtree 2:1 balance. needsBalanceSplit reports a leaf
    // with a same-face neighbour two or more levels finer; balanceLeaves force-
    // splits such leaves to a fixpoint, then rebuilds the draw list.
    bool needsBalanceSplit(int face, int depth,
                           std::uint32_t i, std::uint32_t j) const;
    void balanceLeaves();
    // Edge-stitch mask for a leaf: a bit is set when the neighbour across that
    // edge is covered by a coarser leaf, so this edge must drop to match it.
    unsigned int computeEdgeMask(int face, int depth,
                                 std::uint32_t i, std::uint32_t j) const;

    // One pre-built index template per 4-bit edge mask: bit set means that edge is
    // stitched down to a coarser (one level lower) neighbour by dropping its odd
    // boundary vertices. Shared by all chunks of any planet; held CPU-side because
    // the per-frame batch index buffer is assembled from them.
    static constexpr int NUM_STITCH_TEMPLATES = 16;

    int vertexSize{ 0 };
    int nTexturesUsed{ 0 };
    float lodPixelSize{ 1.0f };

    bool buffersInitialized{ false };
    bool stitchTemplatesBuilt{ false };
    GLuint vao{ 0 };
    std::array<std::vector<unsigned int>, NUM_STITCH_TEMPLATES> stitchTemplate{};
#if CUBESPHERE_WIREFRAME
    std::array<std::vector<unsigned int>, NUM_STITCH_TEMPLATES> stitchLineTemplate{};
#endif

    // Single batched draw: every visible chunk's vertices are concatenated into
    // batchVBO and its stitch indices (offset to its base vertex) into batchIBO,
    // then the whole cube-sphere is drawn with one glDrawElements. The CPU staging
    // vectors are kept as members so their capacity is reused across frames.
    celestia::gl::Buffer batchVBO{};
    celestia::gl::Buffer batchIBO{};
    std::vector<float> batchVertices{};
    std::vector<unsigned int> batchIndices{};

    std::unordered_map<ChunkKey, ChunkMesh, ChunkKeyHash> chunkCache{};
    std::uint64_t frameCounter{ 0 };

    // Active leaves for the current frame: a draw list and a packed-key set used
    // to look up neighbour depths when computing edge-stitch masks.
    std::vector<ChunkKey> frameLeaves{};
    std::unordered_set<std::uint64_t> frameLeafSet{};
};
