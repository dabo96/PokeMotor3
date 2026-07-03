#include "ScriptEngine.h"

#include "Registry.h"
#include "CommonComponents.h"
#include "Physics/CollisionSystem.h"

#include <SDL3/SDL_keyboard.h>
#include <glm/glm.hpp>

#include <cstdio>
#include <sstream>
#include <vector>

namespace {
// `world` is exposed as an OBJECT (usertype) so scripts call it with a colon
// (`world:getTransform(e)`) — matching the existing example scripts. A colon
// call passes `world` as the implicit self, so the methods take it as `self`.
struct LuaWorld {
    scripting::ScriptEngine* engine = nullptr;

    ecs::Registry* reg() const { return engine ? engine->GetRegistry() : nullptr; }

    ecs::TransformComponent* getTransform(ecs::Entity e) const {
        ecs::Registry* r = reg();
        if (r && r->HasComponent<ecs::TransformComponent>(e))
            return &r->GetComponent<ecs::TransformComponent>(e);
        return nullptr;  // → nil in Lua
    }
    bool hasRigidBody(ecs::Entity e) const {
        ecs::Registry* r = reg();
        return r && r->HasComponent<physics::RigidBodyComponent>(e);
    }
    physics::RigidBodyComponent* getRigidBody(ecs::Entity e) const {
        ecs::Registry* r = reg();
        if (r && r->HasComponent<physics::RigidBodyComponent>(e))
            return &r->GetComponent<physics::RigidBodyComponent>(e);
        return nullptr;
    }
    physics::RigidBodyComponent* addRigidBody(ecs::Entity e) const {
        ecs::Registry* r = reg();
        if (!r) return nullptr;
        if (!r->HasComponent<physics::RigidBodyComponent>(e))
            r->AddComponent<physics::RigidBodyComponent>(e);
        return &r->GetComponent<physics::RigidBodyComponent>(e);
    }
};
}  // namespace

namespace scripting {

ScriptEngine& ScriptEngine::Instance() {
    static ScriptEngine instance;
    return instance;
}

void ScriptEngine::LogError(const std::string& context, const std::string& msg) {
    std::fprintf(stderr, "[Lua ERROR] %s: %s\n", context.c_str(), msg.c_str());
}

void ScriptEngine::Init() {
    if (mInitialized) return;
    mLua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                        sol::lib::table, sol::lib::os);
    RegisterBindings();
    mInitialized = true;
    std::printf("[Scripting] ScriptEngine initialized\n");
}

void ScriptEngine::Shutdown() {
    mClasses.clear();
    mTimes.clear();
    mInitialized = false;
}

void ScriptEngine::RegisterBindings() {
    // ---- Math: Vec3 (ported from LuaBindings_Math; supports Vec3(x,y,z) AND
    // Vec3.new(x,y,z), operators, and methods the example scripts use) ----
    mLua.new_usertype<glm::vec3>("Vec3",
        sol::constructors<glm::vec3(), glm::vec3(float), glm::vec3(float, float, float)>(),
        sol::call_constructor,
        sol::constructors<glm::vec3(), glm::vec3(float), glm::vec3(float, float, float)>(),
        "x", &glm::vec3::x, "y", &glm::vec3::y, "z", &glm::vec3::z,
        sol::meta_function::addition,
            [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
        sol::meta_function::subtraction,
            [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
        sol::meta_function::multiplication, sol::overload(
            [](const glm::vec3& a, float b) { return a * b; },
            [](float a, const glm::vec3& b) { return a * b; },
            [](const glm::vec3& a, const glm::vec3& b) { return a * b; }),
        sol::meta_function::division, sol::overload(
            [](const glm::vec3& a, float b) { return a / b; },
            [](const glm::vec3& a, const glm::vec3& b) { return a / b; }),
        sol::meta_function::unary_minus, [](const glm::vec3& a) { return -a; },
        sol::meta_function::equal_to,
            [](const glm::vec3& a, const glm::vec3& b) { return a == b; },
        sol::meta_function::to_string, [](const glm::vec3& v) {
            std::ostringstream ss; ss << "Vec3(" << v.x << ", " << v.y << ", " << v.z << ")";
            return ss.str();
        },
        "length",        [](const glm::vec3& v) { return glm::length(v); },
        "lengthSquared", [](const glm::vec3& v) { return glm::dot(v, v); },
        "normalize",     [](const glm::vec3& v) {
            float len = glm::length(v);
            return len > 0.0001f ? v / len : glm::vec3(0.0f);
        },
        "dot",      [](const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); },
        "cross",    [](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); },
        "lerp",     [](const glm::vec3& a, const glm::vec3& b, float t) { return glm::mix(a, b, t); },
        "distance", [](const glm::vec3& a, const glm::vec3& b) { return glm::distance(a, b); });

    // ---- TransformComponent (position/scale editable from Lua) ----
    mLua.new_usertype<ecs::TransformComponent>("Transform",
        "position", &ecs::TransformComponent::position,
        "scale",    &ecs::TransformComponent::scale);

    // ---- RigidBody (physics) — readable/writable from scripts ----
    mLua.new_usertype<physics::RigidBodyComponent>("RigidBody",
        "velocity",     &physics::RigidBodyComponent::velocity,
        "acceleration", &physics::RigidBodyComponent::acceleration,
        "gravity",      &physics::RigidBodyComponent::gravity,
        "useGravity",   &physics::RigidBodyComponent::useGravity,
        "grounded",     &physics::RigidBodyComponent::grounded);

    // ---- world: entity component access (object → call with `world:method()`) ----
    mLua.new_usertype<LuaWorld>("LuaWorld",
        "getTransform",  &LuaWorld::getTransform,
        "hasRigidBody",  &LuaWorld::hasRigidBody,
        "getRigidBody",  &LuaWorld::getRigidBody,
        "addRigidBody",  &LuaWorld::addRigidBody);
    mLua["world"] = LuaWorld{ this };

    // ---- Debug.Log ----
    sol::table dbg = mLua.create_named_table("Debug");
    dbg.set_function("Log", [](const std::string& s) {
        std::printf("[Lua] %s\n", s.c_str());
    });

    // ---- Input: raw keyboard straight from SDL (no InputSystem wiring needed).
    // Names per SDL_GetScancodeFromName: "W","A","S","D","Space","Left Shift"... ----
    sol::table input = mLua.create_named_table("Input");
    input.set_function("isKeyHeld", [](const std::string& name) -> bool {
        SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) return false;
        const bool* state = SDL_GetKeyboardState(nullptr);
        return state && state[sc];
    });

    // ---- Vec3 constants (so scripts can use Vec3.UP etc., like the old API) ----
    mLua.safe_script(
        "Vec3.ZERO=Vec3.new(0,0,0) Vec3.ONE=Vec3.new(1,1,1) "
        "Vec3.UP=Vec3.new(0,1,0) Vec3.DOWN=Vec3.new(0,-1,0) "
        "Vec3.FORWARD=Vec3.new(0,0,-1) Vec3.BACK=Vec3.new(0,0,1) "
        "Vec3.RIGHT=Vec3.new(1,0,0) Vec3.LEFT=Vec3.new(-1,0,0)",
        &sol::script_pass_on_error);
}

sol::table ScriptEngine::LoadClassFromDisk(const std::string& path) {
    // Record the timestamp even on failure, so a broken script still gets polled
    // and auto-reloads once the user fixes + saves it.
    std::error_code tec;
    mTimes[path] = std::filesystem::last_write_time(path, tec);

    sol::protected_function_result r = mLua.safe_script_file(path, &sol::script_pass_on_error);
    if (!r.valid()) {
        sol::error err = r;
        LogError("load " + path, err.what());
        mClasses.erase(path);
        return sol::table();
    }
    sol::object obj = r;
    if (obj.get_type() != sol::type::table) {
        LogError("load " + path, "script must `return` a table (its class)");
        mClasses.erase(path);
        return sol::table();
    }
    sol::table cls = obj.as<sol::table>();
    cls["__index"] = cls;  // instances fall through to the class via metatable
    mClasses[path] = cls;
    return cls;
}

sol::table ScriptEngine::LoadClass(const std::string& path) {
    auto it = mClasses.find(path);
    if (it != mClasses.end()) return it->second;
    return LoadClassFromDisk(path);
}

sol::table ScriptEngine::CreateInstance(const std::string& path, ecs::Entity entity) {
    sol::table cls = LoadClass(path);
    if (!cls.valid()) return sol::table();
    sol::table instance = mLua.create_table();
    instance["entity"] = static_cast<uint32_t>(entity);
    instance[sol::metatable_key] = cls;
    return instance;
}

void ScriptEngine::CheckHotReload(float dt) {
    mHotTimer += dt;
    if (mHotTimer < HOT_RELOAD_INTERVAL) return;
    mHotTimer = 0.0f;

    std::vector<std::string> changed;
    for (auto& [path, stamp] : mTimes) {  // mTimes covers loaded AND failed scripts
        std::error_code ec;
        auto t = std::filesystem::last_write_time(path, ec);
        if (ec) continue;
        if (stamp != t) changed.push_back(path);
    }
    for (auto& path : changed) {
        if (LoadClassFromDisk(path).valid()) {
            std::printf("[Scripting] hot-reloaded '%s'\n", path.c_str());
            if (mReloadCb) mReloadCb(path);
        }
    }
}

void ScriptEngine::ReloadAll() {
    std::vector<std::string> paths;
    for (auto& [p, t] : mTimes) paths.push_back(p);  // includes previously-failed scripts
    for (auto& p : paths) {
        if (LoadClassFromDisk(p).valid() && mReloadCb) mReloadCb(p);
    }
    std::printf("[Scripting] reloaded %zu script(s)\n", paths.size());
}

}  // namespace scripting
