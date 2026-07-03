#pragma once
#include "Shapes.h"
#include "TerrainCollider.h"
#include "../ECS/Entity.h"
#include "../ECS/Registry.h"
#include "../ECS/Systems.h"
#include <vector>
#include <functional>
#include <memory>

namespace physics {

// Collision layers (bitmask)
enum CollisionLayer : uint32_t {
    LAYER_DEFAULT    = 1 << 0,
    LAYER_PLAYER     = 1 << 1,
    LAYER_NPC        = 1 << 2,
    LAYER_POKEMON    = 1 << 3,
    LAYER_TERRAIN    = 1 << 4,
    LAYER_TRIGGER    = 1 << 5,
    LAYER_STATIC     = 1 << 6,
    LAYER_ALL        = 0xFFFFFFFF
};

// ECS Physics components
struct ColliderComponent {
    enum class Type { AABB, Sphere, Capsule } type = Type::AABB;
    glm::vec3 offset{0.0f};
    glm::vec3 halfExtents{0.5f}; // For AABB
    float radius = 0.5f;         // For Sphere/Capsule
    float height = 1.0f;         // For Capsule
    bool isTrigger = false;
    uint32_t layer = LAYER_DEFAULT;
    uint32_t mask = LAYER_ALL;   // Which layers this collides with
};

struct RigidBodyComponent {
    glm::vec3 velocity{0.0f};
    glm::vec3 acceleration{0.0f};
    float gravity = -20.0f;
    bool useGravity = true;
    bool grounded = false;
    float groundedThreshold = 0.1f;
};

struct CollisionPair {
    ecs::Entity entityA;
    ecs::Entity entityB;
    glm::vec3 contactPoint;
    glm::vec3 normal;
    float penetration;
};

class CollisionSystem : public ecs::System {
public:
    void Update(ecs::Registry& registry, float dt) override;

    // Terrain
    void SetTerrain(std::shared_ptr<TerrainCollider> terrain) { mTerrain = terrain; }
    TerrainCollider* GetTerrain() { return mTerrain.get(); }

    // Raycast against all colliders
    HitResult Raycast(ecs::Registry& registry, const Ray& ray, float maxDist = 1000.0f,
                      uint32_t layerMask = LAYER_ALL, ecs::Entity ignore = ecs::NULL_ENTITY) const;

    // Raycast against terrain only
    HitResult RaycastTerrain(const Ray& ray, float maxDist = 1000.0f) const;

    // Get collisions from last frame
    const std::vector<CollisionPair>& GetCollisions() const { return mCollisions; }

private:
    void IntegratePhysics(ecs::Registry& registry, float dt);
    void TerrainCollision(ecs::Registry& registry);
    void BroadPhase(ecs::Registry& registry);
    void NarrowPhase(ecs::Registry& registry);

    AABB GetWorldAABB(const ColliderComponent& collider, const glm::vec3& position) const;
    Sphere GetWorldSphere(const ColliderComponent& collider, const glm::vec3& position) const;

    std::shared_ptr<TerrainCollider> mTerrain;
    std::vector<CollisionPair> mCollisions;
    std::vector<std::pair<ecs::Entity, ecs::Entity>> mBroadPairs;
};

} // namespace physics
