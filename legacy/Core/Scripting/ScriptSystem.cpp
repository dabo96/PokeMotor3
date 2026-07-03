#include "ScriptSystem.h"

#include "ScriptComponent.h"
#include "ScriptEngine.h"
#include "Registry.h"

namespace scripting {

void ScriptSystem::Init(ecs::Registry& registry) {
    if (mInitialized) return;
    // On hot-reload of a script file, mark every instance using it for re-init so
    // the new code runs (fresh instance + OnCreate). Per-instance state resets —
    // acceptable and predictable for an edit-save-see-it loop.
    ScriptEngine::Instance().SetReloadCallback([&registry](const std::string& path) {
        registry.Each<ScriptComponent>([&](ecs::Entity, ScriptComponent& sc) {
            if (sc.scriptPath == path) sc.initialized = false;
        });
    });
    mInitialized = true;
}

void ScriptSystem::Update(ecs::Registry& registry, float dt) {
    auto& engine = ScriptEngine::Instance();
    if (!engine.IsInitialized()) return;

    InitializeNewScripts(registry);
    engine.CheckHotReload(dt);   // saving in the external editor reloads live

    registry.Each<ScriptComponent>([&](ecs::Entity, ScriptComponent& sc) {
        if (!sc.initialized || !sc.onUpdate.valid()) return;
        auto r = sc.onUpdate(sc.instance, dt);
        if (!r.valid()) {
            sol::error e = r;
            ScriptEngine::LogError("OnUpdate(" + sc.scriptPath + ")", e.what());
        }
    });
}

void ScriptSystem::InitializeNewScripts(ecs::Registry& registry) {
    auto& engine = ScriptEngine::Instance();
    registry.Each<ScriptComponent>([&](ecs::Entity entity, ScriptComponent& sc) {
        if (sc.initialized || sc.scriptPath.empty()) return;

        sc.instance = engine.CreateInstance(sc.scriptPath, entity);
        // Mark attempted either way so a broken script doesn't re-log every frame;
        // a hot-reload (fix + save) resets initialized=false and retries.
        sc.initialized = true;
        if (!sc.instance.valid()) {
            ScriptEngine::LogError("init", "no instance for '" + sc.scriptPath + "'");
            return;
        }

        sc.onCreate  = sc.instance["OnCreate"];
        sc.onUpdate  = sc.instance["OnUpdate"];
        sc.onDestroy = sc.instance["OnDestroy"];

        if (sc.onCreate.valid()) {
            auto r = sc.onCreate(sc.instance);
            if (!r.valid()) {
                sol::error e = r;
                ScriptEngine::LogError("OnCreate(" + sc.scriptPath + ")", e.what());
            }
        }
    });
}

void ScriptSystem::Shutdown(ecs::Registry& registry) {
    registry.Each<ScriptComponent>([&](ecs::Entity, ScriptComponent& sc) {
        if (sc.initialized && sc.onDestroy.valid()) {
            auto r = sc.onDestroy(sc.instance);
            if (!r.valid()) {
                sol::error e = r;
                ScriptEngine::LogError("OnDestroy(" + sc.scriptPath + ")", e.what());
            }
        }
    });
}

}  // namespace scripting
