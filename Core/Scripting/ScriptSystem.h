// Core/Scripting/ScriptSystem.h — corre los ScriptComponent de la escena: carga
// cada .lua en su entorno, llama on_start una vez y on_update(self, dt) cada frame,
// todo protegido (un error loguea y NO tumba el motor ni para a los demás scripts).
// El estado de sol2 vive en el .cpp (PIMPL) para no arrastrar sol2 a quien lo use
// (p.ej. el editor). Diseño: MotorGrafico_BriefScripting.md.
#pragma once

#include "Core/ECS/Entity.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pk {

class LuaVM;
class Scene;
class Input;

// Variable que un script publica (tabla `exports`) para verla/editarla en el
// inspector. Soporta número (speed, rangos, cooldowns…), booleano (flags) y texto.
struct ScriptExport {
    enum class Type { Number, Bool, Text };
    std::string name;
    Type        type    = Type::Number;
    double      number  = 0.0;   // válido si type == Number
    bool        boolean = false; // válido si type == Bool
    std::string text;            // válido si type == Text
};

// Acciones de UI/secuencia que un script de evento puede ESPERAR. Las inyecta el lado
// Game (DialogueMode + GameStack) con std::function, así Core/Scripting no depende de
// Game: el ScriptSystem solo pide "muestra esto y avísame con done() cuando se cierre".
// El `done`/`done(idx)` lo invoca Game al cerrarse la UI → la corrutina se reanuda.
struct ScriptUIActions {
    std::function<void(const std::string& text, std::function<void()> done)>                 showText;
    std::function<void(const std::vector<std::string>& opts, std::function<void(int)> done)> showChoice;
    // wait(seconds) lo maneja el propio ScriptSystem con dt (no necesita Game).
};

class ScriptSystem {
public:
    ScriptSystem();
    ~ScriptSystem();

    void init(LuaVM* vm, Input* input);      // guarda refs + registra la Script API de gameplay
    void update(Scene& scene, float dt);
    void clear();                            // al cambiar de escena: olvida instancias

    // --- Corrutinas de evento (UI dirigida por script) ---
    // Inyecta las acciones de UI (las registra el lado Game). Ver ScriptUIActions.
    void setUIActions(ScriptUIActions actions);
    // ¿La instancia de esta entidad define la función (p.ej. "on_interact")?
    bool hasFunction(Entity e, const char* fn) const;
    // Arranca la función `fn` de la entidad como corrutina (cede en show_text/choice/wait).
    void runEvent(Scene& scene, Entity e, const char* fn);
    // Reanuda las corrutinas listas y caduca los wait(). Llamar 1x por frame (a nivel
    // Engine, fuera de la pila de modos: así un evento sigue aunque su modo esté pausado
    // y el push de UI no ocurra a mitad de iteración del GameStack).
    void pumpEvents(float dt);
    // ¿Hay alguna corrutina de evento viva (corriendo o esperando una UI)?
    bool eventsActive() const;

    // Consulta de colisión que respalda is_walkable(x,y) y self:try_step en Lua. La
    // provee quien conozca el mapa (p.ej. OverworldMode con su TileMap), así el
    // sistema no se acopla a ningún tilemap concreto. Sin callback: todo transitable.
    void setWalkable(std::function<bool(int, int)> fn);

    // Para el inspector: lee/escribe la tabla `exports` del script de la entidad.
    std::vector<ScriptExport> exportsOf(Entity e) const;
    void setExport(Entity e, const ScriptExport& ex);   // escribe según ex.type

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace pk
