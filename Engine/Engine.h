// Engine/Engine.h — la raíz del motor: posee los subsistemas, gestiona el orden
// de arranque/apagado, y corre el bucle principal (timestep fijo + variable +
// render interpolado). Diseño: MotorGrafico_Engine.md. Cima del grafo de deps.
#pragma once

#include <memory>

#include "Assets/AssetManager.h"
#include "Core/EventBus.h"
#include "Core/Time.h"
#include "Input/ActionMap.h"
#include "Input/Input.h"
#include "Platform/Window.h"
#include "Renderer/Renderer.h"
#include "Renderer/Vulkan/VulkanContext.h"
#include "Scene/Scene.h"
#include "Game/Database.h"
#include "Game/GameStack.h"
#include "Game/SceneManager.h"
#include "Game/Selection.h"

#ifdef ENGINE_EDITOR
#include "Editor/EditorUI.h"
#include "Editor/ToolWindow.h"
#include "Editor/TileEditorUI.h"
#endif

namespace pk {

class LuaVM;
class ScriptSystem;

class Engine {
public:
    Engine();        // definidos en el .cpp: LuaVM (unique_ptr) es tipo incompleto aquí
    ~Engine();
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    bool init();   // arranque en orden de dependencias
    void run();    // bucle principal hasta salir

private:
    // Welcome = mostrando la pantalla de bienvenida (selector de proyecto); Running = juego
    // montado y corriendo. En release (sin editor) se arranca directo en Running.
    enum class AppState { Welcome, Running };

    void setupGame();               // carga datos + empuja el modo overworld
    void enterProject();            // tras elegir proyecto: monta el juego (setupGame + escena)
    GameContext makeGameContext();  // arma el contexto que reciben los modos
    void fixedUpdate(float dt);     // lógica determinista (N pasos)
    void variableUpdate(float dt);  // input + lógica + listas para el renderer
    void render(float alpha);       // dibujo interpolado (alpha = fracción del paso fijo)
    void shutdown();                // apagado en orden inverso (idempotente)

    // Subsistemas (en orden de dependencia).
    Window        m_window;
    VulkanContext m_vulkan;
    AssetManager  m_assets;   // dueño de mallas/texturas/materiales (por handle)
    Renderer      m_renderer;
    SceneManager  m_sceneManager;   // posee la escena activa (entidades + componentes)
#ifdef ENGINE_EDITOR
    EditorUI      m_editor;      // UI del motor (FluentUI), fuera de release
    ToolWindow    m_tileWindow;  // 2ª ventana OS (editor de tiles)
    TileEditor    m_tileEditor;  // estado/UI del editor de tiles (dentro de esa ventana)
#endif
    EventBus      m_bus;
    Input         m_input;
    ActionMap     m_actions;
    Clock         m_clock;

    Database      m_db;       // datos de juego (especies para encuentros, ...)
    GameStack     m_game;     // pila de modos (overworld, combate, ...)
    Selection     m_selection;// selección del editor (compartida modo ↔ inspector)
    std::unique_ptr<LuaVM>       m_lua;          // estado de Lua (sol2) — scripting
    std::unique_ptr<ScriptSystem> m_scriptSystem; // corre los ScriptComponent

    bool         m_running     = false;
    bool         m_initialized = false;
    AppState     m_appState    = AppState::Running;   // editor lo pone en Welcome al arrancar
    Subscription m_keyLogSub;
};

}  // namespace pk
