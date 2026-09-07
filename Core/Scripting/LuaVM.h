// Core/Scripting/LuaVM.h — estado de Lua (sol2): registra el Script API y carga/
// ejecuta scripts. Errores de carga/runtime van a la consola y NUNCA tumban el
// motor. Diseño: MotorGrafico_BriefScripting.md.
#pragma once

#include <sol/sol.hpp>

#include <string>

namespace pk {

class LuaVM {
public:
    void init();                                     // abre librerías + registra el Script API

    // Carga y ejecuta un chunk; devuelve la tabla de módulo (exports/on_start/on_update)
    // o una tabla inválida si hubo error (logueado). Para ScriptComponent. Con `outError`
    // el mensaje se devuelve además al llamante (el inspector lo muestra en la entidad, no
    // solo en la consola), y se vacía si la carga fue bien.
    sol::table loadModule(const std::string& path, std::string* outError = nullptr);

    // Define los wrappers Lua de la API de eventos (show_text/show_choice/wait) que
    // ceden con coroutine.yield. Los llama el ScriptSystem tras registrar las funciones
    // C++ que esos wrappers usan (__push_dialogue/__push_choice/__wait). Idempotente.
    void installEventPrelude();

    bool runFile(const std::string& path);           // ejecuta un .lua suelto (utilidad/prueba)

    sol::state& state() { return m_lua; }

private:
    sol::state m_lua;
};

}  // namespace pk
