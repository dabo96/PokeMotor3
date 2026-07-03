#include "CollisionSystem.h"
#include "../ECS/CommonComponents.h"
#include "../Events/EventBus.h"
#include "../Events/GameEvents.h"
#include <algorithm>

namespace physics {

void CollisionSystem::Update(ecs::Registry& registry, float dt) {
    IntegratePhysics(registry, dt);
    TerrainCollision(registry);
    mCollisions.clear();
    BroadPhase(registry);
    NarrowPhase(registry);

    // Emit collision events
    for (auto& pair : mCollisions) {
        events::EventBus::Instance().Emit(events::CollisionEvent{
            pair.entityA, pair.entityB, pair.contactPoint, pair.normal
        });
    }
}

void CollisionSystem::IntegratePhysics(ecs::Registry& registry, float dt) {
    for (auto [entity, transform, rb] : registry.GetView<ecs::TransformComponent, RigidBodyComponent>()) {
        // grounded is re-evaluated every frame by terrain + collision contacts;
        // clearing it here means walking off an edge correctly resumes falling.
        rb.grounded = false;
        // Gravity is applied unconditionally — contact resolution kills the
        // into-surface velocity so resting is stable on ANY static collider
        // (floors/platforms/ramps), not just heightmap terrain.
        if (rb.useGravity) {
            rb.velocity.y += rb.gravity * dt;
        }

        rb.velocity += rb.acceleration * dt;
        transform.position += rb.velocity * dt;
    }
}

void CollisionSystem::TerrainCollision(ecs::Registry& registry) {
    if (!mTerrain) return;

    for (auto [entity, transform, rb] : registry.GetView<ecs::TransformComponent, RigidBodyComponent>()) {
        float footY = transform.position.y;

        // If entity has a collider, offset foot position
        if (registry.HasComponent<ColliderComponent>(entity)) {
            auto& col = registry.GetComponent<ColliderComponent>(entity);
            footY = transform.position.y + col.offset.y;
            if (col.type == ColliderComponent::Type::Capsule)
                footY -= col.height * 0.5f;
            else if (col.type == ColliderComponent::Type::AABB)
                footY -= col.halfExtents.y;
            else
                footY -= col.radius;
        }

        auto height = mTerrain->GetHeightAt(transform.position.x, transform.position.z);
        if (height.has_value()) {
            float terrainY = *height;
            float entityBottom = footY;
            float diff = terrainY - entityBottom;

            if (diff > -rb.groundedThreshold) {
                // On or below terrain
                transform.position.y += std::max(0.0f, diff);
                if (rb.velocity.y < 0) rb.velocity.y = 0;
                rb.grounded = true;
            } else {
                rb.grounded = false;
            }
        }
    }
}

void CollisionSystem::BroadPhase(ecs::Registry& registry) {
    mBroadPairs.clear();

    // Collect all collidable entities with AABBs
    struct ColEntry {
        ecs::Entity entity;
        AABB aabb;
        uint32_t layer;
        uint32_t mask;
    };

    std::vector<ColEntry> entries;
    for (auto [entity, transform, collider] :
         registry.GetView<ecs::TransformComponent, ColliderComponent>()) {
        AABB aabb = GetWorldAABB(collider, transform.position);
        entries.push_back({entity, aabb, collider.layer, collider.mask});
    }

    // Brute force O(n^2) — fine for <500 entities
    for (size_t i = 0; i < entries.size(); ++i) {
        for (size_t j = i + 1; j < entries.size(); ++j) {
            // Layer check
            if (!(entries[i].layer & entries[j].mask) &&
                !(entries[j].layer & entries[i].mask))
                continue;

            if (TestAABBvsAABB(entries[i].aabb, entries[j].aabb)) {
                mBroadPairs.push_back({entries[i].entity, entries[j].entity});
            }
        }
    }
}

void CollisionSystem::NarrowPhase(ecs::Registry& registry) {
    for (auto& [entityA, entityB] : mBroadPairs) {
        auto& transformA = registry.GetComponent<ecs::TransformComponent>(entityA);
        auto& transformB = registry.GetComponent<ecs::TransformComponent>(entityB);
        auto& colA = registry.GetComponent<ColliderComponent>(entityA);
        auto& colB = registry.GetComponent<ColliderComponent>(entityB);

        // Use sphere tests for narrow phase when applicable
        bool collides = false;
        glm::vec3 contactPoint, normal;
        float penetration = 0.0f;

        if (colA.type == ColliderComponent::Type::Sphere &&
            colB.type == ColliderComponent::Type::Sphere) {
            Sphere sa = GetWorldSphere(colA, transformA.position);
            Sphere sb = GetWorldSphere(colB, transformB.position);
            if (TestSphereVsSphere(sa, sb)) {
                collides = true;
                glm::vec3 diff = sa.center - sb.center;
                float dist = glm::length(diff);
                normal = dist > 0.001f ? diff / dist : glm::vec3(0, 1, 0);
                penetration = (sa.radius + sb.radius) - dist;
                contactPoint = sb.center + normal * sb.radius;
            }
        } else {
            // AABB vs AABB fallback
            AABB aabbA = GetWorldAABB(colA, transformA.position);
            AABB aabbB = GetWorldAABB(colB, transformB.position);
            if (TestAABBvsAABB(aabbA, aabbB)) {
                collides = true;
                glm::vec3 resolution = ResolveAABBOverlap(aabbA, aabbB);
                normal = glm::normalize(resolution);
                penetration = glm::length(resolution);
                contactPoint = (aabbA.Center() + aabbB.Center()) * 0.5f;
            }
        }

        if (!collides) continue;

        // Check if either is a trigger
        if (colA.isTrigger || colB.isTrigger) {
            events::EventBus::Instance().Emit(events::TriggerEvent{
                colA.isTrigger ? entityA : entityB,
                colA.isTrigger ? entityB : entityA,
                true
            });
            continue;
        }

        mCollisions.push_back({entityA, entityB, contactPoint, normal, penetration});

        // Resolve: push entities apart
        bool hasRbA = registry.HasComponent<RigidBodyComponent>(entityA);
        bool hasRbB = registry.HasComponent<RigidBodyComponent>(entityB);

        if (hasRbA && !hasRbB) {
            transformA.position += normal * penetration;
        } else if (!hasRbA && hasRbB) {
            transformB.position -= normal * penetration;
        } else if (hasRbA && hasRbB) {
            transformA.position += normal * (penetration * 0.5f);
            transformB.position -= normal * (penetration * 0.5f);
        }

        // Velocity response: remove the component of each RigidBody's velocity
        // that drives it INTO the contact, so bodies rest/slide instead of
        // sinking and jittering. `normal` points the way A is pushed out (so it's
        // A's surface normal; B's is the opposite). An upward contact also flags
        // grounded for gameplay (jump checks, gravity-aware movement).
        auto respond = [](RigidBodyComponent& rb, const glm::vec3& surfaceNormal) {
            float vn = glm::dot(rb.velocity, surfaceNormal);
            if (vn < 0.0f) rb.velocity -= surfaceNormal * vn;   // cancel into-surface motion
            if (surfaceNormal.y > 0.5f) rb.grounded = true;     // standing on it
        };
        if (hasRbA) respond(registry.GetComponent<RigidBodyComponent>(entityA),  normal);
        if (hasRbB) respond(registry.GetComponent<RigidBodyComponent>(entityB), -normal);
    }
}

AABB CollisionSystem::GetWorldAABB(const ColliderComponent& collider, const glm::vec3& position) const {
    glm::vec3 center = position + collider.offset;
    glm::vec3 half;

    if (collider.type == ColliderComponent::Type::Sphere) {
        half = glm::vec3(collider.radius);
    } else if (collider.type == ColliderComponent::Type::Capsule) {
        half = glm::vec3(collider.radius, collider.height * 0.5f, collider.radius);
    } else {
        half = collider.halfExtents;
    }

    return AABB{center - half, center + half};
}

Sphere CollisionSystem::GetWorldSphere(const ColliderComponent& collider, const glm::vec3& position) const {
    return Sphere{position + collider.offset, collider.radius};
}

HitResult CollisionSystem::Raycast(ecs::Registry& registry, const Ray& ray, float maxDist,
                                    uint32_t layerMask, ecs::Entity ignore) const {
    HitResult best;
    best.distance = maxDist;

    // Test terrain first
    if (mTerrain) {
        auto terrainHit = mTerrain->Raycast(ray, maxDist);
        if (terrainHit.hit && terrainHit.distance < best.distance) {
            best = terrainHit;
        }
    }

    // Test entity colliders
    auto* pool = registry.GetPool<ColliderComponent>();
    if (!pool) return best;

    for (size_t i = 0; i < pool->Size(); ++i) {
        ecs::Entity entity = pool->GetEntity(i);
        if (entity == ignore) continue;

        auto& col = pool->GetByIndex(i);
        if (!(col.layer & layerMask)) continue;

        if (!registry.HasComponent<ecs::TransformComponent>(entity)) continue;
        auto& transform = registry.GetComponent<ecs::TransformComponent>(entity);

        HitResult hit;
        if (col.type == ColliderComponent::Type::Sphere) {
            Sphere s = GetWorldSphere(col, transform.position);
            hit = RayVsSphere(ray, s);
        } else {
            AABB aabb = GetWorldAABB(col, transform.position);
            hit = RayVsAABB(ray, aabb);
        }

        if (hit.hit && hit.distance < best.distance) {
            best = hit;
        }
    }

    return best;
}

HitResult CollisionSystem::RaycastTerrain(const Ray& ray, float maxDist) const {
    if (!mTerrain) return HitResult{};
    return mTerrain->Raycast(ray, maxDist);
}

} // namespace physics
