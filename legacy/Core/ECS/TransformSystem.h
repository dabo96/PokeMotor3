#pragma once
#include "Systems.h"
#include "Entity.h"
#include <glm/glm.hpp>

namespace ecs {

class TransformSystem : public System {
public:
    void Update(Registry& registry, float dt) override;

private:
    void UpdateEntityRecursive(Registry& registry, Entity entity, const glm::mat4& parentWorld);
};

} // namespace ecs
