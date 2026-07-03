#pragma once
#include "Registry.h"
#include "Systems.h"
#include "CommonComponents.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace ecs {

// Owns the entity Registry and an ordered list of Systems — the single container
// the app loop talks to each frame. Update(dt) runs every registered system in
// registration order, replacing the hand-rolled `system.Update(registry, dt)`
// calls that used to live in main(). This is the home future sync-systems
// (lights, sprites, particles) plug into as those subsystems migrate from the
// renderer's internal lists onto entities.
//
// Phase 1 of the ECS plan: this only owns the Registry + scheduler. Scene
// composition still lives in main() for now — it migrates here as lights /
// sprites / particles become real entities in later phases.
class World {
public:
    World() = default;

    Registry&       GetRegistry()       { return mRegistry; }
    const Registry& GetRegistry() const { return mRegistry; }

    // Construct and register a system in place. It runs in registration order on
    // every Update(). Returns a reference so the caller can keep a typed handle
    // (e.g. a sync-system that needs the renderer wired in later).
    template<typename T, typename... Args>
    T& AddSystem(Args&&... args) {
        auto sys = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *sys;
        mSystems.push_back(std::move(sys));
        return ref;
    }

    // Run all systems once, in registration order.
    void Update(float dt) {
        for (auto& system : mSystems)
            system->Update(mRegistry, dt);
    }

    // ---- Transform hierarchy (ported from the OpenGL Scene) -----------------
    // Parents `child` under `parent`, keeping BOTH sides in sync (the child's
    // ParentComponent and the parent's ChildrenComponent). TransformSystem then
    // propagates parent→child world matrices. Re-parenting is supported.
    void SetParent(Entity child, Entity parent) {
        if (mRegistry.HasComponent<ParentComponent>(child)) {
            // Detach from the old parent first, then re-add fresh. (The original
            // OpenGL code GetComponent'd the just-removed component here — a latent
            // bug; we re-add instead.)
            RemoveParent(child);
        }
        mRegistry.AddComponent<ParentComponent>(child, ParentComponent{parent});

        if (!mRegistry.HasComponent<ChildrenComponent>(parent))
            mRegistry.AddComponent<ChildrenComponent>(parent, ChildrenComponent{});
        mRegistry.GetComponent<ChildrenComponent>(parent).children.push_back(child);
    }

    // Detaches `child` from its parent, pruning it from the parent's children
    // list. No-op if it has no parent.
    void RemoveParent(Entity child) {
        if (!mRegistry.HasComponent<ParentComponent>(child)) return;
        Entity parent = mRegistry.GetComponent<ParentComponent>(child).parent;
        if (mRegistry.IsAlive(parent) && mRegistry.HasComponent<ChildrenComponent>(parent)) {
            auto& kids = mRegistry.GetComponent<ChildrenComponent>(parent).children;
            kids.erase(std::remove(kids.begin(), kids.end(), child), kids.end());
        }
        mRegistry.RemoveComponent<ParentComponent>(child);
    }

private:
    Registry                             mRegistry;
    std::vector<std::unique_ptr<System>> mSystems;
};

} // namespace ecs
