// cubespheremesh.h
//
// Copyright (C) 2001-present, Celestia Development Team
//
// Uniform cube-sphere planet mesh: six cube faces, each a regular
// (N+1)x(N+1) grid projected to the unit sphere. Even vertex distribution
// (no pole pinch, unlike the lat/long LODSphereMesh) and a single global
// subdivision level chosen from apparent size, so adjacent faces share edge
// vertices and the mesh is watertight by construction (no stitching).
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <array>
#include <cstddef>
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

    // Drop-in for LODSphereMesh::render plus the eye position in object space
    // (unit-sphere coordinates) for horizon culling. pixWidth is the planet's
    // apparent disc size in pixels and drives the global level.
    void render(unsigned int attributes,
                const celestia::math::Frustum& frustum,
                const Eigen::Vector3f& eyePos,
                float pixWidth,
                Texture** tex,
                int nTextures,
                CelestiaGLProgram* program);

private:
    // Per-patch culling data (a patch is one cell of the per-face p x p grid),
    // precomputed once per level. axis/cosHalfAngle bound the patch as a cone
    // about its centre direction for the horizon test; centre/radius is its
    // bounding sphere for the frustum test. All are unit-sphere geometry.
    struct PatchCull
    {
        Eigen::Vector3f axis;
        Eigen::Vector3f center;
        float cosHalfAngle;
        float radius;
    };

    struct CachedIndexBuffer
    {
        celestia::gl::Buffer buffer;
        // Patches are stored contiguously in the index buffer, ordered
        // (face, pi, pj), so any run of visible patches draws as one call.
        int trianglesPerPatch{ 0 };
        std::vector<PatchCull> patches;
#if CUBESPHERE_WIREFRAME
        celestia::gl::Buffer lineBuffer;
        int linesPerPatch{ 0 };
#endif
    };

    void ensureBuffers();
    void buildVertices(int n, unsigned int attributes);
    celestia::gl::Buffer* getOrCreateVertexBuffer(int n, unsigned int attributes);
    CachedIndexBuffer* getOrCreateIndexBuffer(int n);
    static bool patchCulled(const PatchCull& patch,
                            const celestia::math::Frustum& frustum,
                            const Eigen::Vector3f& eyePos);

    int vertexSize{ 0 };
    int nTexturesUsed{ 0 };
    std::vector<float> vertices{};

    bool buffersInitialized{ false };
    GLuint vao{ 0 };
    // Vertex data is camera-independent, so it is built once per (level,
    // vertex layout) and cached; the key packs both since a single shared mesh
    // serves planets with differing attribute sets (tangents/textures).
    std::unordered_map<long long, celestia::gl::Buffer> vertexBufferCache{};
    std::unordered_map<int, CachedIndexBuffer> indexBufferCache{};
};
