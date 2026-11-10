//
// warpmesh.cpp
//
// Copyright © 2020-2025 Celestia Development Team. All rights reserved.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "warpmesh.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <istream>
#include <limits>
#include <numeric>
#include <string_view>
#include <type_traits>
#include <utility>

#include <celmath/mathlib.h>
#include <celrender/gl/buffer.h>
#include <celrender/gl/vertexobject.h>
#include <celutil/fsutils.h>
#include <celutil/logger.h>
#include "shadermanager.h"

using namespace std::string_view_literals;
using celestia::util::GetLogger;

namespace gl = celestia::gl;
namespace math = celestia::math;
namespace util = celestia::util;

struct WarpMesh::WarpVertex
{
    float x;
    float y;
    float u;
    float v;
    float i;
};

// Index source-UV triangles so moving labels do not scan the entire mesh.
struct WarpMesh::InverseMap
{
    struct Node
    {
        Eigen::Vector2f min;
        Eigen::Vector2f max;
        std::uint32_t first;
        std::uint32_t count;
        std::uint32_t right{0};
    };
    std::vector<std::uint32_t> triangles;
    std::vector<Node> nodes;

    explicit InverseMap(const WarpMesh& mesh)
    {
        const auto count = (mesh.m_nx - 1U) * (mesh.m_ny - 1U) * 2U;
        triangles.resize(count);
        std::iota(triangles.begin(), triangles.end(), 0U);
        build(mesh, 0, count);
    }

    std::uint32_t build(const WarpMesh& mesh, std::uint32_t first, std::uint32_t count)
    {
        const auto infinity = std::numeric_limits<float>::infinity();
        Node node{Eigen::Vector2f::Constant(infinity), Eigen::Vector2f::Constant(-infinity), first, count};
        for (auto i = first; i < first + count; ++i)
        {
            for (const auto* vertex : mesh.triangleVertices(triangles[i]))
            {
                const Eigen::Vector2f point{vertex->u, vertex->v};
                node.min = node.min.cwiseMin(point);
                node.max = node.max.cwiseMax(point);
            }
        }
        const auto index = static_cast<std::uint32_t>(nodes.size());
        nodes.push_back(node);
        if (count > 8U)
        {
            const int axis = node.max.x() - node.min.x() > node.max.y() - node.min.y() ? 0 : 1;
            const auto center = [&mesh, axis](std::uint32_t triangle)
            {
                const auto v = mesh.triangleVertices(triangle);
                return axis == 0 ? v[0]->u + v[1]->u + v[2]->u
                                 : v[0]->v + v[1]->v + v[2]->v;
            };
            const auto half = count / 2U;
            std::nth_element(triangles.begin() + first, triangles.begin() + first + half,
                             triangles.begin() + first + count,
                             [&center](auto a, auto b) { return center(a) < center(b); });
            nodes[index].first = build(mesh, first, half);
            nodes[index].right = build(mesh, first + half, count - half);
            nodes[index].count = 0;
        }
        return index;
    }

    void query(const Eigen::Vector2f& point, std::vector<std::uint32_t>& output,
               std::uint32_t index = 0) const
    {
        const auto& node = nodes[index];
        if ((point.array() < node.min.array() - 1.0e-6f).any() ||
            (point.array() > node.max.array() + 1.0e-6f).any())
            return;
        if (node.count != 0)
            output.insert(output.end(), triangles.begin() + node.first,
                           triangles.begin() + node.first + node.count);
        else
        {
            query(point, output, node.first);
            query(point, output, node.right);
        }
    }
};

namespace {

constexpr std::uint32_t MaxDimension = 1024;
constexpr std::uint32_t MaxUShortVertices = std::numeric_limits<std::uint16_t>::max() + UINT32_C(1);

constexpr std::string_view RegularGridError = "Only monotonic grid warp meshes are supported\n"sv;

bool
readVertex(std::istream& in, WarpMesh::WarpVertex* vertex,
           std::uint32_t nx, std::uint32_t x, std::uint32_t y)
{
    assert(vertex != nullptr);
    if (!(in >> vertex->x >> vertex->y >> vertex->u >> vertex->v >> vertex->i))
    {
        GetLogger()->error("Failed to read mesh data\n");
        return false;
    }

    if (vertex->u < 0.0f || vertex->u > 1.0f ||
        vertex->v < 0.0f || vertex->v > 1.0f ||
        vertex->i < 0.0f || vertex->i > 1.0f)
    {
        GetLogger()->error("Missing vertices are not supported\n");
        return false;
    }

    if (!(std::isfinite(vertex->x) && std::isfinite(vertex->y) &&
          std::isfinite(vertex->u) && std::isfinite(vertex->v) &&
          std::isfinite(vertex->i)))
    {
        GetLogger()->error("Mesh vertex properties must be finite\n");
        return false;
    }

    // Check that the points form a regular grid
    if (x > 0)
    {
        const WarpMesh::WarpVertex* left = vertex - 1;
        if (left->x >= vertex->x || left->y != vertex->y)
        {
            GetLogger()->error(RegularGridError);
            return false;
        }
    }

    if (y > 0)
    {
        const WarpMesh::WarpVertex* up = vertex - nx;
        if (up->x != vertex->x || up->y >= vertex->y)
        {
            GetLogger()->error(RegularGridError);
            return false;
        }
    }

    return true;
}

template<typename T, gl::VertexObject::IndexType IndexType>
void
setIndices(gl::VertexObject& vo,
           std::uint32_t nx, std::uint32_t ny)
{
    const std::uint32_t indexCount = (nx - 1U) * (ny - 1U) * 6U;

    std::vector<T> indices;
    indices.reserve(indexCount);

    std::uint32_t idx = 0;
    for (std::uint32_t y = 0; y < (nx - 1); ++y)
    {
        for (std::uint32_t x = 0; x < (nx - 1); ++x)
        {
            // Top left triangle
            indices.push_back(static_cast<T>(idx + nx));
            indices.push_back(static_cast<T>(idx));
            indices.push_back(static_cast<T>(idx + 1U));
            // Bottom right triangle
            indices.push_back(static_cast<T>(idx + nx));
            indices.push_back(static_cast<T>(idx + 1));
            indices.push_back(static_cast<T>(idx + nx + 1));
            ++idx;
        }

        // skip end of row
        ++idx;
    }

    vo.setIndexBuffer(gl::Buffer::create(gl::Buffer::TargetHint::ElementArray,
                                         std::as_bytes(std::span{indices})),
                      0, IndexType);
    vo.setCount(static_cast<GLsizei>(indices.size()));
}

inline void
normalizeUV(float& u, float& v)
{
    // Texture coordinate is [0, 1], normalize to [-1, 1]
    u = u * 2 - 1;
    v = v * 2 - 1;
}

inline void
interpolateUV(float f, const WarpMesh::WarpVertex* a, const WarpMesh::WarpVertex* b, float& u, float& v)
{
    u = math::lerp(a->u, b->u, f);
    v = math::lerp(a->v, b->v, f);
    normalizeUV(u, v);
}

void
interpolateTriangle(const WarpMesh::WarpVertex* v1,
                    const WarpMesh::WarpVertex* v2,
                    const WarpMesh::WarpVertex* v3,
                    float w1, float w2,
                    float& u, float& v)
{
    float w3 = 1.0f - w1 - w2;
    u = w1 * v1->u + w2 * v2->u + w3 * v3->u;
    v = w1 * v1->v + w2 * v2->v + w3 * v3->v;
}

void
interpolateQuad(const WarpMesh::WarpVertex* bottomRight, std::uint32_t nx, float x, float y, float& u, float& v)
{
    const WarpMesh::WarpVertex* const bottomLeft = bottomRight - 1U;
    const WarpMesh::WarpVertex* const topLeft = bottomLeft - nx;
    const WarpMesh::WarpVertex* const topRight = bottomRight - nx;

    // Barycentric interpolation on the grid cell normalized to the unit square
    if (float fx = (x - topLeft->x) / (topRight->x - topLeft->x),
              fy = (y - topLeft->y) / (bottomLeft->y - topLeft->y);
        fx + fy < 1.0f)
    {
        // top left triangle
        interpolateTriangle(bottomLeft, topLeft, topRight, fy, 1.0f - fx - fy, u, v);
    }
    else
    {
        // bottom right triangle
        interpolateTriangle(bottomLeft, topRight, bottomRight, 1.0f - fx, 1.0f - fy, u, v);
    }

    normalizeUV(u, v);
}

} // end unnamed namespace

WarpMesh::WarpMesh(std::uint32_t nx, std::uint32_t ny,
                   std::vector<WarpVertex>&& data) :
    m_nx(nx),
    m_ny(ny),
    m_data(std::move(data))
{
    // to simplify interpolation, store the y coordinates of the grid rows
    // in a separate structure
    m_yCoords.reserve(ny);
    for (std::uint32_t i = 0; i < m_data.size(); i += m_nx)
        m_yCoords.push_back(m_data[i].y);
    m_inverseMap = std::make_unique<InverseMap>(*this);
}

WarpMesh::~WarpMesh() = default;

void
WarpMesh::setUpVertexObject(gl::VertexObject& vo) const
{
    static_assert(sizeof(WarpMesh::WarpVertex) == 5 * sizeof(float));
    static_assert(std::is_standard_layout_v<WarpMesh::WarpVertex>);

    const std::uint32_t vertexCount = m_nx * m_ny;
    auto buf = gl::Buffer::create(gl::Buffer::TargetHint::Array,
                                  std::as_bytes(std::span{m_data}),
                                  gl::Buffer::BufferUsage::StaticDraw);

    vo.addVertexBuffer(
        buf,
        CelestiaGLProgram::VertexCoordAttributeIndex,
        2,
        gl::VertexObject::DataType::Float,
        false,
        sizeof(WarpMesh::WarpVertex),
        offsetof(WarpMesh::WarpVertex, x));
    vo.addVertexBuffer(
        buf,
        CelestiaGLProgram::TextureCoord0AttributeIndex,
        2,
        gl::VertexObject::DataType::Float,
        false,
        sizeof(WarpMesh::WarpVertex),
        offsetof(WarpMesh::WarpVertex, u));
    vo.addVertexBuffer(
        buf,
        CelestiaGLProgram::IntensityAttributeIndex,
        1,
        gl::VertexObject::DataType::Float,
        false,
        sizeof(WarpMesh::WarpVertex),
        offsetof(WarpMesh::WarpVertex, i));

    if (vertexCount <= MaxUShortVertices)
        setIndices<GLushort, gl::VertexObject::IndexType::UnsignedShort>(vo, m_nx, m_ny);
    else
        setIndices<GLuint, gl::VertexObject::IndexType::UnsignedInt>(vo, m_nx, m_ny);
}

bool WarpMesh::mapVertex(float x, float y, float& u, float& v) const
{
    if (x < m_data[0].x || x > m_data[m_nx - 1U].x || y < m_yCoords[0] || y > m_yCoords[m_ny - 1U])
        return false;

    const auto xStart = m_data.begin();
    const auto xEnd = xStart + m_nx;
    auto xIt = std::lower_bound(xStart, xEnd, x,
                                [](const WarpVertex& vtx, float xc) { return vtx.x < xc; });
    if (xIt == xEnd)
        return false;
    const auto xIndex = static_cast<std::uint32_t>(xIt - xStart);

    auto yStart = m_yCoords.begin();
    auto yEnd = m_yCoords.end();
    auto yIt = std::lower_bound(yStart, yEnd, y);
    if (yIt == yEnd)
        return false;
    const auto yIndex = static_cast<std::uint32_t>(yIt - yStart);

    const WarpVertex* vertex = m_data.data() + (yIndex * m_nx + xIndex);
    if (xIt->x == x || xIndex == 0)
    {
        // Special case: exactly on x gridline
        if (*yIt == y || yIndex == 0)
        {
            // Extra special case: exactly on grid point
            u = vertex->u;
            v = vertex->v;
            return true;
        }

        const WarpVertex* upVertex = vertex - m_nx;
        float f = (y - upVertex->y) / (vertex->y - upVertex->y);
        interpolateUV(f, upVertex, vertex, u, v);
        return true;
    }

    if (*yIt == y || yIndex == 0)
    {
        // Special case: exactly on y gridline
        const WarpVertex* leftVertex = vertex - 1U;
        float f = (x - leftVertex->x) / (vertex->x - leftVertex->x);
        interpolateUV(f, leftVertex, vertex, u, v);
        return true;
    }

    interpolateQuad(vertex, m_nx, x, y, u, v);
    return true;
}

std::array<const WarpMesh::WarpVertex*, 3>
WarpMesh::triangleVertices(std::uint32_t triangle) const
{
    const auto cell = triangle / 2U;
    const auto* topLeft = &m_data[(cell / (m_nx - 1U)) * m_nx + cell % (m_nx - 1U)];
    return triangle % 2U == 0
        ? std::array{topLeft + m_nx, topLeft, topLeft + 1}
        : std::array{topLeft + m_nx, topLeft + 1, topLeft + m_nx + 1};
}

void WarpMesh::mapTexture(float u, float v, std::vector<Eigen::Vector2f>& output) const
{
    constexpr float tolerance = 1.0e-6f;
    std::vector<std::uint32_t> candidates;
    m_inverseMap->query({u, v}, candidates);
    const auto first = output.size();
    for (const auto triangle : candidates)
    {
        const auto vertices = triangleVertices(triangle);
        const auto* a = vertices[0];
        const auto* b = vertices[1];
        const auto* c = vertices[2];
        const double determinant = (double(b->u) - a->u) * (double(c->v) - a->v)
                                 - (double(c->u) - a->u) * (double(b->v) - a->v);
        if (determinant == 0.0)
            continue;
        const double w1 = ((double(u) - a->u) * (double(c->v) - a->v)
                         - (double(c->u) - a->u) * (double(v) - a->v)) / determinant;
        const double w2 = ((double(b->u) - a->u) * (double(v) - a->v)
                         - (double(u) - a->u) * (double(b->v) - a->v)) / determinant;
        if (w1 < -tolerance || w2 < -tolerance || w1 + w2 > 1.0 + tolerance)
            continue;
        const Eigen::Vector2f position{
            static_cast<float>((1.0 - w1 - w2) * a->x + w1 * b->x + w2 * c->x),
            static_cast<float>((1.0 - w1 - w2) * a->y + w1 * b->y + w2 * c->y)};
        if (std::none_of(output.begin() + first, output.end(),
                         [&position](const auto& other) { return (position - other).squaredNorm() < 1.0e-10f; }))
            output.push_back(position);
    }
}

std::unique_ptr<WarpMesh>
WarpMesh::load(const std::filesystem::path& name)
{
    constexpr std::uint32_t MESHTYPE_RECT = 2;

    std::ifstream f("warp" / name);
    if (!f.good())
        return nullptr;

    std::uint32_t type, nx, ny;
    if (!(f >> type))
    {
        GetLogger()->error("Failed to read mesh header\n");
        return nullptr;
    }

    if (type != MESHTYPE_RECT)
    {
        GetLogger()->error("Unsupported mesh type found: {}\n", type);
        return nullptr;
    }

    if (!(f >> nx >> ny))
    {
        GetLogger()->error("Failed to read mesh header\n");
        return nullptr;
    }

    if (nx < 2U || ny < 2U)
    {
        GetLogger()->error("Row and column numbers should be larger than 2\n");
        return nullptr;
    }

    if (nx > MaxDimension || ny > MaxDimension)
    {
        GetLogger()->error("Row and column numbers should be smaller than {}\n", MaxDimension);
        return nullptr;
    }

    std::vector<WarpVertex> data;
    data.reserve(nx * ny);
    for (std::uint32_t y = 0; y < ny; ++y)
    {
        for (std::uint32_t x = 0; x < nx; ++x)
        {
            WarpVertex* ptr = &data.emplace_back();
            if (!readVertex(f, ptr, nx, x, y))
                return nullptr;
        }
    }

    GetLogger()->info("Read a mesh of {} x {}\n", nx, ny);
    return std::make_unique<WarpMesh>(nx, ny, std::move(data));
}
