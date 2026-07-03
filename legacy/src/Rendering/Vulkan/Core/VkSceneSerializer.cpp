#include "VkSceneSerializer.h"

#include "VulkanContext.h"
#include "Registry.h"
#include "World.h"
#include "CommonComponents.h"
#include "Physics/CollisionSystem.h"
#include "Scripting/ScriptComponent.h"
#include "Rendering/Vulkan/VkComponents.h"

#include <json.hpp>
#include <glm/glm.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace pokemotor::vk {
namespace SceneSerializer {

namespace {
json V3(const glm::vec3& v) { return json::array({ v.x, v.y, v.z }); }
json V4(const glm::vec4& v) { return json::array({ v.x, v.y, v.z, v.w }); }
json Q (const glm::quat& q) { return json::array({ q.x, q.y, q.z, q.w }); }

glm::vec3 rdV3(const json& a) {
    return glm::vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
}
glm::vec4 rdV4(const json& a) {
    return glm::vec4(a[0].get<float>(), a[1].get<float>(), a[2].get<float>(), a[3].get<float>());
}
glm::quat rdQ(const json& a) {  // stored {x,y,z,w}; glm::quat ctor is (w,x,y,z)
    return glm::quat(a[3].get<float>(), a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
}
}  // namespace

bool Save(ecs::Registry& registry, const std::string& path) {
    json root;
    root["version"] = 1;
    json entities = json::array();

    // First pass: entity → output index, so we can write parent references as
    // array indices (the load order matches this iteration order).
    std::unordered_map<ecs::Entity, int> indexOf;
    {
        int i = 0;
        for (auto [entity, tag] : registry.GetView<ecs::TagComponent>())
            indexOf[entity] = i++;
    }

    // Second pass over every scene object (every entity carrying a TagComponent).
    for (auto [entity, tag] : registry.GetView<ecs::TagComponent>()) {
        json e;
        e["name"] = tag.name;

        if (registry.HasComponent<ecs::ParentComponent>(entity)) {
            auto it = indexOf.find(registry.GetComponent<ecs::ParentComponent>(entity).parent);
            if (it != indexOf.end()) e["parentIndex"] = it->second;
        }

        if (registry.HasComponent<ecs::TransformComponent>(entity)) {
            const auto& t = registry.GetComponent<ecs::TransformComponent>(entity);
            e["transform"] = { {"position", V3(t.position)},
                               {"rotation", Q(t.rotation)},
                               {"scale",    V3(t.scale)} };
        }
        if (registry.HasComponent<RenderComponentVk>(entity)) {
            const auto& r = registry.GetComponent<RenderComponentVk>(entity);
            e["render"] = { {"firstMesh", r.firstMesh},
                            {"meshCount", r.meshCount},
                            {"visible",   r.visible} };
        }
        if (registry.HasComponent<ecs::LightComponent>(entity)) {
            const auto& l = registry.GetComponent<ecs::LightComponent>(entity);
            e["light"] = { {"type",         l.type},
                           {"color",        V3(l.color)},
                           {"intensity",    l.intensity},
                           {"range",        l.range},
                           {"direction",    V3(l.direction)},
                           {"innerDegrees", l.innerDegrees},
                           {"outerDegrees", l.outerDegrees},
                           {"radius",       l.radius} };
        }
        if (registry.HasComponent<ecs::DirectionalLightComponent>(entity)) {
            const auto& d = registry.GetComponent<ecs::DirectionalLightComponent>(entity);
            e["sun"] = { {"direction", V3(d.direction)},
                         {"color",     V3(d.color)},
                         {"ambient",   V3(d.ambient)} };
        }
        if (registry.HasComponent<SpriteComponentVk>(entity)) {
            const auto& s = registry.GetComponent<SpriteComponentVk>(entity);
            e["sprite"] = { {"texturePath",   s.texturePath},
                            {"color",         V4(s.color)},
                            {"uvOffsetScale", V4(s.uvOffsetScale)},
                            {"alphaClip",     s.alphaClip},
                            {"metallic",      s.metallic},
                            {"roughness",     s.roughness} };
        }
        if (registry.HasComponent<ParticleEmitterComponentVk>(entity)) {
            const auto& p = registry.GetComponent<ParticleEmitterComponentVk>(entity);
            e["emitter"] = { {"direction",    V3(p.direction)},
                             {"spread",       p.spread},
                             {"minSpeed",     p.minSpeed},
                             {"maxSpeed",     p.maxSpeed},
                             {"minLifetime",  p.minLifetime},
                             {"maxLifetime",  p.maxLifetime},
                             {"startSize",    p.startSize},
                             {"endSize",      p.endSize},
                             {"startColor",   V4(p.startColor)},
                             {"gravity",      V3(p.gravity)},
                             {"emitRate",     p.emitRate},
                             {"maxParticles", p.maxParticles} };
        }
        if (registry.HasComponent<physics::ColliderComponent>(entity)) {
            const auto& c = registry.GetComponent<physics::ColliderComponent>(entity);
            e["collider"] = { {"type",        static_cast<int>(c.type)},
                              {"offset",      V3(c.offset)},
                              {"halfExtents", V3(c.halfExtents)},
                              {"radius",      c.radius},
                              {"height",      c.height},
                              {"isTrigger",   c.isTrigger},
                              {"layer",       c.layer},
                              {"mask",        c.mask} };
        }
        if (registry.HasComponent<physics::RigidBodyComponent>(entity)) {
            const auto& r = registry.GetComponent<physics::RigidBodyComponent>(entity);
            // Config only — runtime velocity/accel/grounded reset on load.
            e["rigidBody"] = { {"gravity",           r.gravity},
                               {"useGravity",        r.useGravity},
                               {"groundedThreshold", r.groundedThreshold} };
        }
        if (registry.HasComponent<scripting::ScriptComponent>(entity)) {
            const auto& s = registry.GetComponent<scripting::ScriptComponent>(entity);
            e["script"] = { {"path", s.scriptPath} };  // runtime instance re-created on load
        }

        entities.push_back(std::move(e));
    }

    root["entities"] = std::move(entities);

    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(path, ec);
    const std::string absStr = ec ? path : abs.string();

    std::ofstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[Scene] save FAILED — cannot open '%s'\n", absStr.c_str());
        return false;
    }
    f << root.dump(2);
    f.close();
    std::printf("[Scene] saved %zu entities to '%s'\n",
                root["entities"].size(), absStr.c_str());
    return true;
}

bool Load(ecs::World& world, VulkanContext& ctx, const std::string& path) {
    ecs::Registry& registry = world.GetRegistry();
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[Scene] load FAILED — cannot open '%s'\n", path.c_str());
        return false;
    }
    json root;
    try {
        f >> root;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[Scene] load FAILED — parse error: %s\n", ex.what());
        return false;
    }
    if (!root.contains("entities") || !root["entities"].is_array()) {
        std::fprintf(stderr, "[Scene] load FAILED — no 'entities' array\n");
        return false;
    }

    // No in-flight frame may read the buffers we're about to rewrite.
    ctx.WaitIdle();
    ctx.ClearLights();
    ctx.ClearParticleEmitters();
    ctx.ClearSprites();
    registry.Clear();

    // Index-aligned with root["entities"], so parentIndex refs resolve in pass 2.
    std::vector<ecs::Entity> created;
    try {
        for (const auto& e : root["entities"]) {
            ecs::Entity ent = registry.CreateEntity();
            created.push_back(ent);
            registry.AddComponent<ecs::TagComponent>(ent).name =
                e.value("name", std::string("Entity"));

            // Transform first — sprites/emitters derive their model/position from it.
            ecs::TransformComponent tc;
            if (e.contains("transform")) {
                const auto& t = e["transform"];
                tc.position = rdV3(t["position"]);
                tc.rotation = rdQ(t["rotation"]);
                tc.scale    = rdV3(t["scale"]);
            }
            registry.AddComponent<ecs::TransformComponent>(ent) = tc;

            if (e.contains("render")) {
                const auto& r = e["render"];
                RenderComponentVk rc;
                rc.firstMesh = r.value("firstMesh", 0u);
                rc.meshCount = r.value("meshCount", 0u);
                rc.visible   = r.value("visible", true);
                registry.AddComponent<RenderComponentVk>(ent) = rc;
            }
            if (e.contains("light")) {
                const auto& l = e["light"];
                ecs::LightComponent lc;
                lc.type         = l.value("type", 0);
                lc.color        = rdV3(l["color"]);
                lc.intensity    = l.value("intensity", 8.0f);
                lc.range        = l.value("range", 6.0f);
                lc.direction    = rdV3(l["direction"]);
                lc.innerDegrees = l.value("innerDegrees", 18.0f);
                lc.outerDegrees = l.value("outerDegrees", 28.0f);
                lc.radius       = l.value("radius", 0.5f);
                switch (lc.type) {
                    case 1: lc.rendererHandle = ctx.RegisterSpotLight(tc.position, lc.direction,
                                lc.color, lc.intensity, lc.range, lc.innerDegrees, lc.outerDegrees); break;
                    case 2: lc.rendererHandle = ctx.RegisterAreaLight(tc.position, lc.color,
                                lc.intensity, lc.range, lc.radius); break;
                    default: lc.rendererHandle = ctx.RegisterPointLight(tc.position, lc.color,
                                lc.intensity, lc.range); break;
                }
                registry.AddComponent<ecs::LightComponent>(ent) = lc;
            }
            if (e.contains("sun")) {
                const auto& s = e["sun"];
                ecs::DirectionalLightComponent dl;
                dl.direction = rdV3(s["direction"]);
                dl.color     = rdV3(s["color"]);
                dl.ambient   = rdV3(s["ambient"]);
                registry.AddComponent<ecs::DirectionalLightComponent>(ent) = dl;
            }
            if (e.contains("sprite")) {
                const auto& s = e["sprite"];
                SpriteComponentVk sc;
                sc.texturePath   = s.value("texturePath", std::string());
                sc.color         = rdV4(s["color"]);
                sc.uvOffsetScale = rdV4(s["uvOffsetScale"]);
                sc.alphaClip     = s.value("alphaClip", 0.5f);
                sc.metallic      = s.value("metallic", 0.0f);
                sc.roughness     = s.value("roughness", 0.6f);
                sc.textureIndex  = sc.texturePath.empty() ? 0u
                                 : ctx.RegisterSpriteTexture(sc.texturePath);
                VulkanContext::SpriteDesc sd;
                sd.model         = tc.GetMatrix();
                sd.color         = sc.color;
                sd.uvOffsetScale = sc.uvOffsetScale;
                sd.alphaClip     = sc.alphaClip;
                sd.metallic      = sc.metallic;
                sd.roughness     = sc.roughness;
                sd.textureIndex  = sc.textureIndex;
                sc.rendererHandle = ctx.RegisterSprite(sd);
                registry.AddComponent<SpriteComponentVk>(ent) = sc;
            }
            if (e.contains("emitter")) {
                const auto& p = e["emitter"];
                ParticleEmitterComponentVk pe;
                pe.direction    = rdV3(p["direction"]);
                pe.spread       = p.value("spread", 0.5f);
                pe.minSpeed     = p.value("minSpeed", 1.0f);
                pe.maxSpeed     = p.value("maxSpeed", 3.0f);
                pe.minLifetime  = p.value("minLifetime", 0.5f);
                pe.maxLifetime  = p.value("maxLifetime", 2.0f);
                pe.startSize    = p.value("startSize", 0.1f);
                pe.endSize      = p.value("endSize", 0.0f);
                pe.startColor   = rdV4(p["startColor"]);
                pe.gravity      = rdV3(p["gravity"]);
                pe.emitRate     = p.value("emitRate", 50.0f);
                pe.maxParticles = p.value("maxParticles", 1000);
                VulkanContext::ParticleEmitterDesc d;
                d.position     = tc.position;
                d.direction    = pe.direction;    d.spread      = pe.spread;
                d.minSpeed     = pe.minSpeed;     d.maxSpeed     = pe.maxSpeed;
                d.minLifetime  = pe.minLifetime;  d.maxLifetime  = pe.maxLifetime;
                d.startSize    = pe.startSize;    d.endSize      = pe.endSize;
                d.startColor   = pe.startColor;   d.gravity      = pe.gravity;
                d.emitRate     = pe.emitRate;     d.maxParticles = pe.maxParticles;
                pe.rendererHandle = ctx.RegisterParticleEmitter(d);
                registry.AddComponent<ParticleEmitterComponentVk>(ent) = pe;
            }
            if (e.contains("collider")) {
                const auto& c = e["collider"];
                physics::ColliderComponent col;
                col.type        = static_cast<physics::ColliderComponent::Type>(c.value("type", 0));
                col.offset      = rdV3(c["offset"]);
                col.halfExtents = rdV3(c["halfExtents"]);
                col.radius      = c.value("radius", 0.5f);
                col.height      = c.value("height", 1.0f);
                col.isTrigger   = c.value("isTrigger", false);
                col.layer       = c.value("layer", static_cast<uint32_t>(physics::LAYER_DEFAULT));
                col.mask        = c.value("mask",  static_cast<uint32_t>(physics::LAYER_ALL));
                registry.AddComponent<physics::ColliderComponent>(ent) = col;
            }
            if (e.contains("rigidBody")) {
                const auto& r = e["rigidBody"];
                physics::RigidBodyComponent rb;  // velocity/accel/grounded default to rest
                rb.gravity           = r.value("gravity", -20.0f);
                rb.useGravity        = r.value("useGravity", true);
                rb.groundedThreshold = r.value("groundedThreshold", 0.1f);
                registry.AddComponent<physics::RigidBodyComponent>(ent) = rb;
            }
            if (e.contains("script")) {
                scripting::ScriptComponent sc;
                sc.scriptPath = e["script"].value("path", std::string());
                registry.AddComponent<scripting::ScriptComponent>(ent) = sc;
                // ScriptSystem instances it (OnCreate) on the next update.
            }
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[Scene] load: malformed entity — %s\n", ex.what());
        return false;
    }

    // Second pass: restore parent-child links now that every entity exists.
    const auto& arr = root["entities"];
    for (size_t i = 0; i < arr.size() && i < created.size(); ++i) {
        if (!arr[i].contains("parentIndex")) continue;
        int pi = arr[i]["parentIndex"].get<int>();
        if (pi >= 0 && pi < static_cast<int>(created.size()))
            world.SetParent(created[i], created[static_cast<size_t>(pi)]);
    }

    std::printf("[Scene] loaded %zu entities from '%s'\n", root["entities"].size(), path.c_str());
    return true;
}

}  // namespace SceneSerializer
}  // namespace pokemotor::vk
