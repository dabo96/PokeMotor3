#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <optional>
#include "Shapes.h"

class Mesh;

namespace physics {

class TerrainCollider {
public:
    // Build from mesh vertex data (extracts triangles from the mesh)
    void BuildFromMesh(const std::vector<glm::vec3>& vertices, const std::vector<unsigned int>& indices,
                       const glm::mat4& transform = glm::mat4(1.0f));

    // Get terrain height at world XZ position (returns nullopt if outside terrain bounds)
    std::optional<float> GetHeightAt(float x, float z) const;

    // Get terrain normal at world XZ position
    glm::vec3 GetNormalAt(float x, float z) const;

    // Raycast against terrain
    HitResult Raycast(const Ray& ray, float maxDist = 1000.0f) const;

    // Check if a point is within the terrain's XZ bounds
    bool IsInBounds(float x, float z) const;

    const AABB& GetBounds() const { return mBounds; }

private:
    struct Triangle {
        glm::vec3 v0, v1, v2;
        glm::vec3 normal;
    };

    // Spatial grid for acceleration
    struct GridCell {
        std::vector<size_t> triangleIndices;
    };

    // Barycentric interpolation on triangle
    static std::optional<float> BarycentricHeight(const glm::vec3& p,
                                                   const Triangle& tri);

    int GetCellX(float x) const;
    int GetCellZ(float z) const;
    const GridCell* GetCell(int cx, int cz) const;

    std::vector<Triangle> mTriangles;
    std::vector<GridCell> mGrid;

    AABB mBounds;
    float mCellSize = 4.0f;
    int mGridWidth = 0;
    int mGridDepth = 0;
};

} // namespace physics
