// Game/GameMode.h — interfaz de un modo de juego + el contexto que recibe.
// La lógica de juego NO posee los subsistemas; los toma prestados por puntero a
// través del GameContext que la Application (Engine) rellena cada frame.
// Diseño: MotorGrafico_IndiceMaestro.md (Fase 4 — pila de modos).
#pragma once

#include "Game/RunMode.h"

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
// Herramienta de manipulación activa en el editor: decide qué gizmo se dibuja sobre la
// entidad seleccionada (y, cuando exista el arrastre, qué hace).
enum class GizmoTool { Select, Move, Rotate, Scale };

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
    // Estado de ejecución del frame. Sin editor siempre es Play; con editor lo fija el
    // usuario (Play/Pausa/Stop). Un modo consulta esto para no simular en edición.
    RunMode          mode     = RunMode::Play;
    bool             uiCapturesMouse = false;  // el ratón está sobre la UI del editor
    GizmoTool        gizmo    = GizmoTool::Select;   // herramienta elegida en el dock del editor
    int              screenW  = 0;
    int              screenH  = 0;
    // Rect del VIEWPORT de escena en píxeles de ventana. w/h a 0 = sin editor: el viewport
    // es la ventana entera. Criterio GEOMÉTRICO para el picking: uiCapturesMouse se calcula
    // del hover que la UI publicó frames atrás, así que un clic rápido sobre un botón puede
    // llegar aquí como si fuese sobre la escena.
    float            viewportX = 0.0f, viewportY = 0.0f, viewportW = 0.0f, viewportH = 0.0f;

    // Paneles del editor que FLOTAN SOBRE el viewport (HUD): con el layout overlay el
    // viewport es casi toda la ventana, así que el rect de arriba ya no basta para saber si
    // un clic era para la escena — hay que restar además lo que la UI tapa. Son los rects
    // del frame anterior (mismo lag de 1 frame que viewport*, imperceptible al clicar).
    static constexpr int kMaxUiRects = 12;
    struct UiRect { float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f; };
    UiRect           uiRects[kMaxUiRects] = {};
    int              uiRectCount = 0;

    bool pointInViewport(float px, float py) const {
        if (viewportW > 0.0f && viewportH > 0.0f) {
            if (px < viewportX || px > viewportX + viewportW ||
                py < viewportY || py > viewportY + viewportH) return false;
        }
        for (int i = 0; i < uiRectCount; ++i) {   // ...y no sobre un panel flotante
            const UiRect& r = uiRects[i];
            if (px >= r.x && px <= r.x + r.w && py >= r.y && py <= r.y + r.h) return false;
        }
        return true;   // sin editor (rect nulo y sin overlays): todo es escena
    }
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
