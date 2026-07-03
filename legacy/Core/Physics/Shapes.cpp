#include "Shapes.h"
#include <algorithm>
#include <cmath>

namespace physics {

bool AABB::Contains(const glm::vec3& point) const {
    return point.x >= min.x && point.x <= max.x &&
           point.y >= min.y && point.y <= max.y &&
           point.z >= min.z && point.z <= max.z;
}

AABB AABB::Expanded(const glm::vec3& amount) const {
    return {min - amount, max + amount};
}

AABB AABB::Translated(const glm::vec3& offset) const {
    return {min + offset, max + offset};
}

bool TestAABBvsAABB(const AABB& a, const AABB& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x &&
           a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}

bool TestSphereVsSphere(const Sphere& a, const Sphere& b) {
    glm::vec3 diff = a.center - b.center;
    float distSq = glm::dot(diff, diff);
    float radiusSum = a.radius + b.radius;
    return distSq <= radiusSum * radiusSum;
}

bool TestAABBvsSphere(const AABB& box, const Sphere& sphere) {
    glm::vec3 closest = ClosestPointOnAABB(sphere.center, box);
    glm::vec3 diff = closest - sphere.center;
    return glm::dot(diff, diff) <= sphere.radius * sphere.radius;
}

bool TestSphereVsCapsule(const Sphere& sphere, const Capsule& capsule) {
    glm::vec3 closest = ClosestPointOnSegment(sphere.center, capsule.base, capsule.tip);
    glm::vec3 diff = sphere.center - closest;
    float distSq = glm::dot(diff, diff);
    float radiusSum = sphere.radius + capsule.radius;
    return distSq <= radiusSum * radiusSum;
}

HitResult RayVsAABB(const Ray& ray, const AABB& box) {
    HitResult result;
    glm::vec3 invDir = 1.0f / ray.direction;

    float t1 = (box.min.x - ray.origin.x) * invDir.x;
    float t2 = (box.max.x - ray.origin.x) * invDir.x;
    float t3 = (box.min.y - ray.origin.y) * invDir.y;
    float t4 = (box.max.y - ray.origin.y) * invDir.y;
    float t5 = (box.min.z - ray.origin.z) * invDir.z;
    float t6 = (box.max.z - ray.origin.z) * invDir.z;

    float tmin = std::max({std::min(t1, t2), std::min(t3, t4), std::min(t5, t6)});
    float tmax = std::min({std::max(t1, t2), std::max(t3, t4), std::max(t5, t6)});

    if (tmax < 0 || tmin > tmax) return result;

    result.hit = true;
    result.distance = tmin < 0 ? tmax : tmin;
    result.point = ray.origin + ray.direction * result.distance;

    // Determine normal based on which face was hit
    glm::vec3 center = box.Center();
    glm::vec3 d = result.point - center;
    glm::vec3 extents = box.Extents();
    float bias = 1.001f;
    result.normal = glm::normalize(glm::vec3(
        int(d.x / extents.x * bias),
        int(d.y / extents.y * bias),
        int(d.z / extents.z * bias)
    ));

    return result;
}

HitResult RayVsSphere(const Ray& ray, const Sphere& sphere) {
    HitResult result;
    glm::vec3 oc = ray.origin - sphere.center;
    float a = glm::dot(ray.direction, ray.direction);
    float b = 2.0f * glm::dot(oc, ray.direction);
    float c = glm::dot(oc, oc) - sphere.radius * sphere.radius;
    float discriminant = b * b - 4 * a * c;

    if (discriminant < 0) return result;

    float t = (-b - std::sqrt(discriminant)) / (2.0f * a);
    if (t < 0) t = (-b + std::sqrt(discriminant)) / (2.0f * a);
    if (t < 0) return result;

    result.hit = true;
    result.distance = t;
    result.point = ray.origin + ray.direction * t;
    result.normal = glm::normalize(result.point - sphere.center);
    return result;
}

HitResult RayVsTriangle(const Ray& ray, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2) {
    // Moller-Trumbore intersection
    HitResult result;
    constexpr float EPSILON = 1e-6f;

    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 h = glm::cross(ray.direction, edge2);
    float a = glm::dot(edge1, h);

    if (a > -EPSILON && a < EPSILON) return result;

    float f = 1.0f / a;
    glm::vec3 s = ray.origin - v0;
    float u = f * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f) return result;

    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(ray.direction, q);
    if (v < 0.0f || u + v > 1.0f) return result;

    float t = f * glm::dot(edge2, q);
    if (t <= EPSILON) return result;

    result.hit = true;
    result.distance = t;
    result.point = ray.origin + ray.direction * t;
    result.normal = glm::normalize(glm::cross(edge1, edge2));
    return result;
}

glm::vec3 ClosestPointOnSegment(const glm::vec3& point, const glm::vec3& a, const glm::vec3& b) {
    glm::vec3 ab = b - a;
    float t = glm::dot(point - a, ab) / glm::dot(ab, ab);
    t = std::clamp(t, 0.0f, 1.0f);
    return a + t * ab;
}

glm::vec3 ClosestPointOnAABB(const glm::vec3& point, const AABB& box) {
    return glm::clamp(point, box.min, box.max);
}

glm::vec3 ResolveAABBOverlap(const AABB& moving, const AABB& stationary) {
    // Find minimum translation vector to push 'moving' out of 'stationary'
    glm::vec3 overlap;
    overlap.x = std::min(moving.max.x - stationary.min.x, stationary.max.x - moving.min.x);
    overlap.y = std::min(moving.max.y - stationary.min.y, stationary.max.y - moving.min.y);
    overlap.z = std::min(moving.max.z - stationary.min.z, stationary.max.z - moving.min.z);

    glm::vec3 center_diff = moving.Center() - stationary.Center();
    glm::vec3 resolution(0.0f);

    if (overlap.x <= overlap.y && overlap.x <= overlap.z) {
        resolution.x = (center_diff.x > 0 ? 1.0f : -1.0f) * overlap.x;
    } else if (overlap.y <= overlap.x && overlap.y <= overlap.z) {
        resolution.y = (center_diff.y > 0 ? 1.0f : -1.0f) * overlap.y;
    } else {
        resolution.z = (center_diff.z > 0 ? 1.0f : -1.0f) * overlap.z;
    }

    return resolution;
}

glm::vec3 ResolveSphereVsAABB(const Sphere& sphere, const AABB& box) {
    glm::vec3 closest = ClosestPointOnAABB(sphere.center, box);
    glm::vec3 diff = sphere.center - closest;
    float dist = glm::length(diff);

    if (dist < 0.0001f) {
        // Sphere center inside AABB, push out via shortest axis
        return ResolveAABBOverlap(
            AABB{sphere.center - glm::vec3(sphere.radius), sphere.center + glm::vec3(sphere.radius)},
            box
        );
    }

    float penetration = sphere.radius - dist;
    if (penetration <= 0) return glm::vec3(0.0f);

    return glm::normalize(diff) * penetration;
}

} // namespace physics
