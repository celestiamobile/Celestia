// terrainlodmanager.cpp
//
// Copyright (C) 2026-present, Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "terrainlodmanager.h"
#include "terraindata.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <celmath/frustum.h>
#include <celmath/mathlib.h>
#include <celcompat/numbers.h>
#include "glsupport.h"

namespace math = celestia::math;

TerrainLODManager::TerrainLODManager() = default;

TerrainLODManager::~TerrainLODManager()
{
    clearCache();
}

void TerrainLODManager::clearCache()
{
    for (auto& [id, patch] : patchCache)
    {
        if (patch->vao != 0) glDeleteVertexArrays(1, &patch->vao);
        if (patch->vbo != 0) glDeleteBuffers(1, &patch->vbo);
        if (patch->nbo != 0) glDeleteBuffers(1, &patch->nbo);
        if (patch->tanbo != 0) glDeleteBuffers(1, &patch->tanbo);
        if (patch->tbo != 0) glDeleteBuffers(1, &patch->tbo);
        if (patch->ebo != 0) glDeleteBuffers(1, &patch->ebo);
    }
    patchCache.clear();
    visiblePatches.clear();
}

void TerrainLODManager::selectPatches(const Eigen::Vector3f& cameraPos,
                                       const math::Frustum& frustum,
                                       float sphereRadius,
                                       float cameraDistance)
{
    (void) cameraDistance;
    currentSphereRadius = sphereRadius;
    visiblePatches.clear();
    totalPatchesEvaluated = 0;
    totalPatchesRendered = 0;
    
    // Create 8 root patches covering the full sphere with a lat/lon grid:
    //   phi (longitude) in 4 quadrants of 90 deg
    //   theta (polar angle from north pole) in 2 hemispheres of 90 deg
    // This fully tiles the sphere (theta=[0,180], phi=[0,360]).
    std::array<TerrainPatch, 8> rootPatches = {
        TerrainPatch{  0.0f,  0.0f, 90.0f, 0}, // N quadrant 1
        TerrainPatch{ 90.0f,  0.0f, 90.0f, 0}, // N quadrant 2
        TerrainPatch{180.0f,  0.0f, 90.0f, 0}, // N quadrant 3
        TerrainPatch{270.0f,  0.0f, 90.0f, 0}, // N quadrant 4
        TerrainPatch{  0.0f, 90.0f, 90.0f, 0}, // S quadrant 1
        TerrainPatch{ 90.0f, 90.0f, 90.0f, 0}, // S quadrant 2
        TerrainPatch{180.0f, 90.0f, 90.0f, 0}, // S quadrant 3
        TerrainPatch{270.0f, 90.0f, 90.0f, 0}, // S quadrant 4
    };
    
    // Recursively select patches
    for (auto& rootPatch : rootPatches)
    {
        computePatchGeometry(rootPatch, sphereRadius);
        selectPatchesRecursive(rootPatch, cameraPos, frustum, sphereRadius);
    }
    
    // Sort by distance (closest first)
    std::sort(visiblePatches.begin(), visiblePatches.end(),
              [&cameraPos](const TerrainPatch& a, const TerrainPatch& b)
              {
                  return (a.center - cameraPos).squaredNorm() <
                         (b.center - cameraPos).squaredNorm();
              });
    
    // Limit to max visible patches
    if (visiblePatches.size() > static_cast<size_t>(maxVisiblePatches))
    {
        visiblePatches.resize(maxVisiblePatches);
    }
}

void TerrainLODManager::selectPatchesRecursive(TerrainPatch& patch,
                                                const Eigen::Vector3f& cameraPos,
                                                const math::Frustum& frustum,
                                                float sphereRadius)
{
    totalPatchesEvaluated++;
    
    // Horizon (back-of-planet) culling: if the patch's outward-facing
    // surface normal points away from the camera by more than the geometric
    // horizon angle, the patch is fully behind the planet's limb and
    // invisible. This eliminates the entire far hemisphere of patches
    // before we ever recurse into them. Particularly important at low
    // altitudes where >50% of the sphere is hidden behind the planet
    // itself.
    //
    // Math: let r = sphereRadius, d = |cameraPos|. Camera is at altitude
    // d - r above surface. Horizon angle from camera-to-center axis is
    // acos(r / d). A patch with normal n_p (= center/|center|) is visible
    // if angle(camera_dir, n_p) < horizon_angle + patch_angular_radius.
    // Convert to dot product to avoid trig: visible iff
    //   dot(P_hat, n_p) > cos(horizon + patchHalfAngle).
    // patchHalfAngle ~= asin(patch.radius / r) for a patch tangent to the
    // sphere.
    {
        float cameraDist = cameraPos.norm();
        if (cameraDist > sphereRadius * 1.0001f)  // above surface
        {
            Eigen::Vector3f cameraDir = cameraPos / cameraDist;
            Eigen::Vector3f patchNormal = patch.center.normalized();
            float dotCN = cameraDir.dot(patchNormal);
            // cos(horizon) = r/d
            float cosHorizon = sphereRadius / cameraDist;
            // Inflate by patch angular radius (small-angle: r_p/R).
            float patchAngularR = patch.radius / sphereRadius;
            // cos(horizon + patchAngularR) ~ cosHorizon - sin(horizon)*patchAngularR
            // For coarse culling we just subtract a generous slack equal to
            // patchAngularR; this errs on the side of keeping borderline
            // patches.
            float threshold = cosHorizon - patchAngularR - 0.01f;
            if (dotCN < threshold)
                return;
        }
    }

    // Atmospheric extinction cull: at low altitudes, patches well beyond the
    // visible haze distance contribute essentially nothing but still pay the
    // expensive per-pixel scattering shader cost. We cull aggressively at low
    // altitude but the cutoff MUST grow to at least the geometric horizon
    // distance — otherwise terrain visible inside the horizon (the whole
    // bottom half of the view at 500 km altitude!) gets dropped.
    {
        float distFromCam = (patch.center - cameraPos).norm();
        float altitude = std::max(0.0f, cameraPos.norm() - sphereRadius);
        // Geometric horizon distance: tangent line from camera to sphere.
        // sqrt(d^2 - r^2) where d = r + alt, simplified.
        float horizonDist = std::sqrt(2.0f * altitude * sphereRadius
                                      + altitude * altitude);
        // Haze cap at sea level (~0.025 R ≈ 160 km on Earth), grows linearly
        // with altitude.
        float hazeDist = sphereRadius * 0.025f + 2.0f * altitude;
        // Use whichever is LARGER: we never want to cull inside the
        // geometric horizon (everything there is potentially visible terrain)
        // but we also keep the haze cap as a floor for sub-horizon views.
        float maxDist = std::max(horizonDist, hazeDist);
        if (altitude < sphereRadius * 0.3f && distFromCam > maxDist + patch.radius)
            return;
    }

    // Frustum culling: skip if outside view.
    // We test against an INFLATED sphere covering the patch's actual
    // displaced geometry: skirt depth (sphereRadius * 2e-3, generated by
    // generatePatchVertices) plus plausible terrain relief from the
    // heightmap (Earth Everest ~0.0014 R; Mars Olympus Mons ~0.0065 R_mars).
    // Without this inflation, near-camera patches whose true geometry
    // extends past the chord-based bounding sphere are falsely culled when
    // grazing the screen edge — visible as black voids on the lower half of
    // horizon-grazing views. We deliberately use the un-inflated radius
    // everywhere else (especially shouldRefine), since inflating the stored
    // radius would also inflate angular size and trigger explosive
    // subdivision of small distant patches.
    const float terrainReliefBudget = sphereRadius * 0.01f;
    if (frustum.testSphere(patch.center, patch.radius + terrainReliefBudget) == math::FrustumAspect::Outside)
        return;
    
    // Check refinement criteria
    if (shouldRefine(patch, cameraPos, sphereRadius))
    {
        // Too coarse: subdivide into 4 children
        if (patch.level < maxPatchLevel)
        {
            auto children = subdividePatch(patch);
            for (auto& child : children)
            {
                computePatchGeometry(child, sphereRadius);
                selectPatchesRecursive(child, cameraPos, frustum, sphereRadius);
            }
        }
        else
        {
            // Hit max LOD level: render this patch anyway
            totalPatchesRendered++;
            visiblePatches.push_back(patch);
        }
    }
    else
    {
        // Coarse enough: render this patch
        totalPatchesRendered++;
        visiblePatches.push_back(patch);
    }
}

bool TerrainLODManager::shouldRefine(const TerrainPatch& patch,
                                      const Eigen::Vector3f& cameraPos,
                                      float sphereRadius) const
{
    // Compute distance from camera to patch center
    float distance = (patch.center - cameraPos).norm();

    // Compute angular size of patch on screen
    float angularSize = std::atan2(patch.radius, distance);

    // Near-surface views need a tighter threshold; otherwise the quadtree
    // stops one level too early and large square patches become visible.
    // Blend from ~3.5 deg at the surface to the configured threshold by
    // ~0.05R altitude (~320 km on Earth).
    float altitudeR = std::max(0.0f, cameraPos.norm() - sphereRadius) / sphereRadius;
    float nearSurfaceThreshold = 0.06f;
    float t = std::clamp(altitudeR / 0.05f, 0.0f, 1.0f);
    float adaptiveThreshold = nearSurfaceThreshold + (refinementThreshold - nearSurfaceThreshold) * t;

    // Refine if angular size exceeds threshold
    return angularSize > adaptiveThreshold;
}

std::array<TerrainPatch, 4> TerrainLODManager::subdividePatch(const TerrainPatch& patch) const
{
    std::array<TerrainPatch, 4> children;
    
    float halfExtent = patch.extent / 2.0f;
    int childLevel = patch.level + 1;
    
    // Create 4 child patches (2x2 subdivision)
    for (int i = 0; i < 2; i++)
    {
        for (int j = 0; j < 2; j++)
        {
            int idx = i * 2 + j;
            children[idx].phi0 = patch.phi0 + halfExtent * j;
            children[idx].theta0 = patch.theta0 + halfExtent * i;
            children[idx].extent = halfExtent;
            children[idx].level = childLevel;
        }
    }
    
    return children;
}

void TerrainLODManager::computePatchGeometry(TerrainPatch& patch, float sphereRadius)
{
    // Convert degrees to radians
    const float degToRad = celestia::numbers::pi / 180.0f;
    float phi0Rad = patch.phi0 * degToRad;
    float theta0Rad = patch.theta0 * degToRad;
    float extentRad = patch.extent * degToRad;
    
    // Compute 4 corners of patch
    for (int i = 0; i < 2; i++)
    {
        for (int j = 0; j < 2; j++)
        {
            float phi = phi0Rad + extentRad * j;
            float theta = theta0Rad + extentRad * i;
            
            // Convert spherical to Cartesian coordinates
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);
            float sinPhi = std::sin(phi);
            float cosPhi = std::cos(phi);
            
            int cornerIdx = i * 2 + j;
            patch.corners[cornerIdx] = sphereRadius * Eigen::Vector3f(
                sinTheta * cosPhi,
                cosTheta,
                sinTheta * sinPhi
            );
        }
    }
    
    // Bounding sphere center: project the centroid of the 4 chord corners
    // back out onto the sphere surface. The chord centroid lies INSIDE the
    // sphere (the chord is shorter than the arc), so a bounding sphere
    // centered there does not enclose the actual curved patch surface near
    // its middle, and frustum culling falsely rejects patches near the
    // limb. Re-projecting to the surface gives a center that's much closer
    // to the geometry, and we inflate the radius to cover both the corner
    // chord error and the surface bulge.
    Eigen::Vector3f chordCentroid = (patch.corners[0] + patch.corners[1] +
                                     patch.corners[2] + patch.corners[3]) * 0.25f;
    float chordLen = chordCentroid.norm();
    if (chordLen > 1.0e-6f)
        patch.center = chordCentroid * (sphereRadius / chordLen);
    else
        patch.center = chordCentroid;

    patch.radius = 0.0f;
    for (const auto& corner : patch.corners)
    {
        float dist = (corner - patch.center).norm();
        patch.radius = std::max(patch.radius, dist);
    }
    // Also bound the surface-midpoint bulge: distance from chord centroid
    // to the surface is (sphereRadius - chordLen), and the surface midpoint
    // is the farthest interior point from the corners.
    patch.radius = std::max(patch.radius, sphereRadius - chordLen);
    
    // Compute unique ID for caching
    patch.patchId = computePatchId(patch.phi0, patch.theta0, patch.extent);
}

uint64_t TerrainLODManager::computePatchId(float phi0, float theta0, float extent) const
{
    // Hash must include extent so that patches at the same (phi0, theta0)
    // but different LOD levels (different extents) do NOT collide. A pure
    // (phi0, theta0) hash poisons the patch cache: when a parent patch
    // subdivides, the (0,0)-corner child shares its parent's coordinates,
    // so it would reuse the parent's cached mesh of the wrong size.
    uint32_t phi_bits = *reinterpret_cast<uint32_t*>(&phi0);
    uint32_t theta_bits = *reinterpret_cast<uint32_t*>(&theta0);
    uint32_t extent_bits = *reinterpret_cast<uint32_t*>(&extent);

    // FNV-1a style mix into a 64-bit value.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint32_t v) {
        h ^= static_cast<uint64_t>(v);
        h *= 1099511628211ull;
    };
    mix(phi_bits);
    mix(theta_bits);
    mix(extent_bits);
    return h;
}

void TerrainLODManager::generatePatchVertices(TerrainPatch& patch,
                                              float sphereRadius,
                                              const TerrainData& terrain)
{
    patch.vertices.clear();
    patch.normals.clear();
    patch.tangents.clear();
    patch.texCoords.clear();
    // Keep vertex density CONSTANT per patch regardless of subdivision level.
    // The previous "halve verts every 2 levels" decay was actively harmful:
    // at deep levels (close-in views) each patch is small in angular extent
    // but covered by very few triangles, so a single triangle ends up
    // spanning hundreds of meters of varied terrain and produces obvious
    // flat-shaded facets. Patch count (and total vertex budget) is already
    // bounded by maxVisiblePatches, so we don't save anything by thinning
    // individual patches.
    int vertCount = verticesPerPatchSide;
    
    const float degToRad = celestia::numbers::pi / 180.0f;

    // Chord-cut compensation: a flat triangle drawn between three points
    // ON a sphere of radius R lies INSIDE the sphere. Without compensation,
    // sub-triangle interiors sink BELOW the base ellipsoid sphere mesh
    // (which is itself triangulated and has its own chord sag) and lose
    // the z-fight, producing the characteristic dot/checker pattern where
    // only vertex regions render.
    //
    // CRITICAL: the bulge MUST be independent of this patch's LOD level.
    // Adjacent patches at different levels share an edge at identical
    // (phi, theta), so if their bulges differ the shared edge vertices end
    // up at different radii — opening a thin LOD-boundary crack that shows
    // the atmosphere/base sphere behind. We therefore use a single constant
    // lift scaled only by sphereRadius, large enough to dominate any
    // reasonable base-sphere tessellation chord sag but small enough to be
    // visually imperceptible (~0.1% of R, ~6 km on Earth).
    // Radial offset above the smooth base sphere. Historically this was
    // sphereRadius * 1e-3 (~6km on Earth) to ensure terrain always sits ABOVE
    // the base sphere even at low-LOD chord sag — but at that magnitude it
    // expanded the terrain silhouette outside the smooth-sphere silhouette by
    // a visible band, occluding the atmosphere halo all around the planet.
    // `terrainDepthBias` (clip-space, applied in the vertex shader) handles
    // z-fight against the base sphere, so we only need a tiny radial offset
    // here — just enough to keep the surface from sinking below the base
    // sphere geometrically at coarse LODs. 1e-5 of radius (~64m on Earth) is
    // imperceptible in the silhouette but well above float epsilon.
    const float chordBulge = sphereRadius * 1.0e-5f;
    // No global baseline lift: lifting every vertex by the worst-case
    // negative displacement makes the terrain shell taller than the highest
    // peak. At low altitudes the camera ends up INSIDE the shell and
    // back-face culling hides everything. Where displacement is negative
    // (e.g. ocean floor), the terrain mesh dips below the base sphere; the
    // base sphere then wins the depth test and provides the underlying
    // texture color — which is the desired behavior (oceans show the base
    // texture, not lifted terrain mesh).

    // Generate vertex grid
    for (int i = 0; i <= vertCount; i++)
    {
        for (int j = 0; j <= vertCount; j++)
        {
            // Normalized position within patch [0, 1]
            float u_local = static_cast<float>(j) / vertCount;
            float v_local = static_cast<float>(i) / vertCount;
            
            // Global spherical coordinates (degrees)
            float phi = patch.phi0 + u_local * patch.extent;
            float theta = patch.theta0 + v_local * patch.extent;
            
            // Global texture coordinates [0, 1] in equirectangular convention.
            // phi is longitude in [0, 360]; theta is colatitude in [0, 180]
            // (0 = north pole, 180 = south pole) — see the cartesian mapping
            // below where direction = (sinTheta*cosPhi, cosTheta, sinTheta*sinPhi).
            // Celestia's equirectangular textures wrap with longitude increasing
            // westward in mesh space (phi=0 along +X corresponds to lon=0, but the
            // texture's u axis runs the opposite way), so flip u to match.
            float u_global = 1.0f - phi / 360.0f;
            float v_global = theta / 180.0f;
            
            // Convert to radians
            float phi_rad = phi * degToRad;
            float theta_rad = theta * degToRad;
            
            // Spherical to Cartesian conversion
            float sinTheta = std::sin(theta_rad);
            float cosTheta = std::cos(theta_rad);
            float sinPhi = std::sin(phi_rad);
            float cosPhi = std::cos(phi_rad);
            
            Eigen::Vector3f direction(
                sinTheta * cosPhi,
                cosTheta,
                sinTheta * sinPhi
            );

            const Eigen::Vector3f baseNormal = direction.normalized();
            const float displacement = terrain.getHeightAt(u_global, v_global);
            Eigen::Vector3f position = baseNormal * (sphereRadius + chordBulge + displacement);
            Eigen::Vector3f normal = baseNormal;
            // Tangent matching LODSphereMesh::createVertices convention so
            // the normal map perturbs identically: T = (sinPhi, 0, -cosPhi).
            // Independent of theta (latitude) — points east along constant
            // latitude in mesh space.
            Eigen::Vector3f tangent(sinPhi, 0.0f, -cosPhi);

            patch.vertices.push_back(position);
            patch.normals.push_back(normal);
            patch.tangents.push_back(tangent);
            // texcoord vec4: xy = UV, z = skirt flag (0 for surface verts).
            patch.texCoords.push_back(Eigen::Vector4f(u_global, v_global, 0.0f, 0.0f));
        }
    }

    // Skirt vertices: duplicate each of the four edges, but pulled inward
    // by skirtDepth to hide T-junction cracks where this patch meets a
    // neighbor at a different LOD level. Layout (append after main grid):
    //   [N+1] top    (i=0)
    //   [N+1] bottom (i=N)
    //   [N+1] left   (j=0)
    //   [N+1] right  (j=N)
    // Indices are emitted in generatePatchIndices.
    const int N = vertCount;
    const int mainCount = (N + 1) * (N + 1);
    // Skirt depth: only needs to exceed the worst-case T-junction sag
    // between adjacent LOD levels. Heightmap-induced sag is bounded by the
    // heightmap's max relief, which for Earth-scale bodies is on the order
    // of 0.002 R (~13 km). The previous 0.1 R (~640 km on Earth) plunged
    // the skirts so deep into the planet's interior that, at sea-level
    // views, the camera could literally see them stretching to the planet
    // core — those huge diagonal V's the wireframe revealed.
    //
    // 0.002 R is plenty for T-junction cracks and stays close enough to
    // the surface that the chord-bulge + depth-bias-skip rules continue to
    // hide skirts from outside view.
    const float skirtDepth = sphereRadius * 2.0e-3f;
    auto emitSkirtVertex = [&](int i, int j)
    {
        const int idx = i * (N + 1) + j;
        const Eigen::Vector3f& p = patch.vertices[idx];
        const Eigen::Vector3f n = p.normalized();
        Eigen::Vector3f skirtPos = p - n * skirtDepth;
        patch.vertices.push_back(skirtPos);
        patch.normals.push_back(patch.normals[idx]);
        patch.tangents.push_back(patch.tangents[idx]);
        // Copy uv from the source vert but mark this vertex as a skirt (z=1).
        Eigen::Vector4f tc = patch.texCoords[idx];
        tc.z() = 1.0f;
        patch.texCoords.push_back(tc);
    };
    for (int j = 0; j <= N; ++j) emitSkirtVertex(0, j);  // top
    for (int j = 0; j <= N; ++j) emitSkirtVertex(N, j);  // bottom
    for (int i = 0; i <= N; ++i) emitSkirtVertex(i, 0);  // left
    for (int i = 0; i <= N; ++i) emitSkirtVertex(i, N);  // right
    patch.skirtBaseIndex = mainCount;
    patch.skirtVertCount = N + 1;
    if (patch.level >= 5 && !patch.vertices.empty())
    {
        (void)patch;
    }
}

void TerrainLODManager::generatePatchIndices(TerrainPatch& patch)
{
    patch.indices.clear();

    // Must match the vertex count emitted by generatePatchVertices.
    int vertCount = verticesPerPatchSide;
    
    int vertsPerRow = vertCount + 1;
    
    // Generate triangle indices (two triangles per grid quad).
    // Using explicit triangles avoids strip row-connection artifacts.
    for (int i = 0; i < vertCount; i++)
    {
        for (int j = 0; j < vertCount; j++)
        {
            unsigned int i0 = static_cast<unsigned int>(i * vertsPerRow + j);
            unsigned int i1 = static_cast<unsigned int>(i * vertsPerRow + (j + 1));
            unsigned int i2 = static_cast<unsigned int>((i + 1) * vertsPerRow + j);
            unsigned int i3 = static_cast<unsigned int>((i + 1) * vertsPerRow + (j + 1));

            patch.indices.push_back(i0);
            patch.indices.push_back(i1);
            patch.indices.push_back(i2);

            patch.indices.push_back(i1);
            patch.indices.push_back(i3);
            patch.indices.push_back(i2);
        }
    }

    // Skirt triangles: one quad per edge segment connecting the surface
    // edge vertex to the corresponding inward-pulled skirt vertex.
    // Skirts seal T-junction cracks where this patch meets a coarser
    // neighbor. Winding is intentionally emitted in BOTH orientations so
    // skirts are visible regardless of which side the camera sees — the
    // chord-bulge above the base sphere guarantees they only show through
    // at actual crack locations.
    const int N = vertCount;
    const int rowStride = vertsPerRow;
    const int topBase    = patch.skirtBaseIndex + 0 * (N + 1);
    const int bottomBase = patch.skirtBaseIndex + 1 * (N + 1);
    const int leftBase   = patch.skirtBaseIndex + 2 * (N + 1);
    const int rightBase  = patch.skirtBaseIndex + 3 * (N + 1);
    auto emitSkirtQuad = [&](unsigned int a, unsigned int b,
                             unsigned int c, unsigned int d)
    {
        // Two triangles, both orientations (so culling can't hide them):
        patch.indices.push_back(a); patch.indices.push_back(b); patch.indices.push_back(c);
        patch.indices.push_back(a); patch.indices.push_back(c); patch.indices.push_back(b);
        patch.indices.push_back(b); patch.indices.push_back(d); patch.indices.push_back(c);
        patch.indices.push_back(b); patch.indices.push_back(c); patch.indices.push_back(d);
    };
    // Top edge (i=0, j varies): grid vert (0,j) <-> skirt top[j]
    for (int j = 0; j < N; ++j)
    {
        unsigned int g0 = static_cast<unsigned int>(0 * rowStride + j);
        unsigned int g1 = static_cast<unsigned int>(0 * rowStride + j + 1);
        unsigned int s0 = static_cast<unsigned int>(topBase + j);
        unsigned int s1 = static_cast<unsigned int>(topBase + j + 1);
        emitSkirtQuad(g0, g1, s0, s1);
    }
    // Bottom edge (i=N, j varies)
    for (int j = 0; j < N; ++j)
    {
        unsigned int g0 = static_cast<unsigned int>(N * rowStride + j);
        unsigned int g1 = static_cast<unsigned int>(N * rowStride + j + 1);
        unsigned int s0 = static_cast<unsigned int>(bottomBase + j);
        unsigned int s1 = static_cast<unsigned int>(bottomBase + j + 1);
        emitSkirtQuad(g0, g1, s0, s1);
    }
    // Left edge (j=0, i varies)
    for (int i = 0; i < N; ++i)
    {
        unsigned int g0 = static_cast<unsigned int>(i * rowStride + 0);
        unsigned int g1 = static_cast<unsigned int>((i + 1) * rowStride + 0);
        unsigned int s0 = static_cast<unsigned int>(leftBase + i);
        unsigned int s1 = static_cast<unsigned int>(leftBase + i + 1);
        emitSkirtQuad(g0, g1, s0, s1);
    }
    // Right edge (j=N, i varies)
    for (int i = 0; i < N; ++i)
    {
        unsigned int g0 = static_cast<unsigned int>(i * rowStride + N);
        unsigned int g1 = static_cast<unsigned int>((i + 1) * rowStride + N);
        unsigned int s0 = static_cast<unsigned int>(rightBase + i);
        unsigned int s1 = static_cast<unsigned int>(rightBase + i + 1);
        emitSkirtQuad(g0, g1, s0, s1);
    }

    patch.indexCount = patch.indices.size();
}

void TerrainLODManager::generateMeshes(const TerrainData& terrain)
{
    for (auto& patch : visiblePatches)
    {
        // Check if already cached
        if (patchCache.count(patch.patchId) == 0)
        {
            // Generate new patch
            generatePatchVertices(patch, currentSphereRadius, terrain);
            generatePatchIndices(patch);
            uploadPatchToGPU(patch);
            
            // Store in cache
            auto cached = std::make_unique<TerrainPatch>(patch);
            patchCache[patch.patchId] = std::move(cached);
        }
        else
        {
            // Use cached data
            auto& cached = patchCache[patch.patchId];
            patch.vao = cached->vao;
            patch.vbo = cached->vbo;
            patch.nbo = cached->nbo;
            patch.tanbo = cached->tanbo;
            patch.tbo = cached->tbo;
            patch.ebo = cached->ebo;
            patch.indexCount = cached->indexCount;
            patch.meshGenerated = cached->meshGenerated;
        }
    }
}

void TerrainLODManager::uploadPatchToGPU(TerrainPatch& patch)
{
    // Create VAO
    glGenVertexArrays(1, &patch.vao);
    glBindVertexArray(patch.vao);
    
    // Upload vertices
    glGenBuffers(1, &patch.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, patch.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 patch.vertices.size() * sizeof(Eigen::Vector3f),
                 patch.vertices.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector3f), (void*)0);
    
    // Upload normals
    glGenBuffers(1, &patch.nbo);
    glBindBuffer(GL_ARRAY_BUFFER, patch.nbo);
    glBufferData(GL_ARRAY_BUFFER,
                 patch.normals.size() * sizeof(Eigen::Vector3f),
                 patch.normals.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector3f), (void*)0);

    // Upload tangents (attribute index 6 = TangentAttributeIndex). Required
    // when the shader has TexUsage::NormalTexture set so the normal map can
    // perturb per-pixel normals in tangent space the same way the smooth
    // sphere path does.
    glGenBuffers(1, &patch.tanbo);
    glBindBuffer(GL_ARRAY_BUFFER, patch.tanbo);
    glBufferData(GL_ARRAY_BUFFER,
                 patch.tangents.size() * sizeof(Eigen::Vector3f),
                 patch.tangents.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 3, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector3f), (void*)0);
    
    // Upload texture coordinates as vec4 (s, t, isSkirt, 0). The dynamic
    // shader pipeline declares in_TexCoord0 as vec4 and reads the skirt flag
    // from .z; .xy is sampled as the actual UV via .st in TexCoord2D().
    glGenBuffers(1, &patch.tbo);
    glBindBuffer(GL_ARRAY_BUFFER, patch.tbo);
    glBufferData(GL_ARRAY_BUFFER,
                 patch.texCoords.size() * sizeof(Eigen::Vector4f),
                 patch.texCoords.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Eigen::Vector4f), (void*)0);

    // Upload indices
    glGenBuffers(1, &patch.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, patch.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 patch.indices.size() * sizeof(unsigned int),
                 patch.indices.data(),
                 GL_STATIC_DRAW);
    
    glBindVertexArray(0);
    patch.meshGenerated = true;
}

void TerrainLODManager::render(CelestiaGLProgram* program)
{
    if (!program) return;
    
    program->use();
    
    // Explicit GL state: opaque, depth-tested, back-face culled.
    // Previous render passes (atmosphere/clouds) may leave blending or
    // culling in unexpected states.
    GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLboolean cullWasEnabled  = glIsEnabled(GL_CULL_FACE);
    GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
    GLint     prevCullFace = GL_BACK;
    GLint     prevFrontFace = GL_CCW;
    GLint     prevDepthFunc = GL_LESS;
    GLboolean prevDepthMask = GL_TRUE;
    glGetIntegerv(GL_CULL_FACE_MODE, &prevCullFace);
    glGetIntegerv(GL_FRONT_FACE, &prevFrontFace);
    glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    // Use the renderer-wide depth comparison so the function we leave behind
    // matches what later passes (atmosphere, clouds) expect. Using GREATER here
    // would silently leak a stricter comparison.
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    // Polygon offset disabled — chord-bulge alone should be enough.
    GLboolean polyOffsetWasEnabled = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    GLfloat   prevPolyFactor = 0.0f, prevPolyUnits = 0.0f;
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &prevPolyFactor);
    glGetFloatv(GL_POLYGON_OFFSET_UNITS,  &prevPolyUnits);
    glDisable(GL_POLYGON_OFFSET_FILL);

    int drawn = 0;
    // Optional wireframe (desktop GL only) for debugging: set
    // CELESTIA_TERRAIN_WIREFRAME=1 in the environment.
    static const bool wireframe = []{
        const char* v = std::getenv("CELESTIA_TERRAIN_WIREFRAME");
        return v && v[0] == '1';
    }();
#ifdef GL_LINE
    if (wireframe)
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
#endif
    for (const auto& patch : visiblePatches)
    {
        if (!patch.meshGenerated) continue;
        
        glBindVertexArray(patch.vao);
        glDrawElements(GL_TRIANGLES,
                       patch.indexCount,
                       GL_UNSIGNED_INT,
                       nullptr);
        ++drawn;
    }
#ifdef GL_LINE
    if (wireframe)
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif
    (void)drawn;
    glBindVertexArray(0);

    // Restore prior state
    if (polyOffsetWasEnabled) glEnable(GL_POLYGON_OFFSET_FILL); else glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(prevPolyFactor, prevPolyUnits);
    if (blendWasEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (cullWasEnabled)  glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (depthWasEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(static_cast<GLenum>(prevDepthFunc));
    glDepthMask(prevDepthMask);
    glCullFace(static_cast<GLenum>(prevCullFace));
    glFrontFace(static_cast<GLenum>(prevFrontFace));
}

void TerrainLODManager::printStats() const
{
    std::cout << "Terrain LOD Statistics:\n"
              << "  Patches evaluated: " << totalPatchesEvaluated << "\n"
              << "  Patches rendered: " << totalPatchesRendered << "\n"
              << "  Patches cached: " << patchCache.size() << "\n"
              << "  Visible patches: " << visiblePatches.size() << "\n";
}
