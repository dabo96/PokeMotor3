// Game/GameMode.h — interfaz de un modo de juego + el contexto que recibe.
// La lógica de juego NO posee los subsistemas; los toma prestados por puntero a
// través del GameContext que la Application (Engine) rellena cada frame.
// Diseño: MotorGrafico_IndiceMaestro.md (Fase 4 — pila de modos).
#pragma once

namespace pk {

class Scene;
class AssetManager;
class Renderer;
class EventBus;
class Input;
class ActionMap;
class Database;
class GameStack;
class ScriptSystem;
struct Selection;

// Todo lo que un modo necesita del motor. Los punteros persistentes (scene,
// assets, ...) y los volátiles del frame (input, tamaño de pantalla) los pone
// el Engine antes de invocar al modo.
struct GameContext {
    Scene*           scene    = nullptr;
    AssetManager*    assets   = nullptr;
    Renderer*        renderer = nullptr;
    EventBus*        bus      = nullptr;
    const Input*     input    = nullptr;
    const ActionMap* actions  = nullptr;
    Database*        db       = nullptr;
    GameStack*       stack    = nullptr;   // para que un modo empuje/saque otros
    ScriptSystem*    scripts  = nullptr;   // API de scripting (p.ej. registrar la colisión de grid)
    Selection*       selection = nullptr;  // selección del editor (picking ↔ inspector)
    bool             uiCapturesMouse = false;  // el ratón está sobre la UI del editor
    int              screenW  = 0;
    int              screenH  = 0;
};

class GameMode {
public:
    virtual ~GameMode() = default;
    virtual void onEnter(GameContext&) {}
    virtual void onExit(GameContext&) {}
    virtual void handleInput(GameContext&) {}
    virtual void fixedUpdate(GameContext&, float /*dt*/) {}
    virtual void variableUpdate(GameContext&, float /*dt*/) {}
    virtual void render(GameContext&) {}     // emite sprites/UI al renderer (separado de la lógica)

    // Layering de la pila: ¿este modo congela la lógica del de abajo? ¿lo tapa al pintar?
    // Por defecto un modo empujado pausa y tapa al anterior (combate); un overlay como un
    // menú/diálogo deja ver el mundo detrás (override blocksRenderBelow → false).
    virtual bool blocksUpdateBelow() const { return true; }
    virtual bool blocksRenderBelow() const { return true; }

    // Auto-cierre seguro: el modo pide salir y el GameStack lo saca DESPUÉS de actualizar
    // (nunca un pop desde dentro del propio update, que se autodestruiría a media función).
    virtual bool wantsPop() const { return false; }
};

}  // namespace pk
