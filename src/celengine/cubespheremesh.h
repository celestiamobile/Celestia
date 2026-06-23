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

#include <cstddef>
#include <cstdint>
#include <unordered_map>
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
    // which drives the screen-space-error refinement.
    void render(unsigned int attributes,
                const celestia::math::Frustum& frustum,
                const Eigen::Vector3f& eyePos,
                float pixWidth,
                float pixelSize,
                Texture** tex,
                int nTextures,
                CelestiaGLProgram* program);

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

    // A generated, GPU-resident chunk mesh plus its unit-sphere cull bounds:
    // a cone (axis, cosHalfAngle) about the chunk centre for the horizon test
    // and a bounding sphere (center, radius) for the frustum test.
    struct ChunkMesh
    {
        celestia::gl::Buffer vbuf{ celestia::gl::Buffer::TargetHint::Array };
        celestia::gl::Buffer ibuf{ celestia::gl::Buffer::TargetHint::ElementArray };
        GLsizei indexCount{ 0 };
        Eigen::Vector3f axis{ Eigen::Vector3f::UnitZ() };
        Eigen::Vector3f center{ Eigen::Vector3f::Zero() };
        float cosHalfAngle{ 1.0f };
        float radius{ 0.0f };
#if CUBESPHERE_WIREFRAME
        celestia::gl::Buffer lineBuffer{ celestia::gl::Buffer::TargetHint::ElementArray };
        GLsizei lineCount{ 0 };
#endif
    };

    void ensureBuffers();
    ChunkMesh* getOrCreateChunk(const ChunkKey& key, unsigned int attributes);
    void drawChunk(ChunkMesh& chunk, unsigned int attributes);
    bool shouldSplit(int face, int depth, std::uint32_t i, std::uint32_t j,
                     const Eigen::Vector3f& eyePos) const;
    void renderNode(int face, int depth, std::uint32_t i, std::uint32_t j,
                    unsigned int attributes,
                    const celestia::math::Frustum& frustum,
                    const Eigen::Vector3f& eyePos);

    int vertexSize{ 0 };
    int nTexturesUsed{ 0 };
    float lodPixelSize{ 1.0f };

    bool buffersInitialized{ false };
    GLuint vao{ 0 };
    std::unordered_map<ChunkKey, ChunkMesh, ChunkKeyHash> chunkCache{};
};
