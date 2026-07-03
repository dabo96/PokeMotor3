// Engine/Engine.cpp — implementación de la raíz del motor.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // SDL_SetMainReady

#include "Engine/Engine.h"
#include "Core/Log.h"
#include "Core/Project.h"
#include "Core/Scripting/LuaVM.h"
#include "Core/Scripting/ScriptSystem.h"
#include "Game/DialogueMode.h"
#include "Game/OverworldMode.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#ifdef ENGINE_EDITOR
#include "Editor/TileEditorUI.h"
#endif

#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

namespace pk {

namespace {
constexpr double kFixedDt = 1.0 / 60.0;  // paso fijo de la sim (60 Hz)

#ifdef POKEMOTOR_VK_VALIDATION
constexpr bool kValidation = true;
#else
constexpr bool kValidation = false;
#endif

// Ancla el directorio de trabajo al del ejecutable: las rutas relativas
// ("Shaders/SPV/...", "Assets/...") resuelven sin importar desde dónde se lance.
void anchorCwdToExe() {
    const char* base = SDL_GetBasePath();
    if (!base) return;
    std::error_code ec;
    std::filesystem::current_path(base, ec);
}
}  // namespace

Engine::Engine() = default;
Engine::~Engine() { shutdown(); }

bool Engine::init() {
    // Arranque en orden de dependencias (shutdown hace lo inverso).
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR("SDL_Init falló: %s", SDL_GetError());
        return false;
    }
    anchorCwdToExe();

    // Raíz de proyecto: el CONTENIDO (escenas, tilesets, sprites, scripts, datos) se
    // resuelve contra el proyecto activo (por defecto "Default" en Documentos), con
    // fallback a los assets del motor. Debe ir ANTES de cargar cualquier contenido.
    Project::instance().init();

    if (!m_window.create("PokeMotor — Engine", 1280, 720)) {
        SDL_Quit();
        return false;
    }
    LOG_INFO("Ventana creada (%dx%d).", m_window.width(), m_window.height());

    if (!m_vulkan.init(m_window.sdl(), kValidation)) {
        LOG_ERROR("VulkanContext init falló");
        m_window.destroy();
        SDL_Quit();
        return false;
    }
    if (!m_assets.init(&m_vulkan)) {
        LOG_ERROR("AssetManager init falló");
        m_vulkan.shutdown();
        m_window.destroy();
        SDL_Quit();
        return false;
    }
    if (!m_renderer.init(&m_vulkan, &m_assets)) {
        LOG_ERROR("Renderer init falló");
        m_assets.shutdown();
        m_vulkan.shutdown();
        m_window.destroy();
        SDL_Quit();
        return false;
    }

    m_renderer.setRenderMode(RenderMode::Sprite2D);   // demo 2D (Plan2D, Paso 1)
    m_sceneManager.init(&m_assets);                   // registra los componentes serializables

    // El scripting se crea ANTES de setupGame: OverworldMode::onEnter (dentro de
    // setupGame) registra su colisión de grid en el ScriptSystem (ctx.scripts).
    m_lua = std::make_unique<LuaVM>();
    m_lua->init();
    m_scriptSystem = std::make_unique<ScriptSystem>();
    m_scriptSystem->init(m_lua.get(), &m_input);

    // Puente scripting → UI del juego: un script de evento (corrutina) pide presentar un
    // diálogo/elección y ESPERA; aquí lo materializamos empujando un DialogueMode a la pila
    // de modos. El `done`/`done(idx)` lo invoca el DialogueMode al cerrarse y reanuda la
    // corrutina. El push ocurre en Engine::variableUpdate (vía pumpEvents), FUERA de la
    // iteración del GameStack, así que es seguro modificar la pila.
    {
        ScriptUIActions actions;
        actions.showText = [this](const std::string& text, std::function<void()> done) {
            auto mode = std::make_unique<DialogueMode>(text);
            mode->setOnDone(std::move(done));
            GameContext ctx = makeGameContext();
            m_game.push(std::move(mode), ctx);
        };
        actions.showChoice = [this](const std::vector<std::string>& opts, std::function<void(int)> done) {
            // El diálogo de elección entrega el índice por onPick (lo llama una vez, con la
            // opción elegida o -1 si se canceló). Texto vacío: solo el menú de opciones.
            auto mode = std::make_unique<DialogueMode>(std::string{}, opts, std::move(done));
            GameContext ctx = makeGameContext();
            m_game.push(std::move(mode), ctx);
        };
        m_scriptSystem->setUIActions(std::move(actions));
    }

    // El JUEGO no se monta aquí: se difiere a enterProject(), que se llama tras elegir
    // proyecto en la pantalla de bienvenida (editor) o directo en release.

#ifdef ENGINE_EDITOR
    if (m_editor.init(m_vulkan, m_window.sdl(), &m_renderer, &m_assets)) {
        m_editor.setSelection(&m_selection);   // el Inspector edita la selección
        m_editor.setEventBus(&m_bus);          // drag-drop de assets → viewport
        m_editor.setSceneManager(&m_sceneManager);   // jerarquía/inspector sobre la escena
        m_editor.setScriptSystem(m_scriptSystem.get());  // exports del script en el inspector
        m_renderer.setUICallback([this](VkCommandBuffer c, VkImageView v, VkExtent2D e) {
            m_editor.render(c, v, e);
        });
    }
#endif

    m_input.init(&m_bus);
    m_actions.bind(Action::MoveUp,    Key::W); m_actions.bind(Action::MoveUp,    Key::Up);
    m_actions.bind(Action::MoveDown,  Key::S); m_actions.bind(Action::MoveDown,  Key::Down);
    m_actions.bind(Action::MoveLeft,  Key::A); m_actions.bind(Action::MoveLeft,  Key::Left);
    m_actions.bind(Action::MoveRight, Key::D); m_actions.bind(Action::MoveRight, Key::Right);
    m_actions.bind(Action::Confirm,   Key::Enter); m_actions.bind(Action::Confirm, Key::Space);
    m_actions.bind(Action::Cancel,    Key::Escape);
    m_actions.bind(Action::Menu,      Key::Tab);    // abre el menú del juego (overlay)

    m_keyLogSub = m_bus.subscribe<KeyPressedEvent>(
        [](const KeyPressedEvent& e) {
            LOG_TRACE("EventBus: KeyPressed (code %d)", (int)e.key);
        });

#ifdef ENGINE_EDITOR
    // Editor: arranca en la pantalla de BIENVENIDA (sin proyecto). El usuario crea/abre uno
    // ahí (eligiendo la ruta); enterProject() monta el juego al confirmarse (ver run()).
    m_appState = AppState::Welcome;
    m_editor.setWelcomeMode(true);
#else
    // Release/juego: el proyecto va empaquetado junto al exe. Abre su .pkproj si existe;
    // si no, usa esa carpeta como raíz tal cual. Luego monta el juego.
    if (const char* base = SDL_GetBasePath()) {
        Project::instance().setRoot(base);
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(base, ec))
            if (e.path().extension() == ".pkproj") { Project::instance().openProject(e.path().string()); break; }
    }
    enterProject();
#endif

    m_initialized = true;
    return true;
}

// Monta el juego para el proyecto activo: carga su escena inicial (si la trae) y empuja el
// overworld. Llamado tras elegir proyecto (bienvenida) o al arrancar en release.
void Engine::enterProject() {
    // Auto-carga la escena inicial del proyecto (lo último guardado) ANTES de empujar el
    // overworld, para que éste se vincule a ella. Sin escena guardada, se siembra el mapa demo.
    bool loadedScene = false;
    {
        const std::string& start = Project::instance().startScene();
        if (!start.empty()) {
            const std::string abs = Project::instance().resolveRead(start);
            if (m_sceneManager.load(abs)) {
                loadedScene = true;
                LOG_INFO("Escena inicial del proyecto cargada: %s", abs.c_str());
            }
        }
    }

    setupGame();    // carga datos y empuja el overworld (crea al jugador-entidad con script)

    // NPCs de prueba TEMPORALES: solo en una escena NUEVA (sin escena guardada). Si se cargó
    // una escena del proyecto, sus entidades ya vienen del disco (no re-sembrar/duplicar).
    if (!loadedScene) {
    // TEMPORAL (verificación UI de juego, Fase 4): un NPC con DialogueComponent en (3,5),
    // a la izquierda del inicio del jugador (5,5) en el mapa de demo. Para probar: da un
    // paso a la IZQUIERDA (queda en (4,5) mirando a la izq) y pulsa Enter mirando al NPC.
    {
        Scene& sc = m_sceneManager.current();
        Entity  e = sc.createEntity();
        sc.add<NameComponent>(e, NameComponent{ "NPC" });
        Transform tr; tr.position = Vec2(3.0f, 5.0f);
        sc.add<Transform>(e, tr);
        SpriteComponent sp;
        sp.texturePath = "Assets/Models/Sprites/001.png";
        sp.tex         = m_assets.loadTexture(sp.texturePath);
        sp.layer       = 1;
        sc.add<SpriteComponent>(e, sp);
        sc.add<DialogueComponent>(e, DialogueComponent{
            "Hola! Bienvenido a PokeMotor. Esta es la caja de dialogo con efecto maquina "
            "de escribir, capaz de paginar texto largo en varias paginas. Pulsa Enter para "
            "avanzar y, al final, para cerrar el dialogo." });
    }

    // TEMPORAL (verificación UI ↔ Scripting): un NPC con SCRIPT de evento en (7,5), a la
    // derecha del inicio del jugador (5,5). Su on_interact corre como corrutina: presenta
    // diálogo + elección con ramas (ver npc_healer.lua). Para probar: da un paso a la
    // DERECHA (queda en (6,5) mirando a la der) y pulsa Enter mirando al NPC.
    {
        Scene& sc = m_sceneManager.current();
        Entity  e = sc.createEntity();
        sc.add<NameComponent>(e, NameComponent{ "Enfermera" });
        Transform tr; tr.position = Vec2(7.0f, 5.0f);
        sc.add<Transform>(e, tr);
        SpriteComponent sp;
        sp.texturePath = "Assets/Models/Sprites/001.png";
        sp.tex         = m_assets.loadTexture(sp.texturePath);
        sp.layer       = 1;
        sc.add<SpriteComponent>(e, sp);
        sc.add<ScriptComponent>(e, ScriptComponent{ "Assets/Scripts/npc_healer.lua" });
    }
    }  // if (!loadedScene)

    m_appState = AppState::Running;
}

void Engine::run() {
    // Patrón "Fix Your Timestep": acumular tiempo real, correr la sim en pasos
    // fijos, lo demás una vez por frame, render interpolado por alpha.
    m_running = true;
    m_clock = Clock{};            // el tiempo de init no cuenta
    double accumulator = 0.0;

    while (m_running && !m_window.shouldClose()) {
        float frameTime = m_clock.tick();
        if (frameTime > 0.25f) frameTime = 0.25f;  // anti spiral-of-death

        // SO → Input → lógica (nunca al revés).
        std::vector<RawEvent> events = m_window.drainEvents();
        m_input.update(events);
#ifdef ENGINE_EDITOR
        m_editor.beginInputFrame();   // limpia flancos del frame previo (antes de los eventos)
        if (m_tileWindow.isOpen()) m_tileWindow.beginInputFrame();
        for (const SDL_Event& e : m_window.rawEvents()) {   // rutea por ventana
            if (m_tileWindow.isOpen() && sdlEventWindowId(e) == m_tileWindow.windowId())
                m_tileWindow.processEvent(e);
            else
                m_editor.processEvent(e);
        }
#endif
        // Escape ya NO cierra el motor: es la acción Cancel de la UI del juego (cerrar
        // menú/diálogo). El cierre va por la X de la ventana (quitRequested).
        if (m_input.quitRequested()) {
            m_running = false;
            break;
        }
#ifdef ENGINE_EDITOR
        if (m_editor.consumeQuitRequest()) {   // "Salir" del menú Archivo → cierre limpio
            m_running = false;
            break;
        }

        // Pantalla de BIENVENIDA: aún no hay proyecto ni juego. Solo dibujamos la UI y
        // esperamos a que el usuario cree/abra un proyecto; entonces montamos el juego.
        if (m_appState == AppState::Welcome) {
            m_editor.beginFrame(frameTime);
            render(0.0f);                       // pila de modos vacía → solo limpia + UI bienvenida
            std::string projPath; bool isNew = false;
            if (m_editor.consumeProjectChoice(projPath, isNew)) {
                const bool ok = isNew ? Project::instance().newProject(projPath, "")
                                      : Project::instance().openProject(projPath);
                if (ok) { enterProject(); m_editor.setWelcomeMode(false); }
            }
            continue;   // no corras la simulación del juego mientras está la bienvenida
        }
#endif

        // Guardar/abrir/nueva escena: ahora en el menú Archivo del editor (con diálogo
        // nativo). El editor ejecuta la acción sobre el SceneManager + ScriptSystem.

        accumulator += frameTime;
        while (accumulator >= kFixedDt) {
            fixedUpdate(static_cast<float>(kFixedDt));
            accumulator -= kFixedDt;
        }

        const float alpha = static_cast<float>(accumulator / kFixedDt);
        variableUpdate(frameTime);
        m_bus.dispatch();          // entrega los eventos del frame, tras la lógica
#ifdef ENGINE_EDITOR
        m_editor.beginFrame(frameTime);   // NewFrame + construye los paneles
#endif
        render(alpha);             // interpolado entre los dos últimos pasos fijos

#ifdef ENGINE_EDITOR
        // Editor de tiles en ventana OS aparte (T3): abrir bajo demanda, renderizar, cerrar.
        if (m_editor.consumeTileEditorRequest() && !m_tileWindow.isOpen()) {
            m_tileWindow.open(m_vulkan, "PokeMotor — Editor de tiles", 1280, 1024);
            // La ventana recrea su backend FluentUI en cada apertura: el atlas registrado
            // en el backend anterior quedó colgante. Forzamos re-registro en el nuevo.
            m_tileEditor.resetAtlasCache();
            m_tileEditor.setWindow(m_tileWindow.window());   // parentar el diálogo de imagen
        }
        if (m_tileWindow.isOpen())
            m_tileWindow.renderFrame(frameTime,
                [this](int w, int h) { m_tileEditor.build(w, h); });
        if (m_tileWindow.isOpen() && m_tileWindow.wantsClose())
            m_tileWindow.close();
#endif
    }

    LOG_INFO("Cerrando.");
}

void Engine::setupGame() {
    m_db.load();                                   // especies (JSON o defaults)
    m_tileEditor.setEventBus(&m_bus);              // guardar en tiles → overworld recarga
    m_tileEditor.setSceneManager(&m_sceneManager); // edita el TileMapComponent de la escena activa
    m_tileEditor.setAssetManager(&m_assets);       // tileset PNG → paleta/lienzo con arte real
    GameContext ctx = makeGameContext();
    m_game.push(std::make_unique<OverworldMode>(), ctx);   // onEnter crea el mapa + la entidad jugador
}

GameContext Engine::makeGameContext() {
    GameContext ctx;
    ctx.scene    = &m_sceneManager.current();
    ctx.assets   = &m_assets;
    ctx.renderer = &m_renderer;
    ctx.bus      = &m_bus;
    ctx.input    = &m_input;
    ctx.actions  = &m_actions;
    ctx.db        = &m_db;
    ctx.stack     = &m_game;
    ctx.scripts   = m_scriptSystem.get();   // API de grid: el overworld registra su colisión
    ctx.selection = &m_selection;
#ifdef ENGINE_EDITOR
    ctx.uiCapturesMouse = m_editor.wantsInput();   // ratón sobre cualquier panel/barra del editor
#endif
    ctx.screenW  = m_window.width();
    ctx.screenH  = m_window.height();
    return ctx;
}

void Engine::fixedUpdate(float dt) {
    GameContext ctx = makeGameContext();
    m_game.fixedUpdate(ctx, dt);
}

void Engine::variableUpdate(float dt) {
    // Cada modo configura el renderer (cámara, sprite, draw items, luces): así el
    // overworld dibuja el tilemap y el combate una pantalla distinta, sin que el
    // Engine sepa en qué modo está.
    GameContext ctx = makeGameContext();
    m_game.handleInput(ctx);
    // El ScriptSystem (on_update de gameplay) lo corre el modo del mundo (OverworldMode)
    // dentro de su variableUpdate, no aquí: así un overlay que congela el mundo (menú,
    // diálogo) pausa también esos scripts (el jugador deja de moverse). Ver
    // OverworldMode::variableUpdate.
    m_game.variableUpdate(ctx, dt);

    // Corrutinas de evento (UI dirigida por script): se reanudan AQUÍ, fuera de la pila de
    // modos. A diferencia del on_update de gameplay, un evento debe poder continuar aunque
    // su modo (overworld) esté congelado bajo el DialogueMode que él mismo empujó: al
    // cerrarse el diálogo, el done() pone waiting=false y este pump reanuda el script. El
    // push de nuevos DialogueModes ocurre aquí, ya fuera de GameStack::variableUpdate.
    if (m_scriptSystem) m_scriptSystem->pumpEvents(dt);
}

void Engine::render(float /*alpha*/) {
    // 'alpha' está disponible para interpolar entre el penúltimo y el último paso fijo
    // cuando haya estado de sim que interpolar. Por ahora el render es directo.

    // La UI del juego (sprites screen-space + texto) es EFÍMERA por frame: la limpiamos
    // antes de que los modos emitan, así al cerrar un overlay (menú/diálogo) su UI no
    // queda "pegada" en el renderer (nadie más la sobreescribe). Cada modo que tenga UI
    // la vuelve a poner en su render().
    m_renderer.setUISprites({});
    m_renderer.setTexts({});

    // Cada modo de la pila emite lo suyo (overworld: tiles + entidades + cámara; overlays
    // como el menú: UI en screen-space), respetando el layering (blocksRenderBelow). El
    // render-feed del ECS vive en OverworldMode::render (no aquí), para que un modo que
    // tape el overworld no dibuje sus entidades.
    GameContext ctx = makeGameContext();
    m_game.render(ctx);

    m_renderer.drawFrame();
}

void Engine::shutdown() {
    if (!m_initialized) return;
#ifdef ENGINE_EDITOR
    m_tileWindow.close();    // cierra la 2ª ventana (si está abierta) antes del device
    m_editor.shutdown();     // DestroyContext de FluentUI (antes que el device)
#endif
    m_renderer.shutdown();   // vkDeviceWaitIdle + destruye lo del renderer
    m_assets.shutdown();     // libera mallas/texturas (antes que el allocator)
    m_vulkan.shutdown();     // swapchain → allocator → device → surface → instance
    m_window.destroy();
    SDL_Quit();
    m_initialized = false;
}

}  // namespace pk
