// terrainlodmanager.h
//
// Copyright (C) 2026-present, Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

#include <Eigen/Core>

#include <celengine/glsupport.h>
#include <celengine/shadermanager.h>
#include <celrender/gl/buffer.h>

namespace celestia::math
{
class Frustum;
}

class TerrainData;
class Body;

/// Represents a single terrain patch in the quadtree hierarchy
struct TerrainPatch
{
    // Spherical coordinate system (0-360 degrees)
    float phi0, theta0;           // Origin coordinates
    float extent;                 // Size in degrees
    int level;                    // Subdivision level (0 = root, higher = finer)
    
    // Derived 3D geometry
    Eigen::Vector3f center;       // Center position in 3D
    float radius;                 // Bounding sphere radius
    std::array<Eigen::Vector3f, 4> corners;  // 4 corner positions
    
    // Height bounds (precomputed from heightmap data)
    float heightMin = 0.0f;
    float heightMax = 0.0f;
    
    // Unique patch identifier for caching
    uint64_t patchId = 0;
    
    // Rendering data. texCoords are vec4: (s, t, isSkirt, 0) so the dynamic
    // shader pipeline (which exposes in_TexCoord0 as vec4) can read the skirt
    // flag from .z without needing a dedicated attribute slot.
    std::vector<Eigen::Vector3f> vertices;
    std::vector<Eigen::Vector3f> normals;
    std::vector<Eigen::Vector4f> texCoords;
    std::vector<unsigned int> indices;
    
    GLuint vao = 0;               // Vertex array object
    GLuint vbo = 0;               // Vertex buffer object
    GLuint nbo = 0;               // Normal buffer object
    GLuint tbo = 0;               // Texture coordinate buffer object (s,t,isSkirt,0)
    GLuint ebo = 0;               // Element (index) buffer object
    int indexCount = 0;
    int skirtBaseIndex = 0;    // vertex index where skirt verts begin
    int skirtVertCount = 0;    // verts per skirt edge (N+1)
    
    bool meshGenerated = false;
};

/// Manages LOD-based terrain rendering using adaptive quadtree subdivision
class TerrainLODManager
{
public:
    TerrainLODManager();
    ~TerrainLODManager();
    
    TerrainLODManager(const TerrainLODManager&) = delete;
    TerrainLODManager& operator=(const TerrainLODManager&) = delete;
    TerrainLODManager(TerrainLODManager&&) = delete;
    TerrainLODManager& operator=(TerrainLODManager&&) = delete;
    
    /// Update visible patches based on camera position and frustum
    /// @param cameraPos World position of camera
    /// @param frustum View frustum for culling
    /// @param sphereRadius Planet radius in km
    /// @param cameraDistance Distance from camera to planet center
    void selectPatches(const Eigen::Vector3f& cameraPos,
                       const celestia::math::Frustum& frustum,
                       float sphereRadius,
                       float cameraDistance);
    
    /// Generate mesh data for all visible patches
    /// @param terrain Heightmap data source
    void generateMeshes(const TerrainData& terrain);
    
    /// Render all visible patches
    /// @param program Shader program to use
    void render(CelestiaGLProgram* program);
    
    /// Get list of visible patches (for debugging)
    const std::vector<TerrainPatch>& getVisiblePatches() const
    {
        return visiblePatches;
    }
    
    /// Configure refinement sensitivity
    /// @param angleRadians Angular size threshold in radians
    void setRefinementThreshold(float angleRadians)
    {
        refinementThreshold = angleRadians;
    }
    
    /// Set maximum LOD level
    /// @param level Maximum subdivision level (0-16 typical)
    void setMaxPatchLevel(int level)
    {
        maxPatchLevel = level;
    }
    
    /// Set maximum number of patches to keep in memory
    void setMaxVisiblePatches(int count)
    {
        maxVisiblePatches = count;
    }
    
    /// Clear all cached patch data
    void clearCache();
    
    /// Print statistics (for debugging)
    void printStats() const;
    
private:
    /// Recursively select patches based on camera distance
    void selectPatchesRecursive(TerrainPatch& patch,
                                const Eigen::Vector3f& cameraPos,
                                const celestia::math::Frustum& frustum,
                                float sphereRadius);
    
    /// Check if a patch should be refined further
    bool shouldRefine(const TerrainPatch& patch,
                      const Eigen::Vector3f& cameraPos,
                      float sphereRadius) const;
    
    /// Subdivide a patch into 4 children
    std::array<TerrainPatch, 4> subdividePatch(const TerrainPatch& patch) const;
    
    /// Compute bounding sphere for a patch
    void computePatchGeometry(TerrainPatch& patch, float sphereRadius);
    
    /// Generate sphere mesh vertices for a patch
    void generatePatchVertices(TerrainPatch& patch,
                               float sphereRadius,
                               const TerrainData& terrain);
    
    /// Create index buffer for patch mesh
    void generatePatchIndices(TerrainPatch& patch);
    
    /// Upload patch data to GPU
    void uploadPatchToGPU(TerrainPatch& patch);
    
    /// Compute unique ID for patch caching
    uint64_t computePatchId(float phi0, float theta0, float extent) const;
    
    // Configuration
    // refinementThreshold is the minimum angular size (radians) a patch must
    // subtend from the camera before it gets subdivided. Smaller = more
    // subdivision = finer triangles at close range, but linearly more draw
    // calls. With 32 verts/side, 0.16 rad (~9°) keeps per-triangle screen
    // area near 0.28° — Gouraud scattering bands invisible — while
    // producing ~1/4 the patches (and draw calls) of the earlier 16-vert /
    // 0.08-rad configuration.
    float refinementThreshold = 0.16f;
    // maxPatchLevel caps recursion depth. At L10, each root quadrant (90°)
    // is split into 4^10 = ~1M leaf patches in the worst case, but the
    // visible-patch cap (maxVisiblePatches) keeps memory bounded — only
    // patches near the camera ever subdivide that deep. Going beyond L6
    // (the previous cap) is essential for ground-level viewing: at L6 each
    // patch is ~150 km wide, so close-in views see flat-shaded facets
    // hundreds of meters across. With 32 verts/side we can drop one level
    // and still cover finer-than-meter ground at deep subdivision.
    int maxPatchLevel = 13;
    int maxVisiblePatches = 5000;       // Memory limit
    // 32 verts/side ⇒ 33×33 = 1089 vertices, 2048 triangles per patch.
    // Trades CPU draw-call overhead (fewer larger patches) for slightly
    // more vertex work per patch. On modern GPUs this is an unambiguous
    // win: vertex shading is cheap, but each glDrawElements has CPU/driver
    // cost and the per-pixel scattering shader is what really hurts when
    // patches multiply.
    int verticesPerPatchSide = 32;
    
    // State
    float currentSphereRadius = 1.0f;
    std::vector<TerrainPatch> visiblePatches;
    std::unordered_map<uint64_t, std::unique_ptr<TerrainPatch>> patchCache;
    
    // Statistics
    mutable int totalPatchesEvaluated = 0;
    mutable int totalPatchesRendered = 0;
};
