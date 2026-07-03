#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <sol/sol.hpp>

#include "Entity.h"

namespace ecs { class Registry; }

namespace scripting {

// Minimal Lua scripting host (Phase 5a). Owns the sol2 state, loads script
// "class" tables (cached, hot-reloaded on file change), and instances them per
// entity. Bindings are intentionally small for now (entity transform + log) and
// grow in later phases. Singleton so bindings/loaders reach it from anywhere.
class ScriptEngine {
public:
    static ScriptEngine& Instance();

    void Init();
    void Shutdown();
    bool IsInitialized() const { return mInitialized; }

    void           SetRegistry(ecs::Registry* r) { mRegistry = r; }
    ecs::Registry* GetRegistry() const            { return mRegistry; }
    sol::state&    Lua()                          { return mLua; }

    // Returns the cached class table for a script (loads from disk on first use).
    // Invalid table on error.
    sol::table LoadClass(const std::string& path);

    // Builds a fresh instance table (metatable → class) with `entity` set.
    sol::table CreateInstance(const std::string& path, ecs::Entity entity);

    // Poll script files for changes (every HOT_RELOAD_INTERVAL); reload changed
    // class tables and fire the reload callback so live instances refresh.
    void CheckHotReload(float dt);
    void ReloadAll();  // force reload every cached script

    using ReloadCallback = std::function<void(const std::string& path)>;
    void SetReloadCallback(ReloadCallback cb) { mReloadCb = std::move(cb); }

    static void LogError(const std::string& context, const std::string& msg);

private:
    ScriptEngine() = default;
    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    void RegisterBindings();
    sol::table LoadClassFromDisk(const std::string& path);  // (re)reads + caches

    sol::state     mLua;
    bool           mInitialized = false;
    ecs::Registry* mRegistry    = nullptr;

    std::unordered_map<std::string, sol::table>                          mClasses;
    std::unordered_map<std::string, std::filesystem::file_time_type>     mTimes;
    float mHotTimer = 0.0f;
    static constexpr float HOT_RELOAD_INTERVAL = 1.0f;

    ReloadCallback mReloadCb;
};

}  // namespace scripting
