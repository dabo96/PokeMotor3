#pragma once
#include <glm/glm.hpp>
#include <optional>

namespace physics {

struct AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    glm::vec3 Center() const { return (min + max) * 0.5f; }
    glm::vec3 Extents() const { return (max - min) * 0.5f; }
    bool Contains(const glm::vec3& point) const;
    AABB Expanded(const glm::vec3& amount) const;
    AABB Translated(const glm::vec3& offset) const;
};

struct Sphere {
    glm::vec3 center{0.0f};
    float radius = 0.5f;
};

struct Capsule {
    glm::vec3 base{0.0f};   // Bottom sphere center
    glm::vec3 tip{0.0f, 1.0f, 0.0f}; // Top sphere center
    float radius = 0.3f;
};

struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
};

struct HitResult {
    bool hit = false;
    float distance = 0.0f;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
};

// Intersection tests
bool TestAABBvsAABB(const AABB& a, const AABB& b);
bool TestSphereVsSphere(const Sphere& a, const Sphere& b);
bool TestAABBvsSphere(const AABB& box, const Sphere& sphere);
bool TestSphereVsCapsule(const Sphere& sphere, const Capsule& capsule);

HitResult RayVsAABB(const Ray& ray, const AABB& box);
HitResult RayVsSphere(const Ray& ray, const Sphere& sphere);
HitResult RayVsTriangle(const Ray& ray, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2);

// Closest point helpers
glm::vec3 ClosestPointOnSegment(const glm::vec3& point, const glm::vec3& a, const glm::vec3& b);
glm::vec3 ClosestPointOnAABB(const glm::vec3& point, const AABB& box);

// Overlap resolution
glm::vec3 ResolveAABBOverlap(const AABB& moving, const AABB& stationary);
glm::vec3 ResolveSphereVsAABB(const Sphere& sphere, const AABB& box);

} // namespace physics
