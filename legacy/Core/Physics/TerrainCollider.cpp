#include "TerrainCollider.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace physics {

void TerrainCollider::BuildFromMesh(const std::vector<glm::vec3>& vertices,
                                     const std::vector<unsigned int>& indices,
                                     const glm::mat4& transform) {
    mTriangles.clear();

    // Transform vertices
    std::vector<glm::vec3> transformed(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        glm::vec4 v = transform * glm::vec4(vertices[i], 1.0f);
        transformed[i] = glm::vec3(v);
    }

    // Build AABB
    mBounds.min = glm::vec3(std::numeric_limits<float>::max());
    mBounds.max = glm::vec3(std::numeric_limits<float>::lowest());
    for (auto& v : transformed) {
        mBounds.min = glm::min(mBounds.min, v);
        mBounds.max = glm::max(mBounds.max, v);
    }

    // Build triangles
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        Triangle tri;
        tri.v0 = transformed[indices[i]];
        tri.v1 = transformed[indices[i + 1]];
        tri.v2 = transformed[indices[i + 2]];
        glm::vec3 edge1 = tri.v1 - tri.v0;
        glm::vec3 edge2 = tri.v2 - tri.v0;
        tri.normal = glm::normalize(glm::cross(edge1, edge2));
        // Only keep roughly horizontal triangles (terrain)
        if (tri.normal.y > 0.1f)
            mTriangles.push_back(tri);
    }

    // Build spatial grid
    float rangeX = mBounds.max.x - mBounds.min.x;
    float rangeZ = mBounds.max.z - mBounds.min.z;
    mGridWidth = std::max(1, static_cast<int>(std::ceil(rangeX / mCellSize)));
    mGridDepth = std::max(1, static_cast<int>(std::ceil(rangeZ / mCellSize)));
    mGrid.resize(mGridWidth * mGridDepth);

    for (size_t i = 0; i < mTriangles.size(); ++i) {
        auto& tri = mTriangles[i];
        // Find min/max cell coverage for this triangle
        float minX = std::min({tri.v0.x, tri.v1.x, tri.v2.x});
        float maxX = std::max({tri.v0.x, tri.v1.x, tri.v2.x});
        float minZ = std::min({tri.v0.z, tri.v1.z, tri.v2.z});
        float maxZ = std::max({tri.v0.z, tri.v1.z, tri.v2.z});

        int cx0 = std::clamp(GetCellX(minX), 0, mGridWidth - 1);
        int cx1 = std::clamp(GetCellX(maxX), 0, mGridWidth - 1);
        int cz0 = std::clamp(GetCellZ(minZ), 0, mGridDepth - 1);
        int cz1 = std::clamp(GetCellZ(maxZ), 0, mGridDepth - 1);

        for (int cz = cz0; cz <= cz1; ++cz) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                mGrid[cz * mGridWidth + cx].triangleIndices.push_back(i);
            }
        }
    }
}

std::optional<float> TerrainCollider::GetHeightAt(float x, float z) const {
    if (!IsInBounds(x, z)) return std::nullopt;

    int cx = std::clamp(GetCellX(x), 0, mGridWidth - 1);
    int cz = std::clamp(GetCellZ(z), 0, mGridDepth - 1);
    auto* cell = GetCell(cx, cz);
    if (!cell) return std::nullopt;

    glm::vec3 point(x, 0.0f, z);
    std::optional<float> bestHeight;

    for (size_t idx : cell->triangleIndices) {
        auto h = BarycentricHeight(point, mTriangles[idx]);
        if (h.has_value()) {
            if (!bestHeight.has_value() || *h > *bestHeight)
                bestHeight = h;
        }
    }

    return bestHeight;
}

glm::vec3 TerrainCollider::GetNormalAt(float x, float z) const {
    if (!IsInBounds(x, z)) return glm::vec3(0.0f, 1.0f, 0.0f);

    int cx = std::clamp(GetCellX(x), 0, mGridWidth - 1);
    int cz = std::clamp(GetCellZ(z), 0, mGridDepth - 1);
    auto* cell = GetCell(cx, cz);
    if (!cell) return glm::vec3(0.0f, 1.0f, 0.0f);

    glm::vec3 point(x, 0.0f, z);

    for (size_t idx : cell->triangleIndices) {
        auto h = BarycentricHeight(point, mTriangles[idx]);
        if (h.has_value()) {
            return mTriangles[idx].normal;
        }
    }

    return glm::vec3(0.0f, 1.0f, 0.0f);
}

HitResult TerrainCollider::Raycast(const Ray& ray, float maxDist) const {
    HitResult best;
    best.distance = maxDist;

    for (auto& tri : mTriangles) {
        auto hit = RayVsTriangle(ray, tri.v0, tri.v1, tri.v2);
        if (hit.hit && hit.distance < best.distance) {
            best = hit;
        }
    }

    return best;
}

bool TerrainCollider::IsInBounds(float x, float z) const {
    return x >= mBounds.min.x && x <= mBounds.max.x &&
           z >= mBounds.min.z && z <= mBounds.max.z;
}

std::optional<float> TerrainCollider::BarycentricHeight(const glm::vec3& p,
                                                         const Triangle& tri) {
    // Project onto XZ plane for barycentric test
    float x = p.x, z = p.z;

    float x0 = tri.v0.x, z0 = tri.v0.z;
    float x1 = tri.v1.x, z1 = tri.v1.z;
    float x2 = tri.v2.x, z2 = tri.v2.z;

    float denom = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2);
    if (std::abs(denom) < 1e-8f) return std::nullopt;

    float u = ((z1 - z2) * (x - x2) + (x2 - x1) * (z - z2)) / denom;
    float v = ((z2 - z0) * (x - x2) + (x0 - x2) * (z - z2)) / denom;
    float w = 1.0f - u - v;

    if (u < -0.001f || v < -0.001f || w < -0.001f) return std::nullopt;

    return u * tri.v0.y + v * tri.v1.y + w * tri.v2.y;
}

int TerrainCollider::GetCellX(float x) const {
    return static_cast<int>((x - mBounds.min.x) / mCellSize);
}

int TerrainCollider::GetCellZ(float z) const {
    return static_cast<int>((z - mBounds.min.z) / mCellSize);
}

const TerrainCollider::GridCell* TerrainCollider::GetCell(int cx, int cz) const {
    if (cx < 0 || cx >= mGridWidth || cz < 0 || cz >= mGridDepth) return nullptr;
    return &mGrid[cz * mGridWidth + cx];
}

} // namespace physics
