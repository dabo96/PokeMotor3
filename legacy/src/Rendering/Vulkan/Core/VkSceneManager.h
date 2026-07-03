#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace ecs { class World; }

namespace pokemotor::vk {

class VulkanContext;

// Deferred scene switching, adapted from the OpenGL SceneManager to our
// single-World model. Register scenes by name → JSON path; ChangeScene() only
// REQUESTS a switch, which ProcessPending() applies at a safe frame boundary
// (before the systems run / before BeginFrame) — so a switch triggered mid-frame
// or from future gameplay never loads while the GPU is mid-read. One active
// scene at a time; the overlay stack (Push/Pop) from the old version is deferred
// until we have layered scenes.
class SceneManager {
public:
    static SceneManager& Instance();

    // Wire the World + renderer the loads operate on. Call once at startup.
    void Init(ecs::World* world, VulkanContext* ctx);

    // Map a scene name to a JSON file (no load yet).
    void RegisterScene(const std::string& name, const std::string& jsonPath);

    // Auto-register every *.json in a directory (skips names starting with '_').
    void ScanSceneDirectory(const std::string& directory);

    // Request a switch to a registered scene. Applied on the next ProcessPending.
    void ChangeScene(const std::string& name);

    // Apply any pending switch (SceneSerializer::Load). Call once per frame at a
    // safe point. Returns true if it actually switched (caller then rebuilds any
    // editor-side entity list).
    bool ProcessPending();

    const std::string&       ActiveScene() const { return mActive; }
    std::vector<std::string> SceneNames() const;

private:
    SceneManager() = default;

    ecs::World*    mWorld = nullptr;
    VulkanContext* mCtx   = nullptr;
    std::unordered_map<std::string, std::string> mScenes;  // name → json path
    std::string mActive;
    std::string mPending;
    bool        mHasPending = false;
};

}  // namespace pokemotor::vk
