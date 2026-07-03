// Core/Scripting/LuaVM.cpp — implementación del estado de Lua.
#include "Core/Scripting/LuaVM.h"

#include "Core/Log.h"

namespace pk {

void LuaVM::init() {
    m_lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                         sol::lib::table, sol::lib::os);

    // Script API — la superficie estable que los scripts llaman (NO las tripas del
    // motor). Mínima en v1; se amplía con criterio. log() va a la consola del editor.
    m_lua.set_function("log", [](const std::string& msg) { LOG_INFO("[lua] %s", msg.c_str()); });
}

void LuaVM::installEventPrelude() {
    // Wrappers de la API de eventos: presentan UI/acción (vía las funciones C++
    // __push_* que registra el ScriptSystem) y CEDEN con coroutine.yield(); la
    // corrutina del evento se reanuda cuando la UI/acción termina (lo decide el
    // ScriptSystem en su scheduler). Definidos como globals → visibles en el entorno
    // de cada script (loadModule hereda los globals). Error de carga → consola.
    const char* kPrelude = R"LUA(
        function show_text(text)
            __push_dialogue(text)
            coroutine.yield()            -- pausa hasta que el diálogo se cierre
        end
        function show_choice(options)
            __push_choice(options)
            return coroutine.yield()     -- al reanudar, devuelve el índice elegido
        end
        function wait(seconds)
            __wait(seconds)
            coroutine.yield()            -- pausa N segundos
        end
    )LUA";
    const sol::protected_function_result res =
        m_lua.safe_script(kPrelude, sol::script_pass_on_error);
    if (!res.valid()) {
        const sol::error err = res;
        LOG_ERROR("Lua: error en el preludio de eventos: %s", err.what());
    }
}

sol::table LuaVM::loadModule(const std::string& path) {
    // Entorno NUEVO por script (hereda los globals, pero sus asignaciones globales —
    // on_start/on_update/exports — quedan aquí, sin contaminar el estado global ni a
    // otros scripts). Permite recargar limpio. Error → se loguea y se devuelve el
    // entorno parcial (el ScriptSystem verá on_update inválido y no lo llamará).
    sol::environment env(m_lua, sol::create, m_lua.globals());
    sol::protected_function_result res =
        m_lua.safe_script_file(path, env, sol::script_pass_on_error);
    if (!res.valid()) {
        const sol::error err = res;
        LOG_ERROR("Lua: error en '%s': %s", path.c_str(), err.what());
    }
    return env;
}

bool LuaVM::runFile(const std::string& path) {
    const sol::protected_function_result res =
        m_lua.safe_script_file(path, sol::script_pass_on_error);
    if (!res.valid()) {
        const sol::error err = res;
        LOG_ERROR("Lua: error en '%s': %s", path.c_str(), err.what());
        return false;
    }
    return true;
}

}  // namespace pk
