// cubespheremesh.cpp
//
// Copyright (C) 2001-present, Celestia Development Team
//
// See cubespheremesh.h. Uniform cube-sphere: one global subdivision level for
// all six faces, so the whole sphere is a single watertight indexed mesh.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "cubespheremesh.h"

#include <algorithm>
#include <cmath>

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

// Quads per face edge, from the planet's apparent disc size. The equator
// crosses 4 faces, so 4*n segments span it; snapping to powers of two keeps a
// handful of distinct index buffers in the cache. Range tuned to roughly match
// LODSphereMesh's density from a tiny disc to a full-screen close-up.
int
chooseLevel(float pixWidth)
{
    float target = pixWidth / 30.0f;
    int p = static_cast<int>(std::lround(std::log2(std::max(target, 1.0f))));
    p = std::clamp(p, 3, 8); // 2^3=8 .. 2^8=256 quads per face edge
    return 1 << p;
}

inline void*
bufferOffset(std::size_t n)
{
    return reinterpret_cast<void*>(n);
}

// Patches per face edge for culling granularity. Distant low-resolution
// planets get a single patch (the whole face); close, highly tessellated ones
// get up to 8x8 patches so that off-horizon and off-screen regions are skipped
// at finer grain. Always a power of two that divides n.
int
patchesPerEdge(int n)
{
    return std::clamp(n / 8, 1, 8);
}

} // anonymous namespace


// Horizon + frustum cull for one patch, in the per-axis normalized object space
// (where an ellipsoid maps exactly to this unit sphere). See the level builder
// for how the cone (axis, cosHalfAngle) and bounding sphere are derived.
bool
CubeSphereMesh::patchCulled(const PatchCull& patch,
                            const celestia::math::Frustum& frustum,
                            const Eigen::Vector3f& eyePos)
{
    float d = eyePos.norm();
    if (d > 1.0f)
    {
        float cosTheta = patch.axis.dot(eyePos) / d;
        // Largest dot(P, E) on the patch is d*cos(max(0, theta - a)); when the
        // eye direction lies inside the patch cone the reduced angle is 0.
        float cosReduced;
        if (cosTheta >= patch.cosHalfAngle)
        {
            cosReduced = 1.0f;
        }
        else
        {
            float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
            float sinHalf = std::sqrt(std::max(0.0f, 1.0f - patch.cosHalfAngle * patch.cosHalfAngle));
            cosReduced = cosTheta * patch.cosHalfAngle + sinTheta * sinHalf; // cos(theta - a)
        }
        if (d * cosReduced < 1.0f)
            return true;
    }

    return frustum.testSphere(patch.center, patch.radius)
           == celestia::math::FrustumAspect::Outside;
}


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


// Camera-independent vertex array for level n and the current vertex layout,
// built once and cached. The key packs the layout (vertexSize) alongside n
// because the shared mesh serves planets with different attribute sets.
gl::Buffer*
CubeSphereMesh::getOrCreateVertexBuffer(int n, unsigned int attributes)
{
    long long key = (static_cast<long long>(n) << 8) | static_cast<long long>(vertexSize);
    auto it = vertexBufferCache.find(key);
    if (it != vertexBufferCache.end())
        return &it->second;

    buildVertices(n, attributes);

    auto [pos, ok] = vertexBufferCache.try_emplace(key, gl::Buffer::TargetHint::Array);
    gl::Buffer& vbuf = pos->second;
    vbuf.bind();
    vbuf.setData(celestia::util::array_view<void>(vertices.data(),
                                                  vertices.size() * sizeof(float)),
                 gl::Buffer::BufferUsage::StaticDraw);
    return &vbuf;
}


// Build the interleaved vertex array for an n x n quad grid on every face.
// Layout per vertex: position(3) [+ tangent(3) if Tangents] [+ uv(2) if textured].
// The normal aliases the position (unit sphere) so it needs no storage.
void
CubeSphereMesh::buildVertices(int n, unsigned int attributes)
{
    const bool wantTangents = (attributes & Tangents) != 0;
    const bool wantTex = nTexturesUsed > 0;

    const float twoPi = 2.0f * static_cast<float>(celestia::numbers::pi);
    const float invTwoPi = 1.0f / twoPi;
    const float invPi = 1.0f / static_cast<float>(celestia::numbers::pi);

    vertices.clear();
    vertices.reserve(static_cast<std::size_t>(NUM_FACES) * (n + 1) * (n + 1) * vertexSize);

    for (int face = 0; face < NUM_FACES; ++face)
    {
        const FaceBasis& f = faceBases[face];

        // Unwrap the equirectangular longitude seam relative to the face centre
        // so texture coordinates stay continuous across each face.
        Vector3f cc = spherePoint(f, 0.5f, 0.5f);
        float centerLon = std::atan2(cc.z(), cc.x());

        for (int i = 0; i <= n; ++i)
        {
            float s = static_cast<float>(i) / n;
            for (int j = 0; j <= n; ++j)
            {
                float t = static_cast<float>(j) / n;
                Vector3f p = spherePoint(f, s, t);

                vertices.push_back(p.x());
                vertices.push_back(p.y());
                vertices.push_back(p.z());

                if (wantTangents)
                {
                    // East-pointing tangent (matches LODSphereMesh convention).
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
    }
}


// Triangle index buffer for an n x n grid on all six faces. Vertex (i,j) of a
// face is at face*(n+1)^2 + i*(n+1) + j. Independent of the camera, so it is
// built once per level and cached.
// Index buffers and per-patch cull data for level n. Triangles (and wireframe
// lines) are grouped patch-by-patch in (face, pi, pj) order so a run of visible
// patches can be drawn in a single call. Camera-independent, built once per n.
CubeSphereMesh::CachedIndexBuffer*
CubeSphereMesh::getOrCreateIndexBuffer(int n)
{
    auto it = indexBufferCache.find(n);
    if (it != indexBufferCache.end())
        return &it->second;

    int p = patchesPerEdge(n);
    int q = n / p; // quads per patch edge
    int stride = n + 1;

    auto [pos, ok] = indexBufferCache.try_emplace(n);
    CachedIndexBuffer& cib = pos->second;
    cib.trianglesPerPatch = q * q * 6;
    cib.patches.reserve(static_cast<std::size_t>(NUM_FACES) * p * p);

    std::vector<unsigned int> indices;
    indices.reserve(static_cast<std::size_t>(NUM_FACES) * n * n * 6);
#if CUBESPHERE_WIREFRAME
    cib.linesPerPatch = 4 * q * (q + 1);
    std::vector<unsigned int> lineIndices;
#endif

    for (int face = 0; face < NUM_FACES; ++face)
    {
        const FaceBasis& f = faceBases[face];
        auto base = static_cast<unsigned int>(face * stride * stride);
        for (int pi = 0; pi < p; ++pi)
        {
            for (int pj = 0; pj < p; ++pj)
            {
                int i0 = pi * q;
                int j0 = pj * q;

                for (int i = i0; i < i0 + q; ++i)
                {
                    for (int j = j0; j < j0 + q; ++j)
                    {
                        unsigned int v00 = base + static_cast<unsigned int>(i * stride + j);
                        unsigned int v10 = v00 + static_cast<unsigned int>(stride);
                        unsigned int v01 = v00 + 1u;
                        unsigned int v11 = v10 + 1u;
                        indices.push_back(v00); indices.push_back(v10); indices.push_back(v11);
                        indices.push_back(v00); indices.push_back(v11); indices.push_back(v01);
                    }
                }

#if CUBESPHERE_WIREFRAME
                // Each patch draws its own (q+1)x(q+1) grid edges; borders
                // shared with neighbours are drawn twice, which is harmless for
                // the debug wireframe and keeps patches independently drawable.
                for (int i = i0; i <= i0 + q; ++i)
                {
                    for (int j = j0; j <= j0 + q; ++j)
                    {
                        unsigned int v = base + static_cast<unsigned int>(i * stride + j);
                        if (j < j0 + q)
                        {
                            lineIndices.push_back(v);
                            lineIndices.push_back(v + 1u);
                        }
                        if (i < i0 + q)
                        {
                            lineIndices.push_back(v);
                            lineIndices.push_back(v + static_cast<unsigned int>(stride));
                        }
                    }
                }
#endif

                // Cull data: cone about the patch centre direction, plus a 3D
                // bounding sphere over the patch corners.
                float s0 = static_cast<float>(pi) / p;
                float s1 = static_cast<float>(pi + 1) / p;
                float t0 = static_cast<float>(pj) / p;
                float t1 = static_cast<float>(pj + 1) / p;
                Vector3f corners[4] = {
                    spherePoint(f, s0, t0), spherePoint(f, s1, t0),
                    spherePoint(f, s1, t1), spherePoint(f, s0, t1),
                };
                PatchCull pc;
                pc.axis = spherePoint(f, 0.5f * (s0 + s1), 0.5f * (t0 + t1));
                pc.center = (corners[0] + corners[1] + corners[2] + corners[3]) * 0.25f;
                float cosHalf = 1.0f;
                float radius2 = 0.0f;
                for (const Vector3f& c : corners)
                {
                    cosHalf = std::min(cosHalf, pc.axis.dot(c));
                    radius2 = std::max(radius2, (pc.center - c).squaredNorm());
                }
                pc.cosHalfAngle = cosHalf;
                pc.radius = std::sqrt(radius2);
                cib.patches.push_back(pc);
            }
        }
    }

    cib.buffer = gl::Buffer(gl::Buffer::TargetHint::ElementArray);
    cib.buffer.bind();
    cib.buffer.setData(celestia::util::array_view<void>(indices.data(),
                                                        indices.size() * sizeof(unsigned int)),
                       gl::Buffer::BufferUsage::StaticDraw);

#if CUBESPHERE_WIREFRAME
    cib.lineBuffer = gl::Buffer(gl::Buffer::TargetHint::ElementArray);
    cib.lineBuffer.bind();
    cib.lineBuffer.setData(celestia::util::array_view<void>(lineIndices.data(),
                                                            lineIndices.size() * sizeof(unsigned int)),
                           gl::Buffer::BufferUsage::StaticDraw);
#endif

    return &cib;
}


void
CubeSphereMesh::render(unsigned int attributes,
                       const celestia::math::Frustum& frustum,
                       const Eigen::Vector3f& eyePos,
                       float pixWidth,
                       Texture** tex,
                       int nTextures,
                       CelestiaGLProgram* program)
{
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

    int n = chooseLevel(pixWidth);
    gl::Buffer* vbuf = getOrCreateVertexBuffer(n, attributes);
    CachedIndexBuffer* cib = getOrCreateIndexBuffer(n);

    glBindVertexArray(vao);

    vbuf->bind();

    const auto stride = static_cast<GLsizei>(vertexSize * sizeof(float));
    const int texCoordOffset = ((attributes & Tangents) != 0) ? 6 : 3;

    glEnableVertexAttribArray(CelestiaGLProgram::VertexCoordAttributeIndex);
    glVertexAttribPointer(CelestiaGLProgram::VertexCoordAttributeIndex,
                          3, GL_FLOAT, GL_FALSE, stride, bufferOffset(0));
    if ((attributes & Normals) != 0)
    {
        glEnableVertexAttribArray(CelestiaGLProgram::NormalAttributeIndex);
        // Unit-sphere normal aliases position.
        glVertexAttribPointer(CelestiaGLProgram::NormalAttributeIndex,
                              3, GL_FLOAT, GL_FALSE, stride, bufferOffset(0));
    }
    if ((attributes & Tangents) != 0)
    {
        glEnableVertexAttribArray(CelestiaGLProgram::TangentAttributeIndex);
        glVertexAttribPointer(CelestiaGLProgram::TangentAttributeIndex,
                              3, GL_FLOAT, GL_FALSE, stride, bufferOffset(3 * sizeof(float)));
    }
    for (int tc = 0; tc < nTexturesUsed; ++tc)
    {
        glEnableVertexAttribArray(CelestiaGLProgram::TextureCoord0AttributeIndex + tc);
        glVertexAttribPointer(CelestiaGLProgram::TextureCoord0AttributeIndex + tc,
                              2, GL_FLOAT, GL_FALSE, stride,
                              bufferOffset(texCoordOffset * sizeof(float)));
    }

    // Patches are stored contiguously (face, pi, pj). Each visible patch is
    // accumulated into a run of adjacent patches and flushed as a single draw
    // call when a culled patch (or the end) breaks the run.
    int patchCount = static_cast<int>(cib->patches.size());
#if CUBESPHERE_WIREFRAME
    cib->lineBuffer.bind();
    int perPatch = cib->linesPerPatch;
    GLenum primitive = GL_LINES;
#else
    cib->buffer.bind();
    int perPatch = cib->trianglesPerPatch;
    GLenum primitive = GL_TRIANGLES;
#endif

    int runStart = -1;
    for (int l = 0; l <= patchCount; ++l)
    {
        bool visible = l < patchCount && !patchCulled(cib->patches[l], frustum, eyePos);
        if (visible)
        {
            if (runStart < 0)
                runStart = l;
        }
        else if (runStart >= 0)
        {
            int count = (l - runStart) * perPatch;
            glDrawElements(primitive, count, GL_UNSIGNED_INT,
                           bufferOffset(static_cast<std::size_t>(runStart) * perPatch * sizeof(unsigned int)));
            runStart = -1;
        }
    }

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
