#include "TransformSystem.h"
#include "Registry.h"
#include "CommonComponents.h"

namespace ecs {

void TransformSystem::Update(Registry& registry, float dt) {
    (void)dt;

    // First pass: update all root entities (no parent)
    // Then recursively update children
    auto* transformPool = registry.GetPool<TransformComponent>();
    if (!transformPool) return;

    for (size_t i = 0; i < transformPool->Size(); i++) {
        Entity entity = transformPool->GetEntity(i);
        if (!registry.IsAlive(entity)) continue;

        // Skip children — they'll be updated by their parent
        if (registry.HasComponent<ParentComponent>(entity)) continue;

        auto& transform = transformPool->GetByIndex(i);
        transform.worldMatrix = transform.GetMatrix();

        // Update children recursively
        if (registry.HasComponent<ChildrenComponent>(entity)) {
            auto& children = registry.GetComponent<ChildrenComponent>(entity);
            for (Entity child : children.children) {
                if (registry.IsAlive(child)) {
                    UpdateEntityRecursive(registry, child, transform.worldMatrix);
                }
            }
        }
    }
}

void TransformSystem::UpdateEntityRecursive(Registry& registry, Entity entity,
                                             const glm::mat4& parentWorld) {
    if (!registry.HasComponent<TransformComponent>(entity)) return;

    auto& transform = registry.GetComponent<TransformComponent>(entity);
    glm::mat4 localMatrix = transform.GetMatrix();
    transform.worldMatrix = parentWorld * localMatrix;

    // Recurse into children
    if (registry.HasComponent<ChildrenComponent>(entity)) {
        auto& children = registry.GetComponent<ChildrenComponent>(entity);
        for (Entity child : children.children) {
            if (registry.IsAlive(child)) {
                UpdateEntityRecursive(registry, child, transform.worldMatrix);
            }
        }
    }
}

} // namespace ecs
