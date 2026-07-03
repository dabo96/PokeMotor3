#pragma once

#include "Systems.h"

namespace ecs { class Registry; }

namespace scripting {

// Drives Lua ScriptComponents: instances newly-attached scripts (OnCreate),
// calls OnUpdate every frame, and polls for hot-reload. Register it in the World
// BEFORE physics so scripts can set velocities the physics step then integrates.
class ScriptSystem : public ecs::System {
public:
    void Init(ecs::Registry& registry);                 // wires the hot-reload callback
    void Update(ecs::Registry& registry, float dt) override;
    void Shutdown(ecs::Registry& registry);             // OnDestroy on all scripts

private:
    void InitializeNewScripts(ecs::Registry& registry);
    bool mInitialized = false;
};

}  // namespace scripting
