#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace pokemotor::vk {

// ECS component (lives in Registry alongside ecs::TransformComponent).
// Points at a contiguous slice of meshes inside VulkanContext::m_meshes —
// every mesh in [firstMesh, firstMesh + meshCount) is drawn with the owning
// entity's world matrix, composed with the mesh's own local transform.
struct RenderComponentVk {
    uint32_t firstMesh = 0;
    uint32_t meshCount = 0;
    bool     visible   = true;
};

// Single global directional light for FASE 4 forward shading. Will move into
// the ECS as a real component once we add point/spot lights later.
struct DirectionalLight {
    glm::vec3 direction = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));
    glm::vec3 color     = glm::vec3(1.0f, 0.97f, 0.92f);
    glm::vec3 ambient   = glm::vec3(0.05f, 0.06f, 0.08f);
};

// Sprites and particle emitters as entities (Phase 3), now owning their full
// params (Phase 4a) so the ECS is the source of truth and the renderer is just a
// cache the sync-system refreshes each frame. This makes them editable in the
// inspector and serializable. `rendererHandle`/`textureIndex` are RUNTIME-only
// (re-derived on load), so the serializer skips them. Position/model come from
// the entity's TransformComponent, not stored here.
struct SpriteComponentVk {
    std::string texturePath;                                 // serialized; re-registered on load
    glm::vec4   color          = glm::vec4(1.0f);
    glm::vec4   uvOffsetScale  = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    float       alphaClip      = 0.5f;
    float       metallic       = 0.0f;
    float       roughness      = 0.6f;
    uint32_t    textureIndex   = 0;            // runtime: resolved from texturePath
    uint32_t    rendererHandle = 0xFFFFFFFFu;  // runtime: sprite SSBO row
};

struct ParticleEmitterComponentVk {
    glm::vec3 direction    = glm::vec3(0.0f, 1.0f, 0.0f);
    float     spread       = 0.5f;
    float     minSpeed     = 1.0f;
    float     maxSpeed     = 3.0f;
    float     minLifetime  = 0.5f;
    float     maxLifetime  = 2.0f;
    float     startSize    = 0.1f;
    float     endSize      = 0.0f;
    glm::vec4 startColor   = glm::vec4(1.0f);
    glm::vec3 gravity      = glm::vec3(0.0f, -9.81f, 0.0f);
    float     emitRate     = 50.0f;
    int       maxParticles = 1000;             // fixed after register (slot allocation)
    uint32_t  rendererHandle = 0xFFFFFFFFu;    // runtime: emitter table index
};

}  // namespace pokemotor::vk
