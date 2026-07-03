#pragma once

#include <string>
#include <sol/sol.hpp>

namespace scripting {

// Attaches a Lua "class" script to an entity. The script file returns a table
// with methods (OnCreate/OnUpdate/OnDestroy); ScriptSystem instances it and
// caches the callbacks. `scriptPath` is the only thing serialized — the rest is
// runtime state, re-derived on load / hot-reload.
struct ScriptComponent {
    std::string scriptPath;

    sol::table               instance;       // per-entity instance table
    sol::protected_function  onCreate;
    sol::protected_function  onUpdate;
    sol::protected_function  onDestroy;
    bool                     initialized = false;
};

}  // namespace scripting
