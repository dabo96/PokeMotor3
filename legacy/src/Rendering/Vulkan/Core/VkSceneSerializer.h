#pragma once

#include <string>

namespace ecs { class Registry; class World; }

namespace pokemotor::vk {

class VulkanContext;

// Scene save/load over the unified ECS model (Phase 4b). Every scene object is
// an entity whose components own their data, so serialization is a single pass
// over the registry. Names come from ecs::TagComponent. Runtime-only fields
// (renderer handles, resolved texture indices) are skipped — they're re-derived
// on load.
namespace SceneSerializer {

// Writes every entity that has a TagComponent (i.e. every scene object) plus its
// known components to a JSON file. Returns false on I/O error. Read-only on the
// registry (takes a non-const ref only because the View API isn't const).
bool Save(ecs::Registry& registry, const std::string& path);

// Replaces the current scene with the one in `path`: waits for the GPU to be
// idle, clears the registry + the renderer's per-scene lists (lights/sprites/
// emitters; meshes/textures persist), then recreates every entity and
// re-registers its renderer objects (new handles, textures re-resolved by path).
// Returns false on I/O / parse error. Rebuild the editor's entity list after.
// Takes the World (not just the Registry) so it can restore parent-child links
// via World::SetParent in a second pass.
bool Load(ecs::World& world, VulkanContext& ctx, const std::string& path);

}  // namespace SceneSerializer
}  // namespace pokemotor::vk
