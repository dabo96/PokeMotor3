#pragma once

#include "Systems.h"
#include "Registry.h"
#include "CommonComponents.h"
#include "Rendering/Vulkan/VkComponents.h"
#include "Rendering/Vulkan/Core/VulkanContext.h"

#include <glm/glm.hpp>

namespace pokemotor::vk {

// Phase 3 sync systems: push each sprite / particle-emitter entity's world
// transform into the renderer every frame, so moving the entity moves the
// sprite/emitter. The renderer still owns the SSBO / emitter table; entities
// hold only the handle. Register both AFTER TransformSystem (fresh worldMatrix).

class SpriteSyncSystem : public ecs::System {
public:
    explicit SpriteSyncSystem(VulkanContext* ctx) : mCtx(ctx) {}

    void Update(ecs::Registry& registry, float dt) override {
        (void)dt;
        if (!mCtx) return;
        for (auto [entity, sprite, transform]
             : registry.GetView<SpriteComponentVk, ecs::TransformComponent>()) {
            if (sprite.rendererHandle == 0xFFFFFFFFu) continue;
            VulkanContext::SpriteDesc d{};
            d.model        = transform.worldMatrix;  // position/scale from the entity
            d.color        = sprite.color;
            d.alphaClip    = sprite.alphaClip;
            d.metallic     = sprite.metallic;
            d.roughness    = sprite.roughness;
            d.textureIndex = sprite.textureIndex;
            mCtx->UpdateSprite(sprite.rendererHandle, d);
        }
    }

private:
    VulkanContext* mCtx = nullptr;
};

class ParticleEmitterSyncSystem : public ecs::System {
public:
    explicit ParticleEmitterSyncSystem(VulkanContext* ctx) : mCtx(ctx) {}

    void Update(ecs::Registry& registry, float dt) override {
        (void)dt;
        if (!mCtx) return;
        for (auto [entity, emitter, transform]
             : registry.GetView<ParticleEmitterComponentVk, ecs::TransformComponent>()) {
            if (emitter.rendererHandle == 0xFFFFFFFFu) continue;
            VulkanContext::ParticleEmitterDesc d{};
            d.position     = glm::vec3(transform.worldMatrix[3]);  // spawn origin from the entity
            d.direction    = emitter.direction;
            d.spread       = emitter.spread;
            d.minSpeed     = emitter.minSpeed;
            d.maxSpeed     = emitter.maxSpeed;
            d.minLifetime  = emitter.minLifetime;
            d.maxLifetime  = emitter.maxLifetime;
            d.startSize    = emitter.startSize;
            d.endSize      = emitter.endSize;
            d.startColor   = emitter.startColor;
            d.gravity      = emitter.gravity;
            d.emitRate     = emitter.emitRate;
            d.maxParticles = emitter.maxParticles;
            mCtx->UpdateParticleEmitter(emitter.rendererHandle, d);
        }
    }

private:
    VulkanContext* mCtx = nullptr;
};

}  // namespace pokemotor::vk
