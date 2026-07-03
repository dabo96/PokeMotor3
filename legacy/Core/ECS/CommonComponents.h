#pragma once
#include "Entity.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <memory>
#include <vector>

class Model;
class LODModel;
namespace anim { class Animator; }

namespace ecs {

struct TransformComponent {
    // Local-space values (relative to parent, or world if no parent)
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    // World-space matrix (computed by TransformSystem, or GetMatrix() for root entities)
    glm::mat4 worldMatrix{1.0f};
    bool worldDirty = true; // set true when position/rotation/scale change

    glm::mat4 GetMatrix() const;
    glm::vec3 GetWorldPosition() const { return glm::vec3(worldMatrix[3]); }
};

struct RenderComponent {
    Model* model = nullptr;
    LODModel* lodModel = nullptr; // Alternative: LOD model
    std::string modelPath;        // Path used to load the model (for serialization)
    std::string primitiveDesc;    // e.g. "cube:0.15", "sphere:0.5:32:16:0.85:0.55:0.45", "plane:30:30:0.35:0.55:0.3"
    bool visible = true;
    bool castShadow = true;
    bool isFoliage = false;
};

struct AnimationComponent {
    std::shared_ptr<anim::Animator> animator;
    int currentClip = -1;
    bool playing = false;
    bool loop = true;
};

struct TagComponent {
    std::string name;
    uint32_t layer = 0; // Bitmask for collision/rendering layers
};

struct SpriteComponent {
    // Texture
    unsigned int textureID = 0;
    std::string texturePath;

    // Display
    glm::vec2 size{1.0f, 1.0f};
    glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    float alphaClip = 0.5f;
    bool visible = true;

    // Billboard mode
    enum class BillboardMode : uint8_t {
        Full = 0,    // Spherical — always face camera
        AxisY = 1,   // Cylindrical — rotate around Y only
        None = 2     // No billboarding, use transform rotation
    };
    BillboardMode billboardMode = BillboardMode::Full;

    // Material (written to G-Buffer for deferred lighting)
    float metallic = 0.0f;
    float roughness = 0.9f;
    bool castShadow = true;
    bool unlit = true;       // Bypass PBR lighting, VXGI, and cel shading
    bool flipX = false;      // Mirror sprite horizontally
    bool flipY = false;      // Mirror sprite vertically

    // Sprite sheet / atlas
    glm::ivec2 sheetGridSize{1, 1};
    int currentFrame = 0;

    // Animation
    bool animated = false;
    float frameRate = 12.0f;
    int frameStart = 0;
    int frameEnd = 0;
    bool looping = true;
    float frameAccumulator = 0.0f;
};

struct EmissiveComponent {
    glm::vec3 color{0.0f};
    float intensity = 5.0f;
};

struct ParticleEmitterConfig {
    glm::vec3 direction{0.0f, 1.0f, 0.0f};
    float spread = 0.5f;
    float minSpeed = 1.0f;
    float maxSpeed = 3.0f;
    float minLifetime = 0.5f;
    float maxLifetime = 2.0f;
    float startSize = 0.1f;
    float endSize = 0.0f;
    glm::vec4 startColor{1.0f};
    glm::vec4 endColor{1.0f, 1.0f, 1.0f, 0.0f};
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    float emitRate = 50.0f;
    int maxParticles = 1000;
    bool loop = true;
};

struct ParticleEmitterComponent {
    int emitterID = -1;
    ParticleEmitterConfig config;
    bool needsRegistration = true;
};

struct SSSComponent {
    float strength = 0.5f;               // wrap lighting amount (0 = off, 1 = full wrap)
    glm::vec3 tintColor{1.0f, 0.2f, 0.1f}; // scattered light color (warm red for skin)
};

struct DissolveComponent {
    float amount = 0.0f;             // 0 = fully visible, 1 = fully dissolved
    glm::vec3 edgeColor{1.0f, 0.4f, 0.1f}; // glow color at dissolve edge
    float edgeWidth = 0.05f;         // width of the emissive edge band
    float noiseScale = 3.0f;         // UV scale for noise sampling
    bool active = false;             // set true to start dissolving
    float speed = 1.0f;              // dissolve speed (units per second)
    bool reverse = false;            // true = materializing, false = dissolving
};

struct SDFComponent {
    unsigned int sdfTexture = 0;   // 3D texture handle
    glm::vec3 aabbMin{0.0f};      // local-space AABB
    glm::vec3 aabbMax{1.0f};
    int resolution = 64;
    bool generated = false;
};

struct PointLightComponent {
    glm::vec3 color{1.0f};
    float intensity = 10.0f;
};

struct SpotLightComponent {
    glm::vec3 color{1.0f};
    float intensity = 10.0f;
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    float cutOff = 0.9063f;       // cos(25°)
    float outerCutOff = 0.8192f;  // cos(35°)
};

// Unified punctual light driven through the ECS (Phase 2 of the ECS plan).
// The entity's TransformComponent supplies the world-space position; this
// component carries the rest. A LightSyncSystem pushes it into the renderer
// every frame via `rendererHandle` (returned by VulkanContext::Register*Light
// when the entity is spawned). Supersedes the legacy Point/SpotLightComponent
// above (those stay until the Phase 6 prune). type: 0 = point, 1 = spot, 2 = area.
struct LightComponent {
    int       type           = 0;
    glm::vec3 color          = glm::vec3(1.0f);
    float     intensity      = 8.0f;
    float     range          = 6.0f;
    glm::vec3 direction      = glm::vec3(0.0f, -1.0f, 0.0f);  // spot only
    float     innerDegrees   = 18.0f;                          // spot only
    float     outerDegrees   = 28.0f;                          // spot only
    float     radius         = 0.5f;                           // area only
    uint32_t  rendererHandle = 0xFFFFFFFFu;                    // index into VulkanContext's light list
};

// The single directional sun, now a selectable/editable entity (Unreal-style:
// pick it in the hierarchy and change its colour/direction). main() copies this
// into the DirectionalLight passed to BeginFrame each frame, so CSM/shadows
// follow. Direction is edited directly here (not derived from the transform).
struct DirectionalLightComponent {
    glm::vec3 direction = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));
    glm::vec3 color     = glm::vec3(1.0f, 0.97f, 0.92f);
    glm::vec3 ambient   = glm::vec3(0.05f, 0.06f, 0.08f);
};

struct AudioSourceComponent {
    std::string clipName;         // Name of the loaded AudioClip
    float volume = 1.0f;
    bool loop = false;
    bool playOnStart = false;     // Auto-play when entity enters scene
    bool playing = false;         // Runtime state
    uint32_t soundHandle = 0;     // Handle from AudioManager
};

struct ThirdPersonCameraComponent {
    float distance = 8.0f;
    float minDistance = 2.0f;
    float maxDistance = 20.0f;
    float heightOffset = 2.0f;
    float smoothSpeed = 8.0f;
    float mouseSensitivity = 0.15f;
    float scrollSensitivity = 2.0f;
    float minPitch = -60.0f;
    float maxPitch = 75.0f;
    float fov = 45.0f;
    float collisionOffset = 0.3f;
};

struct DialogueComponent {
    std::string speakerName = "NPC";
    std::vector<std::string> lines;
};

struct ZoneTransitionComponent {
    std::string targetScene;
    glm::vec3 spawnPosition{0.0f, 5.0f, 0.0f};
    std::string targetSpawnId;        // Named spawn point (preferred over raw spawnPosition)
    int transitionType = 0;           // 0=Fade, 1=Wipe, 2=Slide
    float transitionDuration = 0.5f;
};

struct SpawnPointComponent {
    std::string spawnId;
};

struct ParentComponent {
    Entity parent = NULL_ENTITY;
};

struct ChildrenComponent {
    std::vector<Entity> children;
};

} // namespace ecs
