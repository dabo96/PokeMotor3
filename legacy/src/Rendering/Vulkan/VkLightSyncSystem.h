#pragma once

#include "Systems.h"
#include "Registry.h"
#include "CommonComponents.h"
#include "Rendering/Vulkan/Core/VulkanContext.h"

#include <glm/glm.hpp>

namespace pokemotor::vk {

// Bridges ECS light entities to the renderer's light list. Each frame it reads
// every entity that has both a TransformComponent and a LightComponent and
// pushes its parameters (plus the world-space position from the transform) into
// VulkanContext via the cached rendererHandle. Register the system AFTER
// TransformSystem so worldMatrix is current; its writes land in
// VulkanContext::m_lightDescs, which BeginFrame re-packs into the GPU buffer.
//
// This is the Phase-2 sync that lets lights live as entities: move/edit the
// entity and the light follows, with the renderer still owning the GPU buffer.
class LightSyncSystem : public ecs::System {
public:
    explicit LightSyncSystem(VulkanContext* ctx) : mCtx(ctx) {}

    void Update(ecs::Registry& registry, float dt) override {
        (void)dt;
        if (!mCtx) return;
        // LightComponent is the primary (smaller) pool, so we iterate only lights.
        for (auto [entity, light, transform]
             : registry.GetView<ecs::LightComponent, ecs::TransformComponent>()) {
            if (light.rendererHandle == 0xFFFFFFFFu) continue;
            VulkanContext::LightDesc d{};
            d.type         = light.type;
            d.position     = glm::vec3(transform.worldMatrix[3]);  // world position
            d.color        = light.color;
            d.intensity    = light.intensity;
            d.range        = light.range;
            d.direction    = light.direction;
            d.innerDegrees = light.innerDegrees;
            d.outerDegrees = light.outerDegrees;
            d.radius       = light.radius;
            mCtx->UpdateLight(light.rendererHandle, d);
        }
    }

private:
    VulkanContext* mCtx = nullptr;
};

}  // namespace pokemotor::vk
