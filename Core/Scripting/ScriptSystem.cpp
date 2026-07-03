// Core/Scripting/ScriptSystem.cpp — implementación (todo el estado de sol2 vive aquí).
#include "Core/Scripting/ScriptSystem.h"

#include "Core/Log.h"
#include "Core/Project.h"
#include "Core/Scripting/LuaVM.h"
#include "Input/Input.h"
#include "Input/KeyCode.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include <sol/sol.hpp>

#include <cmath>
#include <deque>
#include <filesystem>
#include <functional>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pk {

namespace fs = std::filesystem;

namespace {
// Ruta del script en disco, resuelta por el proyecto activo (scripts del proyecto →
// fallback a los del motor). En Debug el fallback es el árbol de fuentes, así el
// hot-reload del .lua real sigue funcionando.
std::string resolvePath(const std::string& rel) {
    return Project::instance().resolveRead(rel);
}

// Vista que el script ve como `self`: una entidad concreta dentro de SU escena. Se
// construye fresca por llamada (no se guarda entre frames), así nunca apunta a una
// escena vieja. Los métodos operan sobre su Transform (no-op si no lo tiene).
struct ScriptEntity {
    Scene* scene = nullptr;
    Entity e;
    const std::function<bool(int, int)>* walkable = nullptr;  // colisión de grid (prestada del sistema)

    float x() const { return has() ? scene->get<Transform>(e).position.x : 0.0f; }
    float y() const { return has() ? scene->get<Transform>(e).position.y : 0.0f; }
    void  set_position(float nx, float ny) { if (has()) scene->get<Transform>(e).position = Vec2(nx, ny); }
    void  move(float dx, float dy) {
        if (!has()) return;
        Vec2& p = scene->get<Transform>(e).position;
        p.x += dx; p.y += dy;
    }

    // --- API de grid (movimiento por casillas) ---
    // Intenta avanzar UNA celda en la dirección (dx,dy se reducen a -1/0/1 por eje).
    // Devuelve true si el paso arranca: la celda destino es transitable y no había
    // otro paso en curso. El deslizamiento suave lo avanza el ScriptSystem cada frame.
    // Acepta double (no int) a propósito: en Lua los números de move_axis() llegan con
    // subtipo float y sol2 rechazaría convertirlos a int aunque valgan ±1; el signo se
    // calcula aquí, así la API tolera cualquier número que mande el script.
    bool try_step(double dx, double dy) {
        if (!has()) return false;
        if (!scene->has<GridMover>(e)) scene->add<GridMover>(e, GridMover{});
        GridMover& gm = scene->get<GridMover>(e);
        seed(gm);
        if (gm.moving) return false;                   // ya hay un paso en curso

        const int sx = (dx > 0.0) - (dx < 0.0);        // signo: un solo tile por paso
        const int sy = (dy > 0.0) - (dy < 0.0);
        if (sx == 0 && sy == 0) return false;

        const int tx = gm.cell.x + sx, ty = gm.cell.y + sy;
        const bool ok = (walkable && *walkable) ? (*walkable)(tx, ty) : true;
        if (!ok) return false;                         // colisión: el paso no ocurre

        gm.from   = gm.cell;
        gm.cell   = IVec2(tx, ty);
        gm.moving = true;
        gm.t      = 0.0f;
        return true;
    }
    int grid_x() const { return gridCell().x; }
    int grid_y() const { return gridCell().y; }

private:
    bool has() const { return scene && scene->alive(e) && scene->has<Transform>(e); }

    // Siembra la celda lógica desde el Transform la primera vez (entidades colocadas
    // en mundo continuo arrancan en la casilla más cercana).
    void seed(GridMover& gm) const {
        if (gm.seeded || !has()) return;
        const Vec2 p = scene->get<Transform>(e).position;
        gm.cell = gm.from = IVec2(static_cast<int>(std::lround(p.x)),
                                  static_cast<int>(std::lround(p.y)));
        gm.seeded = true;
    }
    // Celda actual: del GridMover si existe; si no, se deriva del Transform.
    IVec2 gridCell() const {
        if (scene && scene->alive(e) && scene->has<GridMover>(e))
            return scene->get<GridMover>(e).cell;
        if (has()) {
            const Vec2 p = scene->get<Transform>(e).position;
            return IVec2(static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y)));
        }
        return IVec2(0, 0);
    }
};

// Llama una función protegida y loguea (sin lanzar) si falla.
template <typename... Args>
void call(sol::protected_function& fn, const char* what, const std::string& path, Args&&... args) {
    if (!fn.valid()) return;
    sol::protected_function_result r = fn(std::forward<Args>(args)...);
    if (!r.valid()) {
        const sol::error err = r;
        LOG_ERROR("Lua %s '%s': %s", what, path.c_str(), err.what());
    }
}
}  // namespace

struct ScriptSystem::Impl {
    struct Instance {
        std::string             path;
        fs::file_time_type      mtime;      // última escritura del .lua (hot-reload)
        sol::table              env;        // tabla/entorno del módulo (on_start/on_update/exports)
        sol::protected_function onUpdate;
        bool                    started = false;
        bool                    mtimeError = false; // ya avisamos que no se lee el mtime
    };

    // Una función de script (on_interact, una cutscene...) corriendo como corrutina.
    // Vive en su propio sol::thread (pila Lua aparte) para poder ceder/reanudar. Mientras
    // `waiting` está activa la corrutina espera a que una UI/acción/temporizador termine.
    struct RunningScript {
        sol::thread    thread;          // hilo Lua propio (mantiene viva la corrutina)
        sol::coroutine co;              // la función a reanudar
        bool           waiting = false; // ¿esperando una UI/acción/wait?
        bool           done    = false; // terminó (o murió por error) → quitar
        sol::object    result;          // valor a devolver al reanudar (índice del choice)
        float          waitLeft = 0.0f; // segundos restantes de un wait() pendiente
    };

    LuaVM* vm    = nullptr;
    Input* input = nullptr;
    Scene* currentScene = nullptr;                      // escena del update() en curso (la usa player_pos)
    std::unordered_map<uint32_t, Instance> instances;   // clave = Entity.id
    std::function<bool(int, int)> walkable;              // colisión de grid (la pone setWalkable)

    // Corrutinas de evento. deque → punteros ESTABLES: los callbacks de UI capturan el
    // RunningScript concreto, no debe moverse al añadir/quitar otros.
    std::deque<RunningScript> running;
    RunningScript* current = nullptr;   // la corrutina que se está arrancando/reanudando
    ScriptUIActions ui;                 // acciones de UI inyectadas por el lado Game

    // Limpia las corrutinas terminadas (no se borran a media reanudación; se marcan).
    void reapDone() {
        for (auto it = running.begin(); it != running.end();)
            it->done ? it = running.erase(it) : ++it;
    }

    // Reanuda una corrutina pasándole su resultado pendiente. Protegido: un error va a la
    // consola y la marca terminada (jamás crashea). `current` apunta a `rs` mientras corre
    // Lua (las funciones __push_* lo necesitan para saber qué corrutina espera).
    void resume(RunningScript& rs) {
        // Una corrutina recién creada aún no ha cedido (status ok, no yielded): no se puede
        // usar runnable() aquí o nunca arrancaría. La de abajo marca `done` al terminar, así
        // que basta con descartar las ya terminadas o inválidas.
        if (rs.done || !rs.co.valid()) {
            rs.done = true;
            return;
        }
        current = &rs;
        const sol::object arg = rs.result;
        rs.result = sol::object(sol::lua_nil);
        // Pasa el resultado del yield (índice del choice o nil) al reanudar.
        sol::protected_function_result r = arg.valid() ? rs.co(arg) : rs.co();
        current = nullptr;
        if (!r.valid()) {
            const sol::error err = r;
            LOG_ERROR("Lua corrutina de evento: %s", err.what());
            rs.done = true;
            return;
        }
        if (rs.co.status() != sol::call_status::yielded) rs.done = true;   // terminó normal
    }
};

ScriptSystem::ScriptSystem() : m_impl(std::make_unique<Impl>()) {}
ScriptSystem::~ScriptSystem() = default;

void ScriptSystem::init(LuaVM* vm, Input* input) {
    m_impl->vm    = vm;
    m_impl->input = input;
    if (!vm) return;

    sol::state& lua = vm->state();

    // self — la entidad que recibe on_update(self, dt).
    lua.new_usertype<ScriptEntity>("Entity",
        "x",            &ScriptEntity::x,
        "y",            &ScriptEntity::y,
        "set_position", &ScriptEntity::set_position,
        "move",         &ScriptEntity::move,
        "try_step",     &ScriptEntity::try_step,    // avanza 1 casilla si es transitable
        "grid_x",       &ScriptEntity::grid_x,      // celda actual (X)
        "grid_y",       &ScriptEntity::grid_y);     // celda actual (Y)

    // is_walkable(x, y) — consulta global de colisión de grid. Delega en el callback
    // que pone setWalkable (p.ej. el TileMap del overworld). Sin callback: transitable.
    // Toma double y redondea a celda (mismo motivo de subtipo numérico que try_step).
    Impl* impl = m_impl.get();
    lua.set_function("is_walkable", [impl](double x, double y) {
        const int ix = static_cast<int>(std::lround(x));
        const int iy = static_cast<int>(std::lround(y));
        return (impl && impl->walkable) ? impl->walkable(ix, iy) : true;
    });

    // player_pos() — posición LÓGICA del jugador (entidad con PlayerTag) de la escena en
    // curso, o nada (nil) si no hay jugador. La usa camera_follow.lua para seguirlo. Se lee
    // del GridMover (celda interpolada, sin el offset visual que el overworld aplica al
    // Transform), o del Transform si no hay GridMover. Devuelve (x, y) → en Lua:
    //   local px, py = player_pos(); if px == nil then return end
    lua.set_function("player_pos", [impl]() -> sol::variadic_results {
        sol::variadic_results out;
        Scene* sc = impl->currentScene;
        if (!sc || !impl->vm) return out;                 // vacío → nil en Lua
        Entity player{};
        sc->view<PlayerTag>().each([&](Entity e, PlayerTag&) { player = e; });
        if (!player.valid()) return out;
        float px = 0.0f, py = 0.0f;
        if (sc->has<GridMover>(player)) {
            const GridMover& gm = sc->get<GridMover>(player);
            if (gm.moving) {
                px = gm.from.x + (gm.cell.x - gm.from.x) * gm.t;
                py = gm.from.y + (gm.cell.y - gm.from.y) * gm.t;
            } else {
                px = static_cast<float>(gm.cell.x);
                py = static_cast<float>(gm.cell.y);
            }
        } else if (sc->has<Transform>(player)) {
            const Vec2 p = sc->get<Transform>(player).position;
            px = p.x; py = p.y;
        }
        sol::state& lua = impl->vm->state();
        out.push_back(sol::make_object(lua, px));
        out.push_back(sol::make_object(lua, py));
        return out;
    });

    // input — eje de movimiento de teclado (-1..1 en cada componente). Y crece hacia
    // abajo, igual que el mundo 2D. Captura el Input* directamente (estable).
    Input* in = input;
    lua.set_function("move_axis", [in]() {
        float dx = 0.0f, dy = 0.0f;
        if (in) {
            if (in->isKeyDown(Key::A) || in->isKeyDown(Key::Left))  dx -= 1.0f;
            if (in->isKeyDown(Key::D) || in->isKeyDown(Key::Right)) dx += 1.0f;
            if (in->isKeyDown(Key::W) || in->isKeyDown(Key::Up))    dy -= 1.0f;
            if (in->isKeyDown(Key::S) || in->isKeyDown(Key::Down))  dy += 1.0f;
        }
        return std::make_tuple(dx, dy);
    });

    // --- API de eventos (la usan los wrappers Lua del preludio, que ceden tras llamarlas).
    // Todas operan sobre impl->current: el RunningScript que se está reanudando/arrancando
    // en este instante (lo fija resume()/runEvent antes de entrar a Lua). Marcan waiting y
    // delegan en la acción inyectada por Game; el `done` la reanuda al cerrarse la UI.

    // __push_dialogue(text): presenta un diálogo y espera a que se cierre.
    lua.set_function("__push_dialogue", [impl](const std::string& text) {
        Impl::RunningScript* rs = impl->current;
        if (!rs) return;
        rs->waiting = true;
        if (!impl->ui.showText) { rs->waiting = false; return; }   // sin acción: no bloquea
        // done() reanudable. Captura el RS por puntero (estable: deque). Si la corrutina
        // ya murió, reanimarla es no-op (el scheduler la ignora por `done`).
        impl->ui.showText(text, [rs]() { rs->waiting = false; });
    });

    // __push_choice(opts): presenta opciones; el índice elegido vuelve por el yield.
    lua.set_function("__push_choice", [impl](sol::table opts) {
        Impl::RunningScript* rs = impl->current;
        if (!rs) return;
        std::vector<std::string> v;
        for (auto& kv : opts) if (kv.second.is<std::string>()) v.push_back(kv.second.as<std::string>());
        rs->waiting = true;
        if (!impl->ui.showChoice) { rs->waiting = false; return; }
        impl->ui.showChoice(v, [impl, rs](int idx) {
            // El índice del choice fluye al script como valor del yield. Lua es 1-based:
            // devolvemos idx+1 (o nil si se canceló con idx<0).
            rs->result  = (idx >= 0 && impl->vm)
                              ? sol::make_object(impl->vm->state(), idx + 1)
                              : sol::object(sol::lua_nil);
            rs->waiting = false;
        });
    });

    // __wait(seconds): pausa la corrutina N segundos (lo descuenta pumpEvents con dt).
    lua.set_function("__wait", [impl](double seconds) {
        Impl::RunningScript* rs = impl->current;
        if (!rs) return;
        rs->waiting  = true;
        rs->waitLeft = static_cast<float>(seconds > 0.0 ? seconds : 0.0);
    });

    // Stubs de acciones secuenciadas aún no implementadas: loguean y reanudan al instante
    // (no bloquean). Demuestran el mecanismo await sin combate/NPCs reales todavía.
    lua.set_function("start_battle", [](sol::optional<std::string> who) {
        LOG_INFO("[lua] start_battle('%s') (stub: reanuda al instante)", who ? who->c_str() : "");
    });
    lua.set_function("move_npc", [](sol::optional<std::string> who, sol::optional<std::string> dir, sol::optional<int> n) {
        LOG_INFO("[lua] move_npc('%s','%s',%d) (stub: reanuda al instante)",
                 who ? who->c_str() : "", dir ? dir->c_str() : "", n ? *n : 0);
    });

    // Wrappers Lua (show_text/show_choice/wait) que llaman a lo de arriba y ceden.
    vm->installEventPrelude();
}

void ScriptSystem::update(Scene& scene, float dt) {
    if (!m_impl->vm) return;
    m_impl->currentScene = &scene;   // visible para player_pos() durante este update

    scene.view<ScriptComponent>().each([&](Entity e, ScriptComponent& sc) {
        Impl::Instance& inst = m_impl->instances[e.id];

        const std::string  full = resolvePath(sc.path);
        std::error_code    ec;
        const fs::file_time_type mtime = fs::last_write_time(full, ec);

        // (Re)cargar si es nuevo, cambió el .lua asignado, o el archivo se editó en
        // disco (hot-reload). Recargar dispara on_start de nuevo (estado fresco).
        const bool firstLoad   = !inst.started || inst.path != sc.path;
        const bool fileChanged = !ec && inst.started && inst.path == sc.path && mtime != inst.mtime;
        if (firstLoad || fileChanged) {
            inst = Impl::Instance{};
            inst.path     = sc.path;
            inst.mtime    = mtime;
            inst.env      = m_impl->vm->loadModule(full);
            inst.onUpdate = inst.env["on_update"];
            sol::protected_function onStart = inst.env["on_start"];
            call(onStart, "on_start", sc.path);
            inst.started = true;
            if (firstLoad) LOG_INFO("Script cargado: %s (hot-reload vigilando: %s)", sc.path.c_str(), full.c_str());
            else           LOG_INFO("Script recargado en caliente: %s", sc.path.c_str());
        } else if (ec) {
            // La lectura del mtime falló (¿archivo movido/bloqueado por el editor?): sin
            // esto el hot-reload queda mudo y parece "que no funciona". Lo avisamos 1 vez.
            if (!inst.mtimeError) {
                LOG_WARN("Hot-reload: no se puede leer la fecha de '%s' (%s).", full.c_str(), ec.message().c_str());
                inst.mtimeError = true;
            }
        } else {
            inst.mtimeError = false;
        }

        call(inst.onUpdate, "on_update", sc.path, ScriptEntity{ &scene, e, &m_impl->walkable }, dt);
    });

    // Tras correr los scripts, el GridMover GOBIERNA la posición de su entidad: una
    // vez sembrado (1er try_step), su Transform = celda actual, interpolada desde la
    // origen mientras hay un paso en curso (deslizamiento suave estilo Pokémon). Quien
    // llamó try_step solo marcó el destino; el avance vive aquí, independiente del
    // script y consistente en todas las entidades. Escribe SIEMPRE (no solo al
    // moverse) para que un consumidor —p.ej. el OverworldMode con su offset visual del
    // jugador— pueda releer la celda lógica fresca cada frame sin que su propio ajuste
    // se realimente.
    scene.view<Transform, GridMover>().each([&](Entity, Transform& tr, GridMover& gm) {
        if (!gm.seeded) return;                        // aún sin sembrar: respeta la pos inicial
        if (gm.moving) {
            gm.t += (gm.stepDuration > 0.0f) ? dt / gm.stepDuration : 1.0f;
            if (gm.t >= 1.0f) { gm.t = 1.0f; gm.moving = false; }
        }
        const Vec2 a(static_cast<float>(gm.from.x), static_cast<float>(gm.from.y));
        const Vec2 b(static_cast<float>(gm.cell.x), static_cast<float>(gm.cell.y));
        tr.position = gm.moving ? glm::mix(a, b, gm.t) : b;
    });
}

void ScriptSystem::clear() {
    m_impl->instances.clear();
    m_impl->running.clear();            // al cambiar de escena: corta los eventos en curso
    m_impl->current = nullptr;
}

void ScriptSystem::setWalkable(std::function<bool(int, int)> fn) { m_impl->walkable = std::move(fn); }

// --- Corrutinas de evento -------------------------------------------------------------

void ScriptSystem::setUIActions(ScriptUIActions actions) { m_impl->ui = std::move(actions); }

bool ScriptSystem::hasFunction(Entity e, const char* fn) const {
    auto it = m_impl->instances.find(e.id);
    if (it == m_impl->instances.end() || !it->second.env.valid()) return false;
    return it->second.env[fn].get_type() == sol::type::function;
}

void ScriptSystem::runEvent(Scene& scene, Entity e, const char* fn) {
    if (!m_impl->vm) return;
    auto it = m_impl->instances.find(e.id);
    if (it == m_impl->instances.end() || !it->second.env.valid()) return;
    sol::protected_function f = it->second.env[fn];
    if (!f.valid()) return;

    sol::state& lua = m_impl->vm->state();
    m_impl->running.emplace_back();
    Impl::RunningScript& rs = m_impl->running.back();
    rs.thread = sol::thread::create(lua);
    // La corrutina corre en el hilo Lua PROPIO del RunningScript (pila Lua aparte): se
    // construye sobre el lua_State* del thread tomando la función del entorno del script.
    // `self` (la misma vista que ve on_update) es el 1er argumento al arrancarla.
    rs.co = sol::coroutine(rs.thread.state().lua_state(), f);
    rs.result  = sol::make_object(lua, ScriptEntity{ &scene, e, &m_impl->walkable });
    rs.waiting = false;
    // NO se arranca aquí: el primer resume lo hace pumpEvents (Engine, fuera de la pila de
    // modos). Así el push del primer DialogueMode no ocurre a mitad de handleInput/update.
}

void ScriptSystem::pumpEvents(float dt) {
    // Caduca los wait() pendientes con el dt del frame.
    for (auto& rs : m_impl->running) {
        if (rs.done || !rs.waiting || rs.waitLeft <= 0.0f) continue;
        rs.waitLeft -= dt;
        if (rs.waitLeft <= 0.0f) { rs.waitLeft = 0.0f; rs.waiting = false; }
    }
    // Reanuda las que ya no esperan. Índice por posición (deque estable: no se reubica al
    // crecer); reapDone al final, nunca a media reanudación.
    for (size_t i = 0; i < m_impl->running.size(); ++i) {
        Impl::RunningScript& rs = m_impl->running[i];
        if (rs.done || rs.waiting) continue;
        m_impl->resume(rs);
    }
    m_impl->reapDone();
}

bool ScriptSystem::eventsActive() const { return !m_impl->running.empty(); }

std::vector<ScriptExport> ScriptSystem::exportsOf(Entity e) const {
    std::vector<ScriptExport> out;
    auto it = m_impl->instances.find(e.id);
    if (it == m_impl->instances.end() || !it->second.env.valid()) return out;

    sol::object exp = it->second.env["exports"];
    if (!exp.is<sol::table>()) return out;
    for (const auto& kv : exp.as<sol::table>()) {
        if (!kv.first.is<std::string>()) continue;
        const sol::object val = kv.second;
        ScriptExport ex;
        ex.name = kv.first.as<std::string>();
        // Orden importa: bool y number ANTES que string (en Lua un número también pasa
        // como string por coerción; los tipos no soportados —tablas, funciones— se omiten).
        if (val.is<bool>())             { ex.type = ScriptExport::Type::Bool;   ex.boolean = val.as<bool>(); }
        else if (val.is<double>())      { ex.type = ScriptExport::Type::Number; ex.number  = val.as<double>(); }
        else if (val.is<std::string>()) { ex.type = ScriptExport::Type::Text;   ex.text    = val.as<std::string>(); }
        else continue;
        out.push_back(std::move(ex));
    }
    return out;
}

void ScriptSystem::setExport(Entity e, const ScriptExport& ex) {
    auto it = m_impl->instances.find(e.id);
    if (it == m_impl->instances.end() || !it->second.env.valid()) return;

    sol::object exp = it->second.env["exports"];
    if (!exp.is<sol::table>()) return;
    sol::table t = exp.as<sol::table>();
    switch (ex.type) {
        case ScriptExport::Type::Bool:   t[ex.name] = ex.boolean; break;
        case ScriptExport::Type::Text:   t[ex.name] = ex.text;    break;
        case ScriptExport::Type::Number: t[ex.name] = ex.number;  break;
    }
}

}  // namespace pk
