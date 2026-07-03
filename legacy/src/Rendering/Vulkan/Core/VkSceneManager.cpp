#include "VkSceneManager.h"

#include "VkSceneSerializer.h"

#include <json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace pokemotor::vk {

SceneManager& SceneManager::Instance() {
    static SceneManager instance;
    return instance;
}

void SceneManager::Init(ecs::World* world, VulkanContext* ctx) {
    mWorld = world;
    mCtx   = ctx;
}

void SceneManager::RegisterScene(const std::string& name, const std::string& jsonPath) {
    mScenes[name] = jsonPath;
}

void SceneManager::ScanSceneDirectory(const std::string& directory) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) return;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        // Skip internal files (e.g. _playmode_snapshot.json), matching the old behaviour.
        if (entry.path().stem().string().rfind("_", 0) == 0) continue;

        std::string name = entry.path().stem().string();
        // Prefer an explicit scene "name" field if the file carries one.
        std::ifstream f(entry.path());
        if (f.is_open()) {
            try {
                auto j = nlohmann::json::parse(f);
                name = j.value("name", name);
            } catch (...) { /* keep the filename as the name */ }
        }
        RegisterScene(name, entry.path().string());
        std::printf("[SceneManager] registered '%s' -> %s\n",
                    name.c_str(), entry.path().string().c_str());
    }
}

void SceneManager::ChangeScene(const std::string& name) {
    mPending    = name;
    mHasPending = true;
}

bool SceneManager::ProcessPending() {
    if (!mHasPending) return false;
    mHasPending = false;
    if (!mWorld || !mCtx) return false;

    auto it = mScenes.find(mPending);
    if (it == mScenes.end()) {
        std::fprintf(stderr, "[SceneManager] unknown scene '%s'\n", mPending.c_str());
        return false;
    }
    if (!SceneSerializer::Load(*mWorld, *mCtx, it->second)) return false;
    mActive = mPending;
    return true;
}

std::vector<std::string> SceneManager::SceneNames() const {
    std::vector<std::string> names;
    names.reserve(mScenes.size());
    for (const auto& [name, path] : mScenes) names.push_back(name);
    return names;
}

}  // namespace pokemotor::vk
