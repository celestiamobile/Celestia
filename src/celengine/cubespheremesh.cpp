// cubespheremesh.cpp
//
// Copyright (C) 2001-present, Celestia Development Team
//
// See cubespheremesh.h. Chunked-LOD cube-sphere: six quadtrees (one per cube
// face) refined toward the camera, each node a CHUNK_RES grid generated on
// demand and cached. This stage refines each face by screen-space error (no
// culling yet); leaves of differing depth do not share edge vertices, so cracks
// appear at LOD boundaries and face seams until edge stitching is added.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "cubespheremesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <Eigen/Core>

#include <celcompat/numbers.h>
#include <celengine/shadermanager.h>
#include <celengine/texture.h>
#include <celmath/frustum.h>
#include <celutil/array_view.h>

using namespace Eigen;
namespace gl = celestia::gl;

namespace
{

// Cube face basis: pos = origin + s*du + t*dv, s,t in [0,1]; du x dv points
// outward so all six faces share the same outward winding.
struct FaceBasis
{
    Vector3f origin;
    Vector3f du;
    Vector3f dv;
};

constexpr int NUM_FACES = 6;

// Grid resolution (quads per edge) of a single chunk. Smaller chunks mean a
// deeper quadtree but more, lighter draw calls.
constexpr int CHUNK_RES = 16;

// Maximum quadtree depth, a safety cap on refinement near the surface. At depth
// d a face edge is split into 2^d cells, so this bounds how small a chunk can get.
constexpr int MAX_DEPTH = 20;

// Maximum tolerated screen-space geometric error, in pixels. A node is split
// while the projection of its mesh-to-sphere deviation (the per-quad sagitta)
// exceeds this, which is the canonical chunked-LOD criterion: it bounds the
// visible error directly rather than the chunk's on-screen size, so it neither
// over-refines foreshortened chunks nor needs a view-angle fudge factor.
constexpr float CHUNK_PIXEL_ERROR = 2.0f;

const std::array<FaceBasis, NUM_FACES> faceBases = { {
    // +X
    { Vector3f( 1.f, -1.f, -1.f), Vector3f(0.f, 2.f, 0.f), Vector3f(0.f, 0.f, 2.f) },
    // -X
    { Vector3f(-1.f, -1.f, -1.f), Vector3f(0.f, 0.f, 2.f), Vector3f(0.f, 2.f, 0.f) },
    // +Y
    { Vector3f(-1.f,  1.f, -1.f), Vector3f(0.f, 0.f, 2.f), Vector3f(2.f, 0.f, 0.f) },
    // -Y
    { Vector3f(-1.f, -1.f, -1.f), Vector3f(2.f, 0.f, 0.f), Vector3f(0.f, 0.f, 2.f) },
    // +Z
    { Vector3f(-1.f, -1.f,  1.f), Vector3f(2.f, 0.f, 0.f), Vector3f(0.f, 2.f, 0.f) },
    // -Z
    { Vector3f(-1.f, -1.f, -1.f), Vector3f(0.f, 2.f, 0.f), Vector3f(2.f, 0.f, 0.f) },
} };

inline Vector3f
spherePoint(const FaceBasis& f, float s, float t)
{
    return (f.origin + s * f.du + t * f.dv).normalized();
}

inline void*
bufferOffset(std::size_t n)
{
    return reinterpret_cast<void*>(n);
}

} // anonymous namespace


CubeSphereMesh::~CubeSphereMesh()
{
    if (vao != 0)
        glDeleteVertexArrays(1, &vao);
}


void
CubeSphereMesh::ensureBuffers()
{
    if (buffersInitialized)
        return;

    while (glGetError() != GL_NO_ERROR) { /* drain */ }
    glGenVertexArrays(1, &vao);
    if (glGetError() != GL_NO_ERROR)
        return;

    buffersInitialized = true;
}


// Generate (or fetch from cache) the GPU mesh for one quadtree node. The node
// covers the face sub-region [s0,s1]x[t0,t1] determined by (depth,i,j); it is a
// plain CHUNK_RES grid on the unit sphere. The shader derives the normal and
// texture coordinates from the position, so only positions (and an optional
// tangent) are stored.
CubeSphereMesh::ChunkMesh*
CubeSphereMesh::getOrCreateChunk(const ChunkKey& key, unsigned int attributes)
{
    auto it = chunkCache.find(key);
    if (it != chunkCache.end())
        return &it->second;

    const bool wantTangents = (attributes & Tangents) != 0;
    const bool wantTex = nTexturesUsed > 0;

    const FaceBasis& f = faceBases[key.face];

    float cells = static_cast<float>(1u << key.depth);
    float s0 = static_cast<float>(key.i) / cells;
    float s1 = static_cast<float>(key.i + 1) / cells;
    float t0 = static_cast<float>(key.j) / cells;
    float t1 = static_cast<float>(key.j + 1) / cells;
    float ds = s1 - s0;
    float dt = t1 - t0;

    const float twoPi = 2.0f * static_cast<float>(celestia::numbers::pi);
    const float invTwoPi = 1.0f / twoPi;
    const float invPi = 1.0f / static_cast<float>(celestia::numbers::pi);

    // Unwrap the equirectangular longitude seam relative to the face centre so
    // texture coordinates stay continuous across the face.
    Vector3f fc = spherePoint(f, 0.5f, 0.5f);
    float centerLon = std::atan2(fc.z(), fc.x());

    std::vector<float> vertices;
    vertices.reserve(static_cast<std::size_t>(CHUNK_RES + 1) * (CHUNK_RES + 1) * key.vertexSize);

    for (int ii = 0; ii <= CHUNK_RES; ++ii)
    {
        float s = s0 + ds * (static_cast<float>(ii) / CHUNK_RES);
        for (int jj = 0; jj <= CHUNK_RES; ++jj)
        {
            float t = t0 + dt * (static_cast<float>(jj) / CHUNK_RES);
            Vector3f p = spherePoint(f, s, t);

            vertices.push_back(p.x());
            vertices.push_back(p.y());
            vertices.push_back(p.z());

            if (wantTangents)
            {
                Vector3f tang(p.z(), 0.0f, -p.x());
                float len = tang.norm();
                if (len > 1.0e-6f)
                    tang /= len;
                else
                    tang = Vector3f(1.0f, 0.0f, 0.0f);
                vertices.push_back(tang.x());
                vertices.push_back(tang.y());
                vertices.push_back(tang.z());
            }

            if (wantTex)
            {
                float lon = std::atan2(p.z(), p.x());
                while (lon - centerLon > celestia::numbers::pi) lon -= twoPi;
                while (centerLon - lon > celestia::numbers::pi) lon += twoPi;
                float u = 1.0f - lon * invTwoPi;
                float v = std::acos(std::clamp(p.y(), -1.0f, 1.0f)) * invPi;
                vertices.push_back(u);
                vertices.push_back(v);
            }
        }
    }

    auto stride = static_cast<unsigned int>(CHUNK_RES + 1);
    auto idx = [stride](int ii, int jj) {
        return static_cast<unsigned int>(ii * stride + jj);
    };

    std::vector<unsigned int> indices;
    indices.reserve(static_cast<std::size_t>(CHUNK_RES) * CHUNK_RES * 6);

    for (int ii = 0; ii < CHUNK_RES; ++ii)
    {
        for (int jj = 0; jj < CHUNK_RES; ++jj)
        {
            unsigned int v00 = idx(ii, jj);
            unsigned int v10 = idx(ii + 1, jj);
            unsigned int v01 = idx(ii, jj + 1);
            unsigned int v11 = idx(ii + 1, jj + 1);
            indices.push_back(v00); indices.push_back(v10); indices.push_back(v11);
            indices.push_back(v00); indices.push_back(v11); indices.push_back(v01);
        }
    }

#if CUBESPHERE_WIREFRAME
    std::vector<unsigned int> lineIndices;
    for (int ii = 0; ii <= CHUNK_RES; ++ii)
    {
        for (int jj = 0; jj <= CHUNK_RES; ++jj)
        {
            unsigned int v = idx(ii, jj);
            if (jj < CHUNK_RES) { lineIndices.push_back(v); lineIndices.push_back(idx(ii, jj + 1)); }
            if (ii < CHUNK_RES) { lineIndices.push_back(v); lineIndices.push_back(idx(ii + 1, jj)); }
        }
    }
#endif

    auto [pos, ok] = chunkCache.try_emplace(key);
    ChunkMesh& chunk = pos->second;

    chunk.vbuf.bind();
    chunk.vbuf.setData(celestia::util::array_view<void>(vertices.data(),
                                                        vertices.size() * sizeof(float)),
                       gl::Buffer::BufferUsage::StaticDraw);

    chunk.ibuf.bind();
    chunk.ibuf.setData(celestia::util::array_view<void>(indices.data(),
                                                        indices.size() * sizeof(unsigned int)),
                       gl::Buffer::BufferUsage::StaticDraw);
    chunk.indexCount = static_cast<GLsizei>(indices.size());

#if CUBESPHERE_WIREFRAME
    chunk.lineBuffer.bind();
    chunk.lineBuffer.setData(celestia::util::array_view<void>(lineIndices.data(),
                                                              lineIndices.size() * sizeof(unsigned int)),
                             gl::Buffer::BufferUsage::StaticDraw);
    chunk.lineCount = static_cast<GLsizei>(lineIndices.size());
#endif

    // Cull bounds: cone about the chunk centre direction plus a bounding sphere
    // over the four corners.
    Vector3f corners[4] = {
        spherePoint(f, s0, t0), spherePoint(f, s1, t0),
        spherePoint(f, s1, t1), spherePoint(f, s0, t1),
    };
    chunk.axis = spherePoint(f, 0.5f * (s0 + s1), 0.5f * (t0 + t1));
    chunk.center = (corners[0] + corners[1] + corners[2] + corners[3]) * 0.25f;
    float cosHalf = 1.0f;
    float radius2 = 0.0f;
    for (const Vector3f& c : corners)
    {
        cosHalf = std::min(cosHalf, chunk.axis.dot(c));
        radius2 = std::max(radius2, (chunk.center - c).squaredNorm());
    }
    chunk.cosHalfAngle = cosHalf;
    chunk.radius = std::sqrt(radius2);

    return &chunk;
}


// Bind one chunk's buffers, point the vertex attributes at them (each chunk owns
// its own VBO, so the pointers must be set per draw) and issue the draw. The
// attribute arrays are enabled once by the caller.
void
CubeSphereMesh::drawChunk(ChunkMesh& chunk, unsigned int attributes)
{
    chunk.vbuf.bind();

    const auto stride = static_cast<GLsizei>(vertexSize * sizeof(float));
    const int texCoordOffset = ((attributes & Tangents) != 0) ? 6 : 3;

    glVertexAttribPointer(CelestiaGLProgram::VertexCoordAttributeIndex,
                          3, GL_FLOAT, GL_FALSE, stride, bufferOffset(0));
    if ((attributes & Normals) != 0)
        glVertexAttribPointer(CelestiaGLProgram::NormalAttributeIndex,
                              3, GL_FLOAT, GL_FALSE, stride, bufferOffset(0));
    if ((attributes & Tangents) != 0)
        glVertexAttribPointer(CelestiaGLProgram::TangentAttributeIndex,
                              3, GL_FLOAT, GL_FALSE, stride, bufferOffset(3 * sizeof(float)));
    for (int tc = 0; tc < nTexturesUsed; ++tc)
        glVertexAttribPointer(CelestiaGLProgram::TextureCoord0AttributeIndex + tc,
                              2, GL_FLOAT, GL_FALSE, stride,
                              bufferOffset(texCoordOffset * sizeof(float)));

#if CUBESPHERE_WIREFRAME
    chunk.lineBuffer.bind();
    glDrawElements(GL_LINES, chunk.lineCount, GL_UNSIGNED_INT, bufferOffset(0));
#else
    chunk.ibuf.bind();
    glDrawElements(GL_TRIANGLES, chunk.indexCount, GL_UNSIGNED_INT, bufferOffset(0));
#endif
}


// Decide whether a node should be refined into its four children. Uses a
// screen-space geometric error metric: the chunk's mesh deviates from the true
// sphere by at most the sagitta of one of its quads; projecting that deviation
// to the screen and comparing against CHUNK_PIXEL_ERROR bounds the visible error
// directly. Working in normalized object space (planet = unit sphere) the error
// and the distance are both in radius units, so the planet radius cancels and the
// test is identical from orbit to ground. Distance is taken to the nearest point
// of the chunk (not its centre) so large chunks whose near edge is close are not
// under-refined.
bool
CubeSphereMesh::shouldSplit(int face, int depth, std::uint32_t i, std::uint32_t j,
                            const Eigen::Vector3f& eyePos) const
{
    if (depth >= MAX_DEPTH)
        return false;

    const FaceBasis& f = faceBases[face];
    float cells = static_cast<float>(1u << depth);
    float s0 = static_cast<float>(i) / cells;
    float s1 = static_cast<float>(i + 1) / cells;
    float t0 = static_cast<float>(j) / cells;
    float t1 = static_cast<float>(j + 1) / cells;

    Vector3f c00 = spherePoint(f, s0, t0);
    Vector3f c10 = spherePoint(f, s1, t0);
    Vector3f c11 = spherePoint(f, s1, t1);
    Vector3f c01 = spherePoint(f, s0, t1);

    Vector3f center = (c00 + c10 + c11 + c01) * 0.25f;
    float chunkRadius = std::sqrt(std::max(std::max((c00 - center).squaredNorm(),
                                                    (c10 - center).squaredNorm()),
                                           std::max((c11 - center).squaredNorm(),
                                                    (c01 - center).squaredNorm())));

    float edge = std::max(std::max((c10 - c00).norm(), (c01 - c00).norm()),
                          std::max((c11 - c10).norm(), (c11 - c01).norm()));

    // Per-quad sagitta: chord c subtends arc ~c on the unit sphere, deviating
    // from it by ~c^2/8. This is the residual error of the chunk's fixed grid.
    float quadChord = edge / static_cast<float>(CHUNK_RES);
    float geoError = quadChord * quadChord * 0.125f;

    float dist = std::max((eyePos - center).norm() - chunkRadius, 1.0e-6f);
    float screenError = geoError / (dist * lodPixelSize);
    return screenError > CHUNK_PIXEL_ERROR;
}


// Descend the quadtree for one face, refining by screen-space error. Cracks
// appear where neighbouring leaves differ in depth (and across face seams); they
// are removed by edge stitching in a later stage. Culling is also added later.
void
CubeSphereMesh::renderNode(int face, int depth, std::uint32_t i, std::uint32_t j,
                           unsigned int attributes,
                           const celestia::math::Frustum& frustum,
                           const Eigen::Vector3f& eyePos)
{
    (void)frustum;

    if (shouldSplit(face, depth, i, j, eyePos))
    {
        for (std::uint32_t c = 0; c < 4; ++c)
            renderNode(face, depth + 1, i * 2 + (c & 1u), j * 2 + ((c >> 1) & 1u),
                       attributes, frustum, eyePos);
        return;
    }

    ChunkKey key{ face, depth, i, j, vertexSize };
    ChunkMesh* chunk = getOrCreateChunk(key, attributes);
    if (chunk != nullptr)
        drawChunk(*chunk, attributes);
}


void
CubeSphereMesh::render(unsigned int attributes,
                       const celestia::math::Frustum& frustum,
                       const Eigen::Vector3f& eyePos,
                       float pixWidth,
                       float pixelSize,
                       Texture** tex,
                       int nTextures,
                       CelestiaGLProgram* program)
{
    (void)pixWidth;

    lodPixelSize = pixelSize;

    if (tex == nullptr)
        nTextures = 0;
    nTexturesUsed = std::min(nTextures, static_cast<int>(MAX_TEXTURES));

    vertexSize = 3;
    if ((attributes & Tangents) != 0)
        vertexSize += 3;
    if (nTexturesUsed > 0)
        vertexSize += 2;

    ensureBuffers();
    if (!buffersInitialized)
        return;

    for (int i = 0; i < nTexturesUsed; ++i)
    {
        tex[i]->beginUsage();
        if (nTexturesUsed > 1)
            glActiveTexture(GL_TEXTURE0 + i);
        TextureTile tile = tex[i]->getTile(0, 0, 0);
        glBindTexture(GL_TEXTURE_2D, tile.texID);
        program->texCoordTransforms[i].base = Vector2f(tile.u, tile.v);
        program->texCoordTransforms[i].delta = Vector2f(tile.du, tile.dv);
    }
    if (nTexturesUsed > 1)
        glActiveTexture(GL_TEXTURE0);

    glBindVertexArray(vao);

    glEnableVertexAttribArray(CelestiaGLProgram::VertexCoordAttributeIndex);
    if ((attributes & Normals) != 0)
        glEnableVertexAttribArray(CelestiaGLProgram::NormalAttributeIndex);
    if ((attributes & Tangents) != 0)
        glEnableVertexAttribArray(CelestiaGLProgram::TangentAttributeIndex);
    for (int tc = 0; tc < nTexturesUsed; ++tc)
        glEnableVertexAttribArray(CelestiaGLProgram::TextureCoord0AttributeIndex + tc);

    for (int face = 0; face < NUM_FACES; ++face)
        renderNode(face, 0, 0, 0, attributes, frustum, eyePos);

    glDisableVertexAttribArray(CelestiaGLProgram::VertexCoordAttributeIndex);
    if ((attributes & Normals) != 0)
        glDisableVertexAttribArray(CelestiaGLProgram::NormalAttributeIndex);
    if ((attributes & Tangents) != 0)
        glDisableVertexAttribArray(CelestiaGLProgram::TangentAttributeIndex);
    for (int tc = 0; tc < nTexturesUsed; ++tc)
        glDisableVertexAttribArray(CelestiaGLProgram::TextureCoord0AttributeIndex + tc);

    for (int i = 0; i < nTexturesUsed; ++i)
        tex[i]->endUsage();

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}
