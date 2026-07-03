// PokeMotor — Vulkan renderer entry point.

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "Camera.h"
#include "Registry.h"
#include "World.h"
#include "Physics/CollisionSystem.h"
#include "ScriptEngine.h"
#include "ScriptSystem.h"
#include "ScriptComponent.h"
#include "CommonComponents.h"
#include "TransformSystem.h"
#include "Rendering/Vulkan/Core/VulkanContext.h"
#include "Rendering/Vulkan/VkComponents.h"
#include "Rendering/Vulkan/VkLightSyncSystem.h"
#include "Rendering/Vulkan/VkSceneSyncSystems.h"
#include "Rendering/Vulkan/Core/VkSceneSerializer.h"
#include "Rendering/Vulkan/Core/VkSceneManager.h"

// F17 — non-render module smoke test
#include "Audio/AudioManager.h"
#include <sol/sol.hpp>

// F16-final — FluentUI Context + Renderer + our Vulkan backend
#include "Rendering/Vulkan/UI/VulkanBackend.h"
#include "core/RenderBackend.h"
#include "core/Context.h"
#include "core/Renderer.h"
#include "UI/Widgets.h"
#include "Theme/FluentTheme.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>  // glm::quat, glm::eulerAngles, glm::angleAxis

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {

constexpr bool kValidationEnabled =
#ifdef POKEMOTOR_VK_VALIDATION
    true;
#else
    false;
#endif

void anchorWorkingDirectoryToExecutable() {
    const char* base = SDL_GetBasePath();
    if (!base) return;
    std::error_code ec;
    std::filesystem::current_path(base, ec);
}

void handleKeyboard(Camera& cam, float dt) {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (keys[SDL_SCANCODE_W]) cam.ProcessKeyboard(FORWARD,  dt);
    if (keys[SDL_SCANCODE_S]) cam.ProcessKeyboard(BACKWARD, dt);
    if (keys[SDL_SCANCODE_A]) cam.ProcessKeyboard(LEFT,     dt);
    if (keys[SDL_SCANCODE_D]) cam.ProcessKeyboard(RIGHT,    dt);
    if (keys[SDL_SCANCODE_SPACE])  cam.ProcessKeyboard(UP,   dt);
    if (keys[SDL_SCANCODE_LCTRL])  cam.ProcessKeyboard(DOWN, dt);
    if (keys[SDL_SCANCODE_Q]) cam.ProcessKeyboard(ROLL_LEFT,  dt);
    if (keys[SDL_SCANCODE_E]) cam.ProcessKeyboard(ROLL_RIGHT, dt);
}

// ---- Gizmo picking / drag math ----------------------------------------------

// Builds a world-space ray from a mouse position inside the viewport panel rect.
// Returns origin (camera pos) and normalized direction.
void mouseRay(float mx, float my, float cx, float cy, float cw, float ch,
              const glm::mat4& view, const glm::mat4& proj, const glm::vec3& camPos,
              glm::vec3& outOrigin, glm::vec3& outDir) {
    float u = (mx - cx) / cw;            // 0..1 across the panel
    float v = (my - cy) / ch;
    float ndcX = u * 2.0f - 1.0f;
    float ndcY = v * 2.0f - 1.0f;        // panel y is top-down; Vulkan proj already Y-flips,
                                         // so NDC y here matches the rendered image directly
    glm::mat4 invVP = glm::inverse(proj * view);
    glm::vec4 pNear = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 pFar  = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    glm::vec3 wNear = glm::vec3(pNear) / pNear.w;
    glm::vec3 wFar  = glm::vec3(pFar)  / pFar.w;
    outOrigin = camPos;
    outDir    = glm::normalize(wFar - wNear);
}

// Closest parameter `s` along the line (axisOrigin + s*axisDir) to the given ray.
float closestSOnAxis(const glm::vec3& axisOrigin, const glm::vec3& axisDir,
                     const glm::vec3& rayOrigin, const glm::vec3& rayDir) {
    glm::vec3 w0 = axisOrigin - rayOrigin;
    float a = glm::dot(axisDir, axisDir);
    float b = glm::dot(axisDir, rayDir);
    float c = glm::dot(rayDir, rayDir);
    float d = glm::dot(axisDir, w0);
    float e = glm::dot(rayDir, w0);
    float denom = a * c - b * b;
    return (glm::abs(denom) > 1e-6f) ? (b * e - c * d) / denom : 0.0f;
}

// Projects a world point to panel pixel coords. Returns false if behind camera.
bool worldToPanel(const glm::vec3& p, const glm::mat4& view, const glm::mat4& proj,
                  float cx, float cy, float cw, float ch, glm::vec2& outPix) {
    glm::vec4 clip = proj * view * glm::vec4(p, 1.0f);
    if (clip.w <= 0.0f) return false;
    glm::vec3 ndc = glm::vec3(clip) / clip.w;
    outPix.x = cx + (ndc.x * 0.5f + 0.5f) * cw;
    outPix.y = cy + (ndc.y * 0.5f + 0.5f) * ch;
    return true;
}

// Distance from point p to segment ab (all 2D).
float distToSeg2D(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b) {
    glm::vec2 ab = b - a; float t = glm::dot(p - a, ab) / glm::max(glm::dot(ab, ab), 1e-6f);
    t = glm::clamp(t, 0.0f, 1.0f);
    return glm::length(p - (a + ab * t));
}

// Ray vs AABB (slab). Returns true + entry distance tHit (>=0) on hit. rayDir
// need not be normalized; tHit is in rayDir units.
bool rayAABB(const glm::vec3& ro, const glm::vec3& rd,
             const glm::vec3& bmin, const glm::vec3& bmax, float& tHit) {
    float tmin = 0.0f, tmax = 1e30f;
    for (int a = 0; a < 3; ++a) {
        float inv = 1.0f / rd[a];
        float t0 = (bmin[a] - ro[a]) * inv;
        float t1 = (bmax[a] - ro[a]) * inv;
        if (inv < 0.0f) { float tmp = t0; t0 = t1; t1 = tmp; }
        tmin = t0 > tmin ? t0 : tmin;
        tmax = t1 < tmax ? t1 : tmax;
        if (tmax <= tmin) return false;
    }
    tHit = tmin;
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    anchorWorkingDirectoryToExecutable();

    SDL_Window* window = SDL_CreateWindow(
        "PokeMotor",
        1280, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    pokemotor::vk::VulkanContext ctx;
    // F17.A — smoke test AudioManager. No clips loaded for now; the manager
    // opens its SDL audio device and stays idle. Real clip playback proves
    // the pipeline when Pokemon game scripts wire SFX/music.
    audio::AudioManager::Instance().Init();
    std::printf("[F17] AudioManager Init OK (master vol %.2f)\n",
                audio::AudioManager::Instance().GetMasterVolume());

    // F17.B — smoke test Lua via sol2. Minimal: open libs, run a script,
    // read back a value. Full ScriptEngine port (with ECS / Input / Physics
    // bindings) is deferred — those submodules need their own Vulkan path
    // first. This confirms the core Lua subsystem links and runs.
    {
        sol::state lua;
        lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string);
        try {
            lua.script(R"(
                function vk_smoke()
                    return "PokeMotorVk + Lua " .. _VERSION
                end
                print("[F17] Lua: " .. vk_smoke())
            )");
        } catch (const sol::error& e) {
            std::fprintf(stderr, "[F17] Lua error: %s\n", e.what());
        }
    }

    // F17.C — ECS smoke test confirmation. The Registry + TransformSystem
    // are already exercised by the cart spawn block below; an explicit log
    // makes the smoke result visible in the console.
    std::printf("[F17] ECS Registry available (cart spawn block follows)\n");

    if (!ctx.Initialize(window, kValidationEnabled)) {
        std::fprintf(stderr, "VulkanContext init failed\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Live pointers to the render-effect toggles/params for the Inspector's
    // Render Settings panel. Pointers target ctx's members — valid for the
    // whole lifetime of ctx, so fetching once here is enough.
    auto rs = ctx.GetRenderSettingsRefs();

    // Unified Hierarchy selection: a single index into one list of all scene
    // objects (entities first, then lights — lights ARE scene objects too). -1 =
    // nothing selected → the Inspector shows the Render Settings. The index maps
    // to a type below: [0 .. sceneEntities.size()-1] are entities, the rest are
    // lights (subtract the entity count). One index makes selection naturally
    // exclusive — no mutual-exclusion bookkeeping needed.
    int selectedItem = -1;
    struct SceneEntity { ecs::Entity entity; std::string name; };
    std::vector<SceneEntity> sceneEntities;
    std::string saveStatus;  // shown in the Inspector after a File→Save

    // Gizmo drag state. draggedAxis: -1 none, 0/1/2 = X/Y/Z. We pick/drag using
    // the PREVIOUS frame's camera + panel rect (computed after input each frame),
    // which lags one frame — imperceptible for a click, and the camera doesn't
    // move while dragging anyway.
    int       draggedAxis   = -1;
    float     dragStartS    = 0.0f;     // param along the axis at grab time
    glm::vec3 dragStartPos  = glm::vec3(0.0f);  // object position at grab time
    glm::mat4 prevView      = glm::mat4(1.0f);
    glm::mat4 prevProj      = glm::mat4(1.0f);
    float     prevCenterX = 0, prevCenterY = 0, prevCenterW = 0, prevCenterH = 0;
    bool      prevValid     = false;

    // ---- ECS scene -------------------------------------------------------
    // One entity = one CoffeeCart instance pointing at the whole mesh slice.
    // Three instances at different positions demonstrate Registry.View<>
    // iteration; transform hierarchy will exercise once a parent/child setup
    // joins later in the migration.
    // The World owns the Registry and the system scheduler (Phase 1 of the ECS
    // plan). We bind a `registry` reference so the existing call sites below stay
    // unchanged, and register TransformSystem with the scheduler so World::Update
    // runs it each frame (replacing the old manual transformSystem.Update call).
    ecs::World world;
    ecs::Registry& registry = world.GetRegistry();
    // Lua scripting (Phase 5a): init the engine, point it at the registry, and
    // register the system FIRST so scripts can set velocities the physics step
    // then integrates. ScriptSystem::Init wires the hot-reload callback.
    scripting::ScriptEngine::Instance().Init();
    scripting::ScriptEngine::Instance().SetRegistry(&registry);
    world.AddSystem<scripting::ScriptSystem>().Init(registry);
    // Physics next: it integrates RigidBodies + resolves collisions (writing
    // transform.position); TransformSystem then propagates the updated positions
    // into world matrices the same frame. Does nothing until entities get
    // Collider/RigidBody components — ready for the demo.
    world.AddSystem<physics::CollisionSystem>();
    world.AddSystem<ecs::TransformSystem>();
    // LightSyncSystem runs after TransformSystem (so worldMatrix is fresh) and
    // pushes each light entity's params into the renderer before BeginFrame packs
    // them. Lights live as entities (Phase 2); the renderer still owns the buffer.
    world.AddSystem<pokemotor::vk::LightSyncSystem>(&ctx);
    // Sprites and particle emitters are entities too (Phase 3); their sync
    // systems push the entity's world transform into the renderer each frame.
    world.AddSystem<pokemotor::vk::SpriteSyncSystem>(&ctx);
    world.AddSystem<pokemotor::vk::ParticleEmitterSyncSystem>(&ctx);
    const uint32_t coffeeCartMeshCount = ctx.MeshCount();

    // Ground plane — gives the shadow pass something to fall on. Y=0 lives at
    // the foot of the carts; we drop it a hair below to avoid Z-fighting with
    // any near-zero geometry the model might carry.
    pokemotor::vk::MeshCPU plane;
    {
        const float s = 30.0f;
        plane.vertices = {
            {{-s, 0.0f, -s}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
            {{ s, 0.0f, -s}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
            {{ s, 0.0f,  s}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
            {{-s, 0.0f,  s}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
        };
        // CCW winding when viewed from +Y (above). The geometry pipeline has
        // back-face culling on, so the OLD order {0,1,2,0,2,3} produced a
        // face normal of (0,-1,0) — visible only from BELOW. From above the
        // ground was culled and we saw the lighting shader's no-geometry
        // sky fallback (dark navy) instead of the gray ground.
        plane.indices = { 0, 2, 1, 0, 3, 2 };
    }
    uint32_t planeMeshIdx = ctx.RegisterMesh(plane, glm::vec4(0.55f, 0.55f, 0.58f, 1.0f));

    // Unit cube (half-extent 0.5) for the physics demo. 6 faces × 4 verts so each
    // face gets a flat outward normal; CCW winding from outside (front-face).
    pokemotor::vk::MeshCPU cube;
    {
        const float h = 0.5f;
        struct Face { glm::vec3 n; glm::vec3 v[4]; };
        const Face faces[6] = {
            {{ 1,0,0}, {{ h,-h, h},{ h,-h,-h},{ h, h,-h},{ h, h, h}}},  // +X
            {{-1,0,0}, {{-h,-h,-h},{-h,-h, h},{-h, h, h},{-h, h,-h}}},  // -X
            {{0, 1,0}, {{-h, h, h},{ h, h, h},{ h, h,-h},{-h, h,-h}}},  // +Y
            {{0,-1,0}, {{-h,-h,-h},{ h,-h,-h},{ h,-h, h},{-h,-h, h}}},  // -Y
            {{0,0, 1}, {{-h,-h, h},{ h,-h, h},{ h, h, h},{-h, h, h}}},  // +Z
            {{0,0,-1}, {{ h,-h,-h},{-h,-h,-h},{-h, h,-h},{ h, h,-h}}},  // -Z
        };
        for (const auto& f : faces) {
            uint32_t base = static_cast<uint32_t>(cube.vertices.size());
            for (int i = 0; i < 4; ++i)
                cube.vertices.push_back({ f.v[i], f.n, glm::vec2(0.0f) });
            cube.indices.insert(cube.indices.end(),
                                { base, base + 1, base + 2, base, base + 2, base + 3 });
        }
    }
    uint32_t cubeMeshIdx = ctx.RegisterMesh(cube, glm::vec4(0.90f, 0.45f, 0.15f, 1.0f), 0.0f, 0.4f);

    auto spawnCart = [&](const glm::vec3& pos, float yawRadians) {
        ecs::Entity e = registry.CreateEntity();
        auto& t = registry.AddComponent<ecs::TransformComponent>(e);
        t.position = pos;
        t.rotation = glm::angleAxis(yawRadians, glm::vec3(0.0f, 1.0f, 0.0f));
        t.scale    = glm::vec3(1.0f);

        auto& r = registry.AddComponent<pokemotor::vk::RenderComponentVk>(e);
        r.firstMesh = 0;
        r.meshCount = coffeeCartMeshCount;
        return e;
    };
    sceneEntities.push_back({ spawnCart({  0.0f, 0.0f,  0.0f}, 0.0f),                "Cart 0 (center)" });
    sceneEntities.push_back({ spawnCart({ -2.5f, 0.0f, -1.5f}, glm::radians( 35.0f)), "Cart 1 (left)" });
    sceneEntities.push_back({ spawnCart({  2.5f, 0.0f, -1.5f}, glm::radians(-35.0f)), "Cart 2 (right)" });

    // Light showcase — one of every punctual type, now spawned as ECS ENTITIES
    // (Phase 2). Each gets a TransformComponent (position) + LightComponent, is
    // registered with the renderer to obtain a handle, and is added to the
    // hierarchy. The LightSyncSystem pushes each entity's params to the renderer
    // every frame, so moving/editing the entity moves/edits the light.
    //   * point lights : small, hard speculars (warm above center, cool left).
    //   * spot light   : cone aimed down at the right cart.
    //   * area lights  : soft, WIDE speculars via the representative-point path.
    auto spawnLight = [&](const glm::vec3& pos, ecs::LightComponent lc, const char* name) {
        ecs::Entity e = registry.CreateEntity();
        registry.AddComponent<ecs::TransformComponent>(e).position = pos;
        switch (lc.type) {
            case 1: lc.rendererHandle = ctx.RegisterSpotLight(pos, lc.direction, lc.color,
                        lc.intensity, lc.range, lc.innerDegrees, lc.outerDegrees); break;
            case 2: lc.rendererHandle = ctx.RegisterAreaLight(pos, lc.color, lc.intensity,
                        lc.range, lc.radius); break;
            default: lc.rendererHandle = ctx.RegisterPointLight(pos, lc.color, lc.intensity,
                        lc.range); break;
        }
        registry.AddComponent<ecs::LightComponent>(e) = lc;
        sceneEntities.push_back({ e, name });
        return e;
    };
    {
        ecs::LightComponent l; l.type = 0; l.color = {1.0f, 0.80f, 0.55f}; l.intensity = 8.0f; l.range = 6.0f;
        spawnLight({ 0.0f, 2.2f, 1.0f }, l, "Point Light (warm)");
    }
    {
        ecs::LightComponent l; l.type = 0; l.color = {0.45f, 0.6f, 1.0f}; l.intensity = 6.0f; l.range = 5.0f;
        spawnLight({ -3.0f, 1.6f, 0.0f }, l, "Point Light (cool)");
    }
    {
        ecs::LightComponent l; l.type = 1; l.direction = {0.0f, -1.0f, 0.0f}; l.color = {1.0f, 1.0f, 0.9f};
        l.intensity = 12.0f; l.range = 7.0f; l.innerDegrees = 18.0f; l.outerDegrees = 28.0f;
        spawnLight({ 2.5f, 3.0f, -1.5f }, l, "Spot Light");
    }
    {
        ecs::LightComponent l; l.type = 2; l.color = {1.0f, 0.68f, 0.40f}; l.intensity = 10.0f; l.range = 7.0f; l.radius = 0.65f;
        spawnLight({ 0.0f, 2.8f, 1.4f }, l, "Area Light (warm)");
    }
    {
        ecs::LightComponent l; l.type = 2; l.color = {0.40f, 0.70f, 1.0f}; l.intensity = 7.0f; l.range = 6.0f; l.radius = 0.50f;
        spawnLight({ 3.6f, 1.3f, 1.2f }, l, "Area Light (cool)");
    }

    // The directional sun as a selectable entity (Unreal-style). Its
    // DirectionalLightComponent holds the real values; the loop copies them into
    // the `sun` passed to BeginFrame each frame, so editing it in the inspector
    // updates the lighting + CSM shadows. Component defaults already match the
    // legacy sun, so the look is unchanged.
    ecs::Entity sunEntity = registry.CreateEntity();
    registry.AddComponent<ecs::TransformComponent>(sunEntity);
    registry.AddComponent<ecs::DirectionalLightComponent>(sunEntity);
    sceneEntities.push_back({ sunEntity, "Directional Light (Sun)" });

    // Particles + sprite showcase.
    {
        // Emitters spawn as entities: the transform drives the spawn origin via
        // ParticleEmitterSyncSystem; the renderer still owns the particle buffer.
        auto spawnEmitter = [&](const pokemotor::vk::VulkanContext::ParticleEmitterDesc& desc,
                                const char* name) {
            ecs::Entity e = registry.CreateEntity();
            registry.AddComponent<ecs::TransformComponent>(e).position = desc.position;
            // The component owns the params (source of truth); copy the desc in,
            // then register to reserve slots + get the runtime handle.
            pokemotor::vk::ParticleEmitterComponentVk ec;
            ec.direction = desc.direction;   ec.spread      = desc.spread;
            ec.minSpeed  = desc.minSpeed;    ec.maxSpeed     = desc.maxSpeed;
            ec.minLifetime = desc.minLifetime; ec.maxLifetime = desc.maxLifetime;
            ec.startSize = desc.startSize;   ec.endSize      = desc.endSize;
            ec.startColor = desc.startColor; ec.gravity      = desc.gravity;
            ec.emitRate  = desc.emitRate;    ec.maxParticles = desc.maxParticles;
            ec.rendererHandle = ctx.RegisterParticleEmitter(desc);
            registry.AddComponent<pokemotor::vk::ParticleEmitterComponentVk>(e) = ec;
            sceneEntities.push_back({ e, name });
            return e;
        };

        // Slow warm steam rising off the center cart (soft particles fade where
        // they meet geometry). Slow, so its motion-blur stretch stays subtle.
        pokemotor::vk::VulkanContext::ParticleEmitterDesc steam{};
        steam.position    = glm::vec3(0.0f, 1.6f, 0.0f);
        steam.direction   = glm::vec3(0.0f, 1.0f, 0.0f);
        steam.spread      = 0.35f;
        steam.minSpeed    = 1.2f;
        steam.maxSpeed    = 2.0f;
        steam.minLifetime = 1.5f;
        steam.maxLifetime = 2.5f;
        steam.startSize   = 0.08f;
        steam.endSize     = 0.25f;
        steam.startColor  = glm::vec4(1.0f, 0.85f, 0.55f, 0.85f);
        steam.gravity     = glm::vec3(0.0f, 0.4f, 0.0f);
        steam.emitRate    = 60.0f;
        steam.maxParticles = 256;
        spawnEmitter(steam, "Steam Emitter");

        // Fast warm sparks shooting up off the left cart. High speed + short life
        // so the particle MOTION BLUR stretches them into clear streaks (the slow
        // steam barely stretches; these make the effect obvious).
        pokemotor::vk::VulkanContext::ParticleEmitterDesc sparks{};
        sparks.position    = glm::vec3(-2.5f, 1.3f, -1.5f);
        sparks.direction   = glm::vec3(0.0f, 1.0f, 0.0f);
        sparks.spread      = 0.45f;
        sparks.minSpeed    = 6.0f;
        sparks.maxSpeed    = 11.0f;
        sparks.minLifetime = 0.4f;
        sparks.maxLifetime = 0.9f;
        sparks.startSize   = 0.05f;
        sparks.endSize     = 0.0f;
        sparks.startColor  = glm::vec4(1.0f, 0.5f, 0.15f, 1.0f);
        sparks.gravity     = glm::vec3(0.0f, -7.0f, 0.0f);
        sparks.emitRate    = 45.0f;
        sparks.maxParticles = 220;
        spawnEmitter(sparks, "Sparks Emitter");
    }

    {
        // Bulbasaur sprite floating over the center cart. With sprite cast
        // shadows it now drops a cut-out silhouette onto the ground/carts.
        // NOTE: atlas animation (sheetCols/Rows + frameRate) needs an actual
        // sprite SHEET; 001.png is a single frame, so it stays static here.
        // Point sd.textureIndex at a multi-frame sheet and set frameRate>0 to
        // see the animation path.
        // The sprite is an entity (Phase 3): its TransformComponent (position +
        // scale) drives the model matrix via SpriteSyncSystem, so the gizmo can
        // move it like anything else. The transform reproduces the old
        // translate*scale exactly, so the look is unchanged.
        ecs::Entity spriteE = registry.CreateEntity();
        auto& st = registry.AddComponent<ecs::TransformComponent>(spriteE);
        st.position = glm::vec3(0.0f, 2.5f, -0.5f);
        st.scale    = glm::vec3(1.5f);

        const uint32_t spriteTex = ctx.RegisterSpriteTexture("Assets/Models/Sprites/001.png");
        pokemotor::vk::VulkanContext::SpriteDesc sd{};
        sd.model         = st.GetMatrix();  // initial; SpriteSyncSystem overwrites each frame
        sd.color         = glm::vec4(1.0f);
        sd.uvOffsetScale = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        sd.alphaClip     = 0.5f;
        sd.metallic      = 0.0f;
        sd.roughness     = 0.6f;
        sd.textureIndex  = spriteTex;
        // The component owns the params (source of truth) for editing/serialization.
        pokemotor::vk::SpriteComponentVk sc;
        sc.texturePath   = "Assets/Models/Sprites/001.png";
        sc.color         = sd.color;
        sc.uvOffsetScale = sd.uvOffsetScale;
        sc.alphaClip     = sd.alphaClip;
        sc.metallic      = sd.metallic;
        sc.roughness     = sd.roughness;
        sc.textureIndex  = spriteTex;
        sc.rendererHandle = ctx.RegisterSprite(sd);
        registry.AddComponent<pokemotor::vk::SpriteComponentVk>(spriteE) = sc;
        // Phase 5a demo: attach a Lua script so the sprite bobs. Edit
        // Assets/Scripts/Bob.lua while running to see hot-reload. Remove to detach.
        registry.AddComponent<scripting::ScriptComponent>(spriteE).scriptPath =
            "Assets/Scripts/Bob.lua";
        sceneEntities.push_back({ spriteE, "Sprite (Bulbasaur)" });

        // Showcase the ported hierarchy: parent the sprite to the center cart
        // (sceneEntities[0]). The cart sits at the origin so the sprite looks
        // identical at rest, but now follows the cart when it moves — and the
        // parenting round-trips through save/load (parentIndex). Remove this line
        // to detach. The sprite's transform position is now LOCAL to the cart.
        if (!sceneEntities.empty())
            world.SetParent(spriteE, sceneEntities[0].entity);
    }

    // Ground entity — single quad. Y=0 sits at the carts' wheel base; the
    // plane lives at y=-0.02 to keep depth fighting away from the casters.
    {
        ecs::Entity ground = registry.CreateEntity();
        auto& t = registry.AddComponent<ecs::TransformComponent>(ground);
        t.position = glm::vec3(0.0f, -0.02f, 0.0f);
        auto& r = registry.AddComponent<pokemotor::vk::RenderComponentVk>(ground);
        r.firstMesh = planeMeshIdx;
        r.meshCount = 1;
        // Static floor collider (no RigidBody): a thick slab whose TOP sits at the
        // ground plane, so falling RigidBodies rest on the visible surface.
        auto& gc = registry.AddComponent<physics::ColliderComponent>(ground);
        gc.type        = physics::ColliderComponent::Type::AABB;
        gc.halfExtents = glm::vec3(30.0f, 1.0f, 30.0f);
        gc.offset      = glm::vec3(0.0f, -1.0f, 0.0f);  // top at position.y (≈ -0.02)
        // Register inside the scope: `ground` is local to this block.
        sceneEntities.push_back({ ground, "Ground" });
    }

    // Physics demo: a cube dropped from above that falls under gravity and rests
    // on the ground collider (showcases P4's collision response). Remove this
    // block when you build your own demo. It serializes like anything else.
    {
        ecs::Entity cube = registry.CreateEntity();
        auto& t = registry.AddComponent<ecs::TransformComponent>(cube);
        t.position = glm::vec3(1.5f, 5.0f, 1.5f);  // drops in from above
        auto& r = registry.AddComponent<pokemotor::vk::RenderComponentVk>(cube);
        r.firstMesh = cubeMeshIdx;
        r.meshCount = 1;
        auto& col = registry.AddComponent<physics::ColliderComponent>(cube);
        col.type        = physics::ColliderComponent::Type::AABB;
        col.halfExtents = glm::vec3(0.5f);
        registry.AddComponent<physics::RigidBodyComponent>(cube);  // gravity on by default
        // Phase 5c demo: drive the cube with WASD from a Lua script (reads input,
        // sets rigidbody velocity). Remove when you build your own demo.
        registry.AddComponent<scripting::ScriptComponent>(cube).scriptPath =
            "Assets/Scripts/Player.lua";
        sceneEntities.push_back({ cube, "Physics Cube (demo)" });
    }

    // Give every scene object a TagComponent name so the serializer can write a
    // self-describing scene from the registry alone (derived once from the UI
    // list — no need to touch each spawn site). Names round-trip on load.
    for (auto& se : sceneEntities)
        registry.AddComponent<ecs::TagComponent>(se.entity).name = se.name;

    // Rebuilds the editor's entity list from the registry — used after a scene
    // Load replaces every entity. Order follows the TagComponent pool.
    auto rebuildSceneList = [&]() {
        sceneEntities.clear();
        for (auto [e, tag] : registry.GetView<ecs::TagComponent>())
            sceneEntities.push_back({ e, tag.name });
        selectedItem = -1;
    };

    // Scene manager owns deferred scene switching over the serializer. Register
    // the editable scene file (and scan a scenes folder if present) so switches
    // go through one safe, frame-boundary path. ChangeScene() requests; the loop
    // applies it via ProcessPending().
    pokemotor::vk::SceneManager::Instance().Init(&world, &ctx);
    pokemotor::vk::SceneManager::Instance().RegisterScene("scene", "scene.json");
    pokemotor::vk::SceneManager::Instance().ScanSceneDirectory("Assets/Scenes");

    // ---- Scripting workflow (Phase 5b) -------------------------------------
    // Which external editor "Open in editor" launches, and helpers to launch it
    // and to scaffold a new script from a template. Paths are cwd-relative
    // ("Assets/Scripts/"), which is where the runtime loads scripts from.
    int editorChoice = 0;  // 0 = VSCode, 1 = Notepad
    const std::vector<std::string> editorNames = { "VSCode", "Notepad" };
    auto openScriptInEditor = [&](const std::string& path) {
        const char* exe = (editorChoice == 0) ? "code" : "notepad";
        // `start` launches detached and returns immediately (no engine freeze).
        std::string cmd = std::string("start \"\" ") + exe + " \"" + path + "\"";
        std::system(cmd.c_str());
    };
    auto createNewScript = [&]() -> std::string {
        namespace fs = std::filesystem;
        std::error_code ec; fs::create_directories("Assets/Scripts", ec);
        std::string full;
        for (int i = 1; i < 10000; ++i) {
            full = "Assets/Scripts/Script" + std::to_string(i) + ".lua";
            if (!fs::exists(full, ec)) break;
        }
        std::ofstream f(full);
        f << "local Script = {}\n\n"
             "function Script:OnCreate()\n"
             "end\n\n"
             "function Script:OnUpdate(dt)\n"
             "    -- local tr = world:getTransform(self.entity)\n"
             "    -- if tr then tr.position = Vec3(tr.position.x, tr.position.y, tr.position.z) end\n"
             "end\n\n"
             "return Script\n";
        return full;
    };

    Camera camera(glm::vec3(0.0f, 1.5f, 4.0f));
    camera.MovementSpeed = 4.0f;

    // F16-final.B — FluentUI uses its OWN Vulkan backend in *shared* mode: it
    // reuses the engine's device/queue and records UI draws onto the engine's
    // command buffer (dynamic rendering, no swapchain of its own). Order:
    //   1. Build a VulkanSharedContext from the engine's handles.
    //   2. CreateContext(..., Vulkan, &shared) makes FluentUI build + init its
    //      shared backend (FluentUI owns it; DestroyContext deletes it).
    //   3. Hand that backend to the engine so the render-graph UI pass can give
    //      it the per-frame command buffer (SetFrameCommandBuffer).
    // `uiShared` only needs to outlive CreateContext — Init copies the handles
    // into the backend, so a local is fine.
    FluentUI::VulkanSharedContext uiShared = ctx.GetUISharedContext();
    FluentUI::UIContext* fluentCtx =
        FluentUI::CreateContext(window, FluentUI::RenderBackendType::Vulkan, &uiShared);
    if (!fluentCtx) {
        std::fprintf(stderr, "[F16] FluentUI::CreateContext failed\n");
        ctx.Shutdown();
        audio::AudioManager::Instance().Shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    ctx.SetUIBackend(FluentUI::GetBackend());
    std::printf("[F16] FluentUI Context created — shared Vulkan backend wired\n");

    // E1.C — Vortex Engine dark theme (palette + sizes baked into
    // GetEditorDarkStyle). Applied once after CreateContext; widgets read
    // through ctx->style every frame so a single assignment is enough.
    fluentCtx->style = FluentUI::GetEditorDarkStyle();

    // Load Lucide icon font. FluentUI's panel minimize button, menu
    // chevrons, etc. all use Lucide glyphs — without this, those widgets
    // either skip the glyph (panel button looks empty) or sample
    // uninitialized atlas slots producing colored splotches. The asset
    // lives in Assets/Fonts/lucide.ttf and is copied to <exe>/assets/fonts/
    // by CMake POST_BUILD.
    if (!fluentCtx->renderer.LoadIconFont("assets/fonts/lucide.ttf", 16)) {
        std::fprintf(stderr,
            "[E1] Lucide icon font not found at assets/fonts/lucide.ttf — "
            "icons (panel chevrons, menu glyphs) will not draw.\n");
    }

    // E1.B/C — Register the offscreen viewport color image as a FluentUI
    // texture so the editor center panel can sample it via FluentUI::Image.
    // The render graph keeps the image alive (createPostResources / on
    // recreateSwapchain it is recreated) but the descriptor set we allocate
    // here lives until the backend Shutdown destroys its pool. We don't
    // re-register on resize yet — that lands in E2 when the viewport image
    // can resize independently from the swapchain.
    void* viewportTexture = FluentUI::RegisterExternalTexture(
        static_cast<void*>(ctx.ViewportImage().View()));
    if (!viewportTexture) {
        std::fprintf(stderr, "[E1] RegisterExternalTexture failed for viewport color\n");
    }

    // UI callback runs INSIDE the render-graph UI pass. By the time we get
    // here, NewFrame() + the widget calls have already happened (in the
    // main loop body, below). The callback just emits the batched geometry.
    //
    // RenderDeferredDropdowns() is REQUIRED — menus, comboboxes, context
    // menus, and tooltips are queued during the widget pass and only
    // rendered (and their queues cleared) by this call. Without it
    // dropdowns never appear AND the queues grow unbounded. Same pattern
    // as FluentApp.cpp's main loop in the OpenGL example.
    ctx.SetUICallback([&]() {
        // Order matters (matches FluentApp's canonical loop): queue the
        // dropdown/menu/tooltip geometry FIRST, then Render() flushes
        // everything — including those deferred draws — to the backend in a
        // single EndFrame. Reversed, the dropdowns would be queued AFTER the
        // flush and never reach the GPU (so menus/comboboxes wouldn't appear).
        FluentUI::RenderDeferredDropdowns();
        FluentUI::Render();
    });

    // Buffer passed to BeginFrame; repopulated every frame from the sun entity's
    // DirectionalLightComponent (defaults already match the legacy values).
    pokemotor::vk::DirectionalLight sun{};

    // Editor vs Play mode. In Editor the LEFT button is free for selection /
    // gizmo dragging and the camera look is driven by HOLDING the RIGHT button
    // (+ WASD) — the same split the OpenGL editor used, so the left click no
    // longer hijacks the camera. Play mode captures the mouse continuously for
    // free FPS navigation; Esc returns to Editor. F5 toggles between the two.
    enum class EditorMode { Editor, Play };
    EditorMode mode = EditorMode::Editor;

    bool mouseCaptured = false;
    auto setMouseCapture = [&](bool capture) {
        if (capture && !mouseCaptured) {
            // The click that triggered camera capture went through FluentUI's
            // ProcessEvent and set mouseDown[0]=true. The matching BUTTON_UP
            // fires while we're in relative mode and gets dropped by the event
            // loop, so FluentUI would otherwise treat the button as held
            // forever — breaking hover/press state on every later widget.
            // Inject a synthetic release now.
            if (auto* uiCtx = FluentUI::GetContext()) {
                SDL_Event fakeUp{};
                fakeUp.type = SDL_EVENT_MOUSE_BUTTON_UP;
                fakeUp.button.button = SDL_BUTTON_LEFT;
                uiCtx->input.ProcessEvent(fakeUp);
            }
        }
        mouseCaptured = capture;
        SDL_SetWindowRelativeMouseMode(window, capture);
        if (!capture) {
            // SDL emits a synthetic MOUSE_MOTION at the warp-back position
            // when exiting relative mode, but that event lands in the queue
            // during the NEXT pump — not synchronously inside
            // SetWindowRelativeMouseMode. So we pump FIRST to materialize it,
            // then flush mouse events to drop it (and any other stale events
            // queued while we were in relative mode). Otherwise the synthetic
            // motion overrides mouseX/Y on the next ProcessEvent, breaking
            // hover hit-tests until the user manually moves the mouse.
            SDL_PumpEvents();
            SDL_FlushEvent(SDL_EVENT_MOUSE_MOTION);
            SDL_FlushEvent(SDL_EVENT_MOUSE_BUTTON_DOWN);
            SDL_FlushEvent(SDL_EVENT_MOUSE_BUTTON_UP);
            if (auto* uiCtx = FluentUI::GetContext()) {
                uiCtx->input.Update(window);
            }
        }
    };

    auto lastTime = std::chrono::high_resolution_clock::now();
    bool running = true;
    while (running) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        // ---- Phase 1: drain SDL events ----
        // ProcessEvent feeds FluentUI input. We defer the camera-capture
        // decision because at this point widgets haven't run yet — we
        // don't know whether the click landed on UI or 3D scene.
        // Track the BUTTON_DOWN coordinates so Phase 3 can test them
        // against the viewport rect instead of relying on FluentUI's
        // `mouseOverAnyWidget` flag (which only some widgets set).
        bool pendingLeftClick = false;
        bool pendingRightDown = false;
        float pendingClickX = 0.0f, pendingClickY = 0.0f;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            // While the camera owns the mouse (SDL relative mode), don't
            // forward mouse events to FluentUI — relative-mode motion
            // coordinates are NOT screen pixels and would corrupt the UI
            // input state (frozen mouseX/Y, possibly stuck mouseDown).
            const bool isMouseEvent =
                ev.type == SDL_EVENT_MOUSE_MOTION
             || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN
             || ev.type == SDL_EVENT_MOUSE_BUTTON_UP
             || ev.type == SDL_EVENT_MOUSE_WHEEL;
            if (auto* uiCtx = FluentUI::GetContext()) {
                if (!mouseCaptured || !isMouseEvent) {
                    uiCtx->input.ProcessEvent(ev);
                }
            }
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (ev.key.key == SDLK_S && (ev.key.mod & SDL_KMOD_CTRL)) {
                        // Ctrl+S — reliable save path independent of the menu UI.
                        bool ok = pokemotor::vk::SceneSerializer::Save(registry, "scene.json");
                        saveStatus = ok ? "Saved scene.json (see console for full path)"
                                        : "Scene save FAILED (see console)";
                    } else if (ev.key.key == SDLK_O && (ev.key.mod & SDL_KMOD_CTRL)) {
                        // Ctrl+O — request a scene switch (applied at frame start).
                        pokemotor::vk::SceneManager::Instance().ChangeScene("scene");
                    } else if (ev.key.key == SDLK_ESCAPE) {
                        if (mode == EditorMode::Play) {
                            mode = EditorMode::Editor;   // leave Play → editor
                            setMouseCapture(false);
                        } else if (mouseCaptured) {
                            setMouseCapture(false);      // release a right-drag look
                        } else {
                            running = false;
                        }
                    } else if (ev.key.key == SDLK_F5) {
                        // Toggle Editor <-> Play. Cancel any in-progress gizmo drag
                        // first: the drag-continue block runs every frame and is
                        // otherwise oblivious to the mode switch, so it would keep
                        // dragging the object around in Play.
                        draggedAxis = -1;
                        if (mode == EditorMode::Editor) {
                            mode = EditorMode::Play;
                            setMouseCapture(true);       // continuous FPS capture
                        } else {
                            mode = EditorMode::Editor;
                            setMouseCapture(false);
                        }
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    // Editor: LEFT defers to Phase 3 for selection / gizmo (no
                    // camera capture); RIGHT held drives the look camera. Play
                    // already owns the mouse, so no per-click handling there.
                    if (mode == EditorMode::Editor && !mouseCaptured) {
                        if (ev.button.button == SDL_BUTTON_LEFT) {
                            pendingLeftClick = true;  // defer until UI runs
                            pendingClickX = ev.button.x;
                            pendingClickY = ev.button.y;
                        } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                            pendingRightDown = true;  // defer viewport hit-test
                            pendingClickX = ev.button.x;
                            pendingClickY = ev.button.y;
                        }
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    // Releasing the right button ends the editor look camera.
                    if (mode == EditorMode::Editor && mouseCaptured &&
                        ev.button.button == SDL_BUTTON_RIGHT) {
                        setMouseCapture(false);
                    }
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (mouseCaptured) {
                        camera.ProcessMouseMovement(ev.motion.xrel, -ev.motion.yrel);
                    }
                    break;
                default:
                    break;
            }
        }

        // ---- Phase 2: run FluentUI widgets ----
        // Pin the renderer's viewport to the actual window size. Without this
        // call FluentUI's projection stays at the 800×600 default (Renderer.h),
        // so the projection doesn't match the swapchain — buttons render at a
        // different position than their hit-test rect, which is what made the
        // hover hit-test misalign vertically. SDL_GetWindowSize returns
        // logical window coords (matches what SDL events report for mouseX/Y),
        // which is exactly what the hit-tests compare against.
        if (auto* uiCtx = FluentUI::GetContext()) {
            int winW, winH;
            SDL_GetWindowSize(window, &winW, &winH);
            uiCtx->renderer.SetViewport(winW, winH);
        }

        // NewFrame copies last frame's mouseOverAnyWidget into LastFrame and
        // resets it. Buttons then re-set it if the cursor hovers them. After
        // this block, mouseOverAnyWidget reflects THIS frame's state — safe
        // to consult for the capture decision below.
        FluentUI::NewFrame(dt);

        // E1.C — Editor shell (Vortex layout): top menu bar, left Hierarchy,
        // right Inspector, bottom Assets, center viewport image. Sizes pulled
        // from project_vortex_engine_ui memory; this is the bare scaffold —
        // panel contents fill in over E2/E3/E4.
        int winW2, winH2;
        SDL_GetWindowSize(window, &winW2, &winH2);
        const float W = static_cast<float>(winW2);
        const float H = static_cast<float>(winH2);
        constexpr float kMenuH    = 28.0f;
        constexpr float kLeftW    = 280.0f;
        constexpr float kRightW   = 320.0f;
        constexpr float kBottomH  = 200.0f;
        const float centerY       = kMenuH;
        const float centerH       = std::max(0.0f, H - kMenuH - kBottomH);
        const float centerX       = kLeftW;
        const float centerW       = std::max(0.0f, W - kLeftW - kRightW);

        if (FluentUI::BeginMenuBar()) {
            if (FluentUI::BeginMenu("File")) {
                FluentUI::MenuItem("New Scene");
                if (FluentUI::MenuItem("Open..."))
                    pokemotor::vk::SceneManager::Instance().ChangeScene("scene");
                if (FluentUI::MenuItem("Save")) {
                    bool ok = pokemotor::vk::SceneSerializer::Save(registry, "scene.json");
                    saveStatus = ok ? "Saved scene.json (see console for full path)"
                                    : "Scene save FAILED (see console)";
                }
                FluentUI::MenuSeparator();
                if (FluentUI::MenuItem("Exit")) running = false;
                FluentUI::EndMenu();
            }
            if (FluentUI::BeginMenu("Edit")) {
                FluentUI::MenuItem("Undo");
                FluentUI::MenuItem("Redo");
                FluentUI::EndMenu();
            }
            if (FluentUI::BeginMenu("View")) {
                FluentUI::MenuItem("Hierarchy");
                FluentUI::MenuItem("Inspector");
                FluentUI::MenuItem("Assets");
                FluentUI::EndMenu();
            }
            if (FluentUI::BeginMenu("Help")) {
                FluentUI::MenuItem("About PokeMotor");
                FluentUI::EndMenu();
            }
            FluentUI::EndMenuBar();
        }

        // Left — Hierarchy
        if (FluentUI::BeginPanel("Hierarchy", FluentUI::Vec2(kLeftW, centerH),
                                  /*reserveLayoutSpace=*/false,
                                  std::nullopt, std::nullopt,
                                  FluentUI::Vec2(0.0f, centerY))) {
            // Every scene object IS an entity now (meshes, lights, the sun), so
            // the hierarchy is just the entity list — no more entity/light index
            // arithmetic. Selecting a row drives `selectedItem` into sceneEntities.
            FluentUI::Label("Scene");
            std::vector<std::string> items;
            items.reserve(sceneEntities.size());
            for (auto& se : sceneEntities) items.push_back(se.name);
            FluentUI::BeginListView("hierarchy-list",
                                    FluentUI::Vec2(0.0f, 0.0f),
                                    &selectedItem, items);
            FluentUI::EndListView();
            FluentUI::EndPanel();
        }

        // Right — Inspector (Render Settings). Edits ctx's effect members live
        // via the `rs` pointers. The list is long, so it lives inside a scroll
        // view that fills the panel below the title.
        if (FluentUI::BeginPanel("Inspector", FluentUI::Vec2(kRightW, centerH),
                                  /*reserveLayoutSpace=*/false,
                                  std::nullopt, std::nullopt,
                                  FluentUI::Vec2(W - kRightW, centerY))) {
          // Save feedback (File→Save has no toast; surface it here so the action
          // is visibly confirmed).
          if (!saveStatus.empty()) {
              FluentUI::Label(saveStatus);
              FluentUI::Separator();
          }
          // Every scene object is an entity; the inspector keys off its
          // components. -1 / out of range = nothing selected → Render Settings.
          const int nEnt = int(sceneEntities.size());
          if (selectedItem >= 0 && selectedItem < nEnt) {
            ecs::Entity e = sceneEntities[size_t(selectedItem)].entity;

            // Transform. Edits the live component; world.Update() (run after the
            // UI each frame) propagates it into worldMatrix via TransformSystem.
            if (registry.HasComponent<ecs::TransformComponent>(e)) {
                FluentUI::Label("Transform");
                auto& t = registry.GetComponent<ecs::TransformComponent>(e);
                FluentUI::DragFloat3("Position", &t.position.x, 0.05f);
                // Edit rotation as Euler degrees, recomputed from the quat each
                // frame (stable) and re-packed only when the user drags it.
                glm::vec3 euler = glm::degrees(glm::eulerAngles(t.rotation));
                if (FluentUI::DragFloat3("Rotation", &euler.x, 0.5f)) {
                    t.rotation = glm::quat(glm::radians(euler));
                }
                FluentUI::DragFloat3("Scale", &t.scale.x, 0.02f);
            }

            // Punctual light. Edits the LightComponent in place; LightSyncSystem
            // pushes it to the renderer each frame (position comes from Transform).
            if (registry.HasComponent<ecs::LightComponent>(e)) {
                FluentUI::Separator();
                FluentUI::Label("Light");
                auto& l = registry.GetComponent<ecs::LightComponent>(e);
                int t = l.type;
                const std::vector<std::string> types = { "Point", "Spot", "Area" };
                if (FluentUI::ComboBox("Type", &t, types)) l.type = t;
                FluentUI::DragFloat3("Color", &l.color.x, 0.01f, 0.0f, 1.0f);
                FluentUI::SliderFloat("Intensity", &l.intensity, 0.0f, 30.0f);
                FluentUI::SliderFloat("Range", &l.range, 0.1f, 30.0f);
                if (l.type == 1) {
                    FluentUI::DragFloat3("Direction", &l.direction.x, 0.02f, -1.0f, 1.0f);
                    FluentUI::SliderFloat("Inner angle", &l.innerDegrees, 1.0f, 80.0f);
                    FluentUI::SliderFloat("Outer angle", &l.outerDegrees, 1.0f, 90.0f);
                }
                if (l.type == 2) {
                    FluentUI::SliderFloat("Radius", &l.radius, 0.05f, 3.0f);
                }
            }

            // Directional sun. Edited here; the loop copies it into the
            // DirectionalLight BeginFrame reads (drives lighting + CSM).
            if (registry.HasComponent<ecs::DirectionalLightComponent>(e)) {
                FluentUI::Separator();
                FluentUI::Label("Directional Light");
                auto& dl = registry.GetComponent<ecs::DirectionalLightComponent>(e);
                FluentUI::DragFloat3("Direction", &dl.direction.x, 0.02f, -1.0f, 1.0f);
                FluentUI::DragFloat3("Color", &dl.color.x, 0.01f, 0.0f, 1.0f);
                FluentUI::DragFloat3("Ambient", &dl.ambient.x, 0.005f, 0.0f, 1.0f);
            }

            // Sprite. The component owns the params; SpriteSyncSystem pushes them.
            if (registry.HasComponent<pokemotor::vk::SpriteComponentVk>(e)) {
                FluentUI::Separator();
                FluentUI::Label("Sprite");
                auto& s = registry.GetComponent<pokemotor::vk::SpriteComponentVk>(e);
                FluentUI::DragFloat3("Color", &s.color.x, 0.01f, 0.0f, 1.0f);
                FluentUI::SliderFloat("Alpha clip", &s.alphaClip, 0.0f, 1.0f);
                FluentUI::SliderFloat("Metallic", &s.metallic, 0.0f, 1.0f);
                FluentUI::SliderFloat("Roughness", &s.roughness, 0.04f, 1.0f);
            }

            // Particle emitter. ParticleEmitterSyncSystem pushes these each frame
            // (maxParticles is fixed at spawn, so it's shown but not editable).
            if (registry.HasComponent<pokemotor::vk::ParticleEmitterComponentVk>(e)) {
                FluentUI::Separator();
                FluentUI::Label("Particle Emitter");
                auto& pe = registry.GetComponent<pokemotor::vk::ParticleEmitterComponentVk>(e);
                FluentUI::SliderFloat("Emit rate", &pe.emitRate, 0.0f, 200.0f);
                FluentUI::SliderFloat("Spread", &pe.spread, 0.0f, 1.0f);
                FluentUI::SliderFloat("Min speed", &pe.minSpeed, 0.0f, 20.0f);
                FluentUI::SliderFloat("Max speed", &pe.maxSpeed, 0.0f, 20.0f);
                FluentUI::DragFloat3("Gravity", &pe.gravity.x, 0.1f, -20.0f, 20.0f);
                FluentUI::DragFloat3("Start color", &pe.startColor.x, 0.01f, 0.0f, 1.0f);
            }

            // ---- Lua Script: the full create → edit → reload → run workflow ----
            FluentUI::Separator();
            FluentUI::Label("Script");
            if (!registry.HasComponent<scripting::ScriptComponent>(e)) {
                if (FluentUI::Button("Add Script Component"))
                    registry.AddComponent<scripting::ScriptComponent>(e);
            } else {
                auto& sc = registry.GetComponent<scripting::ScriptComponent>(e);

                // Pick the script from Assets/Scripts/*.lua (no path typing needed).
                std::vector<std::string> files = { "(none)" };
                {
                    namespace fs = std::filesystem;
                    std::error_code ec;
                    if (fs::is_directory("Assets/Scripts", ec))
                        for (auto& en : fs::directory_iterator("Assets/Scripts", ec))
                            if (en.is_regular_file() && en.path().extension() == ".lua")
                                files.push_back(en.path().filename().string());
                }
                int cur = 0;
                if (!sc.scriptPath.empty()) {
                    std::string curFile = std::filesystem::path(sc.scriptPath).filename().string();
                    for (int i = 1; i < int(files.size()); ++i)
                        if (files[size_t(i)] == curFile) { cur = i; break; }
                }
                if (FluentUI::ComboBox("File", &cur, files)) {
                    sc.scriptPath = (cur == 0) ? std::string()
                                              : ("Assets/Scripts/" + files[size_t(cur)]);
                    sc.initialized = false;  // re-instance with the new script
                }

                FluentUI::ComboBox("Editor", &editorChoice, editorNames);

                if (FluentUI::Button("New Script")) {
                    sc.scriptPath = createNewScript();
                    sc.initialized = false;
                    openScriptInEditor(sc.scriptPath);
                }
                if (FluentUI::Button("Open in editor") && !sc.scriptPath.empty())
                    openScriptInEditor(sc.scriptPath);
                if (FluentUI::Button("Reload / Compile"))
                    scripting::ScriptEngine::Instance().ReloadAll();
                if (FluentUI::Button("Remove Script")) {
                    registry.RemoveComponent<scripting::ScriptComponent>(e);  // sc dangles after — last use
                }
            }

            FluentUI::Separator();
            if (FluentUI::Button("Deselect")) selectedItem = -1;
          } else {
            FluentUI::Label("Render Settings");
            FluentUI::BeginScrollView("render-settings",
                                      FluentUI::Vec2(0.0f, 0.0f));
            if (FluentUI::CollapsingHeader("Shadows")) {
                FluentUI::Checkbox("PCSS soft shadows", rs.pcssEnabled);
                FluentUI::SliderFloat("PCSS light size", rs.pcssLightSize, 0.0f, 0.1f);
                FluentUI::Checkbox("CSM frustum-fit", rs.csmFrustumFit);
                FluentUI::Checkbox("Sprite shadows", rs.spriteShadows);
            }
            if (FluentUI::CollapsingHeader("Reflections (SSR)")) {
                FluentUI::Checkbox("SSR half-res", rs.ssrHalfRes);
            }
            if (FluentUI::CollapsingHeader("Material AA")) {
                FluentUI::Checkbox("Specular AA", rs.specularAA);
            }
            if (FluentUI::CollapsingHeader("Bloom")) {
                FluentUI::Checkbox("Enabled", rs.bloomEnabled);
                FluentUI::SliderFloat("Threshold", rs.bloomThreshold, 0.0f, 4.0f);
                FluentUI::SliderFloat("Knee", rs.bloomKnee, 0.0f, 1.0f);
                FluentUI::SliderFloat("Intensity", rs.bloomIntensity, 0.0f, 2.0f);
                FluentUI::SliderFloat("Upsample intensity", rs.bloomUpsampleIntensity, 0.0f, 2.0f);
            }
            if (FluentUI::CollapsingHeader("Volumetric Fog")) {
                FluentUI::Checkbox("Enabled", rs.fogEnabled);
                FluentUI::Checkbox("God rays", rs.godRays);
                FluentUI::Checkbox("Temporal", rs.fogTemporal);
                FluentUI::SliderFloat("Temporal alpha", rs.fogTemporalAlpha, 0.0f, 0.98f);
                FluentUI::SliderInt("Steps", rs.fogSteps, 4, 64);
                FluentUI::SliderFloat("Density floor", rs.fogDensityFloor, 0.0f, 0.05f, 200.0f, "%.4f");
                FluentUI::SliderFloat("Density scale", rs.fogDensityScale, 0.0f, 0.2f, 200.0f, "%.3f");
                FluentUI::SliderFloat("Height falloff", rs.fogHeightFactor, 0.0f, 1.0f);
                FluentUI::SliderFloat("Scatter", rs.fogScatterStrength, 0.0f, 4.0f);
                FluentUI::SliderFloat("Max distance", rs.fogMaxDistance, 1.0f, 200.0f);
                FluentUI::DragFloat3("Color", &rs.fogColor->x, 0.01f, 0.0f, 1.0f);  // glm::vec3 is 3 contiguous floats
            }
            if (FluentUI::CollapsingHeader("Tonemap / Grading")) {
                FluentUI::SliderFloat("Exposure", rs.exposure, -4.0f, 4.0f);
                FluentUI::SliderFloat("Contrast", rs.contrast, 0.5f, 2.0f);
                FluentUI::SliderFloat("Saturation", rs.saturation, 0.0f, 2.0f);
                FluentUI::Checkbox("Cel shading", rs.celEnabled);
                FluentUI::SliderFloat("Cel steps", rs.celSteps, 2.0f, 8.0f);
            }
            if (FluentUI::CollapsingHeader("Auto-exposure")) {
                FluentUI::Checkbox("Enabled", rs.autoExposureEnabled);
                FluentUI::SliderFloat("Key (gray)", rs.autoExposureKey, 0.05f, 0.6f);
                FluentUI::SliderFloat("Adapt time", rs.autoExposureTau, 0.1f, 4.0f);
                FluentUI::SliderFloat("Min lum", rs.autoExposureMinLum, 0.001f, 1.0f, 200.0f, "%.3f");
                FluentUI::SliderFloat("Max lum", rs.autoExposureMaxLum, 0.5f, 16.0f);
            }
            if (FluentUI::CollapsingHeader("Sharpening (CAS)")) {
                FluentUI::Checkbox("Enabled", rs.casEnabled);
                FluentUI::SliderFloat("Sharpness", rs.casSharpness, 0.0f, 1.0f);
            }
            if (FluentUI::CollapsingHeader("Particles")) {
                FluentUI::Checkbox("Enabled", rs.particlesEnabled);
                FluentUI::Checkbox("Soft particles", rs.softParticles);
                FluentUI::Checkbox("Motion blur", rs.particleMotionBlur);
                FluentUI::SliderFloat("Stretch scale", rs.particleStretchScale, 0.0f, 0.3f);
            }
            if (FluentUI::CollapsingHeader("Lights")) {
                FluentUI::Checkbox("Punctual lights", rs.pointLightsEnabled);
            }
            FluentUI::EndScrollView();
          }
            FluentUI::EndPanel();
        }

        // Bottom — Assets
        if (FluentUI::BeginPanel("Assets", FluentUI::Vec2(W, kBottomH),
                                  /*reserveLayoutSpace=*/false,
                                  std::nullopt, std::nullopt,
                                  FluentUI::Vec2(0.0f, H - kBottomH))) {
            FluentUI::Label("(asset browser TBD)");
            FluentUI::EndPanel();
        }

        // Center — viewport image. Absolute-positioned Image fills the gap
        // between the side panels and the menu/bottom bars. Once the
        // viewport image can resize independently from the swapchain (E2),
        // we'll resample uv1 to match the visible aspect — for now show the
        // whole image with linear filtering.
        if (viewportTexture && centerW > 0.0f && centerH > 0.0f) {
            FluentUI::Image("viewport", viewportTexture,
                            FluentUI::Vec2(centerW, centerH),
                            FluentUI::Vec2(0, 0), FluentUI::Vec2(1, 1),
                            FluentUI::Vec2(centerX, centerY));
        }

        // ---- Phase 3: resolve the deferred camera-capture decision ----
        // Click landed on the viewport image iff its coordinates are inside
        // the central rect computed above. Using the rect directly (instead
        // of FluentUI's `mouseOverAnyWidget`) sidesteps the per-widget
        // hover-flag inconsistency — only Button currently sets that flag
        // reliably; Panel/MenuBar/Image don't, so any click outside Button
        // would otherwise wrongly trigger camera capture.
        if (pendingLeftClick) {
            const bool insideViewport =
                pendingClickX >= centerX &&
                pendingClickX <  centerX + centerW &&
                pendingClickY >= centerY &&
                pendingClickY <  centerY + centerH;
            // Try grabbing a gizmo axis first (only if something is selected and we
            // have last frame's camera). If we grab an axis, DON'T capture camera.
            bool grabbed = false;
            if (insideViewport && prevValid && selectedItem >= 0) {
                // Resolve the selected entity's position (same mapping as the gizmo draw).
                const int nEnt = int(sceneEntities.size());
                glm::vec3 pos; bool ok = false;
                if (selectedItem < nEnt) {
                    ecs::Entity e = sceneEntities[size_t(selectedItem)].entity;
                    if (registry.HasComponent<ecs::TransformComponent>(e)) {
                        // World position (worldMatrix), so the gizmo sits correctly
                        // on parented children too — not just root entities.
                        pos = glm::vec3(registry.GetComponent<ecs::TransformComponent>(e).worldMatrix[3]); ok = true;
                    }
                }
                if (ok) {
                    const float s = 1.0f;  // axis length (must match the gizmo draw)
                    const glm::vec3 axes[3] = { {1,0,0},{0,1,0},{0,0,1} };
                    glm::vec2 mouse(pendingClickX, pendingClickY);
                    float bestDist = 18.0f;  // pixel pick threshold (generous grab)
                    int   bestAxis = -1;
                    glm::vec2 o2;
                    if (worldToPanel(pos, prevView, prevProj, prevCenterX, prevCenterY, prevCenterW, prevCenterH, o2)) {
                        for (int ax = 0; ax < 3; ++ax) {
                            glm::vec2 e2;
                            if (!worldToPanel(pos + axes[ax]*s, prevView, prevProj, prevCenterX, prevCenterY, prevCenterW, prevCenterH, e2)) continue;
                            float dd = distToSeg2D(mouse, o2, e2);
                            if (dd < bestDist) { bestDist = dd; bestAxis = ax; }
                        }
                    }
                    if (bestAxis >= 0) {
                        draggedAxis = bestAxis;
                        dragStartPos = pos;
                        glm::vec3 ro, rd;
                        mouseRay(pendingClickX, pendingClickY, prevCenterX, prevCenterY, prevCenterW, prevCenterH,
                                 prevView, prevProj, camera.Position, ro, rd);
                        dragStartS = closestSOnAxis(pos, axes[bestAxis], ro, rd);
                        grabbed = true;
                    }
                }
            }
            // A left click that didn't grab an axis must NOT capture the camera
            // (that was the old bug). Camera look lives on the RIGHT button now.
            // If no gizmo axis was grabbed, treat the left click as object
            // picking: cast a ray through the panel and select the nearest
            // entity whose world AABB it hits (skip the giant Ground quad).
            // Mesh entities pick per-triangle; mesh-less ones (sprite/light/emitter/
            // sun) pick via a small proxy box at their position.
            if (insideViewport && prevValid && !grabbed) {
                glm::vec3 ro, rd;
                mouseRay(pendingClickX, pendingClickY, prevCenterX, prevCenterY,
                         prevCenterW, prevCenterH, prevView, prevProj, camera.Position, ro, rd);
                int   bestEnt = -1;
                float bestT   = 1e30f;
                for (int ei = 0; ei < int(sceneEntities.size()); ++ei) {
                    if (sceneEntities[size_t(ei)].name == "Ground") continue;
                    ecs::Entity e = sceneEntities[size_t(ei)].entity;
                    if (!registry.HasComponent<ecs::TransformComponent>(e)) continue;
                    const glm::mat4& wm = registry.GetComponent<ecs::TransformComponent>(e).worldMatrix;

                    if (registry.HasComponent<pokemotor::vk::RenderComponentVk>(e)) {
                        const auto& rc = registry.GetComponent<pokemotor::vk::RenderComponentVk>(e);
                        // Per-mesh, two-phase pick: an AABB broad-phase to skip meshes
                        // the ray misses cheaply, then a per-TRIANGLE narrow-phase so
                        // the hit is the actual surface under the cursor — AABBs alone
                        // (even per-mesh) are too loose on rotated carts, so a click
                        // near the center cart would wrongly grab it. RaycastMesh seeds
                        // from bestT and only reports strictly-closer triangle hits.
                        for (uint32_t m = 0; m < rc.meshCount; ++m) {
                            glm::vec3 lo, hi;
                            if (!ctx.MeshAABB(rc.firstMesh + m, lo, hi)) continue;
                            glm::vec3 wmin( 1e30f), wmax(-1e30f);
                            for (int c = 0; c < 8; ++c) {
                                glm::vec3 corner((c & 1) ? hi.x : lo.x,
                                                 (c & 2) ? hi.y : lo.y,
                                                 (c & 4) ? hi.z : lo.z);
                                glm::vec3 w = glm::vec3(wm * glm::vec4(corner, 1.0f));
                                wmin = glm::min(wmin, w); wmax = glm::max(wmax, w);
                            }
                            float tb;
                            if (!rayAABB(ro, rd, wmin, wmax, tb) || tb >= bestT) continue;  // broad-phase
                            if (ctx.RaycastMesh(rc.firstMesh + m, wm, ro, rd, bestT))       // narrow-phase
                                bestEnt = ei;  // RaycastMesh updated bestT to this hit
                        }
                    } else {
                        // No mesh (sprite / light / emitter / sun): pick a small proxy
                        // box at the entity's world position so it's clickable like an
                        // editor handle — not just selectable from the Hierarchy.
                        const glm::vec3 c  = glm::vec3(wm[3]);
                        const glm::vec3 hs(0.35f);
                        float tb;
                        if (rayAABB(ro, rd, c - hs, c + hs, tb) && tb < bestT) {
                            bestT   = tb;
                            bestEnt = ei;
                        }
                    }
                }
                if (bestEnt >= 0) selectedItem = bestEnt;
                // else: leave selection unchanged (don't deselect on a stray miss).
            }
        }

        // Right-button press inside the viewport starts the editor look camera.
        if (pendingRightDown) {
            const bool insideViewport =
                pendingClickX >= centerX &&
                pendingClickX <  centerX + centerW &&
                pendingClickY >= centerY &&
                pendingClickY <  centerY + centerH;
            if (insideViewport) setMouseCapture(true);
        }

        // ---- Continue / end a gizmo drag (Editor only) ----
        if (draggedAxis >= 0 && mode == EditorMode::Editor) {
            float mxAbs, myAbs;
            const Uint32 mb = SDL_GetMouseState(&mxAbs, &myAbs);
            const bool lmbDown = (mb & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) != 0;
            if (!lmbDown || selectedItem < 0 || !prevValid) {
                draggedAxis = -1;  // released or selection gone → stop
            } else {
                const glm::vec3 axes[3] = { {1,0,0},{0,1,0},{0,0,1} };
                glm::vec3 ro, rd;
                mouseRay(mxAbs, myAbs, prevCenterX, prevCenterY, prevCenterW, prevCenterH,
                         prevView, prevProj, camera.Position, ro, rd);
                float s = closestSOnAxis(dragStartPos, axes[draggedAxis], ro, rd);
                glm::vec3 newPos = dragStartPos + axes[draggedAxis] * (s - dragStartS);
                // Write back to the selected entity's transform. Light entities
                // follow via LightSyncSystem (reads worldMatrix next world.Update).
                const int nEnt = int(sceneEntities.size());
                if (selectedItem < nEnt) {
                    ecs::Entity e = sceneEntities[size_t(selectedItem)].entity;
                    if (registry.HasComponent<ecs::TransformComponent>(e)) {
                        // newPos is world-space; convert into the parent's space if
                        // this is a child, so dragging stays correct under hierarchy.
                        glm::vec3 localPos = newPos;
                        if (registry.HasComponent<ecs::ParentComponent>(e)) {
                            ecs::Entity parent = registry.GetComponent<ecs::ParentComponent>(e).parent;
                            if (registry.IsAlive(parent) &&
                                registry.HasComponent<ecs::TransformComponent>(parent)) {
                                glm::mat4 pInv = glm::inverse(
                                    registry.GetComponent<ecs::TransformComponent>(parent).worldMatrix);
                                localPos = glm::vec3(pInv * glm::vec4(newPos, 1.0f));
                            }
                        }
                        registry.GetComponent<ecs::TransformComponent>(e).position = localPos;
                    }
                }
            }
        }

        // ---- Phase 4: end-of-frame input bookkeeping ----
        // Update() resets pressed/released and refreshes mouse position for
        // NEXT frame. Must run AFTER widgets read pressed/released this frame.
        if (auto* uiCtx = FluentUI::GetContext()) {
            uiCtx->input.Update(window);
        }

        // WASD/Q/E fly the camera only while it owns the mouse — i.e. holding
        // the right button in Editor, or anytime in Play. Otherwise the keys
        // stay free for the UI and won't drift the camera while you edit values.
        if (mouseCaptured) handleKeyboard(camera, dt);

        // Apply any pending scene switch at this safe boundary (before systems run
        // on the new scene). Rebuild the editor list when it actually switches.
        if (pokemotor::vk::SceneManager::Instance().ProcessPending()) {
            rebuildSceneList();
            saveStatus = "Loaded scene '" +
                         pokemotor::vk::SceneManager::Instance().ActiveScene() + "'";
        }

        world.Update(dt);  // runs every registered system (TransformSystem, …)

        glm::mat4 view = camera.GetViewMatrix();
        // Use the central viewport PANEL's aspect, not the swapchain's. The scene
        // renders offscreen at swapchain size but FluentUI scales that image into
        // the (differently-shaped) center panel, so matching the panel aspect here
        // cancels the distortion. It also makes a later mouse→world unproject map
        // straight through the panel rect (needed for gizmo picking).
        const float viewportAspect = (centerH > 0.0f && centerW > 0.0f)
            ? (centerW / centerH) : ctx.AspectRatio();
        glm::mat4 proj = glm::perspective(glm::radians(camera.Zoom),
                                          viewportAspect, 0.1f, 200.0f);

        // Pull the (single) directional-light entity into the DirectionalLight
        // BeginFrame reads. View-based so it survives a scene Load (which destroys
        // and recreates the sun entity).
        for (auto [se, dl] : registry.GetView<ecs::DirectionalLightComponent>()) {
            sun.direction = glm::normalize(dl.direction);
            sun.color     = dl.color;
            sun.ambient   = dl.ambient;
            break;  // one sun
        }

        if (ctx.BeginFrame(window, view, proj, camera.Position, sun)) {
            for (auto [entity, transform, render]
                 : registry.GetView<ecs::TransformComponent,
                                    pokemotor::vk::RenderComponentVk>()) {
                if (!render.visible) continue;
                for (uint32_t i = 0; i < render.meshCount; ++i) {
                    ctx.SubmitDraw(render.firstMesh + i, transform.worldMatrix);
                }
            }

            // Transform gizmo: 3 axis lines at the selected entity's position.
            const int gEnt = int(sceneEntities.size());
            glm::vec3 gizmoPos(0.0f);
            bool hasGizmo = false;
            if (selectedItem >= 0 && selectedItem < gEnt) {
                ecs::Entity ge = sceneEntities[size_t(selectedItem)].entity;
                if (registry.HasComponent<ecs::TransformComponent>(ge)) {
                    gizmoPos = glm::vec3(registry.GetComponent<ecs::TransformComponent>(ge).worldMatrix[3]);
                    hasGizmo = true;
                }
            }
            if (hasGizmo) {
                // Draw each axis as an arrow: a shaft line plus a solid cone
                // (triangle fan + base cap) forming the arrowhead. dir is a unit
                // axis; p1/p2 are two perpendiculars (p1 via component-rotate +
                // Gram-Schmidt for robustness, p2 = cross) spanning the cone's
                // base circle.
                auto addArrow = [&](const glm::vec3& dir, const glm::vec3& col) {
                    const float s  = 1.0f;     // shaft length
                    const float hl = 0.16f;    // cone (arrowhead) length
                    const float hr = 0.045f;   // cone base radius — slim, proportional to the thin shaft
                    const glm::vec3 tip  = gizmoPos + dir * s;
                    const glm::vec3 p1   = glm::normalize(glm::vec3(dir.y, dir.z, dir.x) -
                                                          dir * glm::dot(glm::vec3(dir.y, dir.z, dir.x), dir));
                    const glm::vec3 p2   = glm::cross(dir, p1);
                    const glm::vec3 base = tip - dir * hl;
                    ctx.AddDebugLine(gizmoPos, base, col);   // shaft up to the cone base
                    const int N = 12;
                    for (int i = 0; i < N; ++i) {
                        float a0 = float(i)     / float(N) * 6.2831853f;
                        float a1 = float(i + 1) / float(N) * 6.2831853f;
                        glm::vec3 r0 = base + (std::cos(a0) * p1 + std::sin(a0) * p2) * hr;
                        glm::vec3 r1 = base + (std::cos(a1) * p1 + std::sin(a1) * p2) * hr;
                        ctx.AddDebugTriangle(tip, r0, r1, col);   // cone side
                        ctx.AddDebugTriangle(base, r1, r0, col);  // base cap (reverse winding)
                    }
                };
                addArrow(glm::vec3(1,0,0), glm::vec3(1.0f, 0.2f, 0.2f)); // X red
                addArrow(glm::vec3(0,1,0), glm::vec3(0.2f, 1.0f, 0.2f)); // Y green
                addArrow(glm::vec3(0,0,1), glm::vec3(0.3f, 0.5f, 1.0f)); // Z blue
            }

            ctx.EndFrame(window);
        }

        // Cache this frame's camera + panel rect for next frame's gizmo
        // pick/drag (input runs before view/proj are computed, so we use the
        // previous frame's values — one frame of lag, imperceptible).
        // IMPORTANT: the gizmo is DRAWN with the Vulkan Y-flipped projection
        // (VulkanContext negates proj[1][1] in updateUniformBuffer), but `proj`
        // here is the un-flipped GL-convention matrix. Pick/draw must agree, so
        // we store the SAME Y-flipped proj the renderer uses — otherwise the
        // pick is mirrored vertically and you can never grab an axis.
        glm::mat4 projFlipped = proj;
        projFlipped[1][1] *= -1.0f;
        prevView = view; prevProj = projFlipped;
        prevCenterX = centerX; prevCenterY = centerY; prevCenterW = centerW; prevCenterH = centerH;
        prevValid = true;
    }

    ctx.WaitIdle();
    // Destroy FluentUI first — DestroyContext owns the VulkanBackend pointer
    // and will delete it. After this, ctx.UIBackend() returns the (now stale)
    // pointer, but ctx.Shutdown() already nulls m_uiBackend internally.
    FluentUI::DestroyContext();
    ctx.Shutdown();
    audio::AudioManager::Instance().Shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
