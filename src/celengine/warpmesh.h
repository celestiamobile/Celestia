//
// warpmesh.h
//
// Copyright © 2020-2025 Celestia Development Team. All rights reserved.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <Eigen/Core>

namespace celestia::gl
{
class Buffer;
class VertexObject;
} // end namespace gl

// File format for data used to warp an image, for
// detail, see http://paulbourke.net/dataformats/meshwarp/
class WarpMesh //NOSONAR
{
public:
    struct WarpVertex;

    WarpMesh(std::uint32_t, std::uint32_t, std::vector<WarpVertex>&&);
    ~WarpMesh();

    static std::unique_ptr<WarpMesh> load(const std::filesystem::path&);

    // Map data to triangle vertices used for drawing
    void setUpVertexObject(celestia::gl::VertexObject& vo) const;

    // Convert a vertex coordinate to texture coordinate
    bool mapVertex(float x, float y, float& u, float& v) const;
    void mapTexture(float u, float v, std::vector<Eigen::Vector2f>& output) const;

private:
    struct InverseMap;
    std::array<const WarpVertex*, 3> triangleVertices(std::uint32_t) const;

    std::uint32_t m_nx;
    std::uint32_t m_ny;
    std::vector<WarpVertex> m_data;
    std::vector<float> m_yCoords;
    std::unique_ptr<InverseMap> m_inverseMap;
};
