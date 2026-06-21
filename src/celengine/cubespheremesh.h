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

#include <celengine/glsupport.h>
#include <celrender/gl/buffer.h>

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

    // Drop-in for LODSphereMesh::render: same arguments. pixWidth is the
    // planet's apparent disc size in pixels and drives the global level.
    void render(unsigned int attributes,
                const celestia::math::Frustum& frustum,
                float pixWidth,
                Texture** tex,
                int nTextures,
                CelestiaGLProgram* program);

private:
    struct CachedIndexBuffer
    {
        celestia::gl::Buffer buffer;
        int indexCount{ 0 };
    };

    void ensureBuffers();
    void buildVertices(int n, unsigned int attributes);
    CachedIndexBuffer* getOrCreateIndexBuffer(int n);

    int vertexSize{ 0 };
    int nTexturesUsed{ 0 };
    std::vector<float> vertices{};

    bool buffersInitialized{ false };
    GLuint vao{ 0 };
    celestia::gl::Buffer vertexBuffer{};
    std::unordered_map<int, CachedIndexBuffer> indexBufferCache{};
};
