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

#include <algorithm>
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
    m_runMode  = RunMode::Edit;          // con editor se abre EDITANDO: nada de mundo corriendo solo
    m_editor.setRunMode(m_runMode);
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
    // overworld, para que éste se vincule a ella. Un proyecto en blanco no trae ninguna: la
    // escena queda vacía y se puebla desde el menú Entidad (el motor no siembra contenido).
    {
        const std::string& start = Project::instance().startScene();
        if (!start.empty()) {
            const std::string abs = Project::instance().resolveRead(start);
            if (m_sceneManager.load(abs))
                LOG_INFO("Escena inicial del proyecto cargada: %s", abs.c_str());
        }
    }

    setupGame();    // carga los datos del proyecto y empuja el overworld

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
            // El extent del renderer (no el tamaño lógico de la ventana) es el espacio
            // en el que la UI debe maquetarse: es el mismo attachment que recibirá
            // EditorUI::render, y el pass del editor usa loadOp=LOAD.
            m_editor.beginFrame(frameTime, m_renderer.uiExtent());
            render(0.0f);                       // pila de modos vacía → solo limpia + UI bienvenida
            std::string projPath, projTemplate; bool isNew = false;
            if (m_editor.consumeProjectChoice(projPath, isNew, projTemplate)) {
                const bool ok = isNew ? Project::instance().newProject(projPath, "", projTemplate)
                                      : Project::instance().openProject(projPath);
                if (ok) { enterProject(); m_editor.setWelcomeMode(false); }
            }
            continue;   // no corras la simulación del juego mientras está la bienvenida
        }
#endif

        // Guardar/abrir/nueva escena: ahora en el menú Archivo del editor (con diálogo
        // nativo). El editor ejecuta la acción sobre el SceneManager + ScriptSystem.

#ifdef ENGINE_EDITOR
        // Play/Pausa/Stop: por los botones de la toolbar o por atajo (F5 alterna jugar/parar,
        // F6 pausa). El atajo se lee del input del JUEGO, que no compite con el teclado del
        // editor (las teclas de función no escriben en ningún campo de texto).
        {
            RunMode req = m_runMode;
            if (m_editor.consumeRunModeRequest(req)) applyRunMode(req);
        }
        if (m_input.wasKeyPressed(Key::F5))
            applyRunMode(m_runMode == RunMode::Edit ? RunMode::Play : RunMode::Edit);
        if (m_input.wasKeyPressed(Key::F6))
            applyRunMode(m_runMode == RunMode::Play ? RunMode::Paused
                       : m_runMode == RunMode::Paused ? RunMode::Play : m_runMode);
#endif

        // La SIMULACIÓN solo avanza en Play. En Edit/Pausa el resto del frame sigue igual
        // (input del editor, variableUpdate para picking/drops, render): lo que se congela
        // es el paso fijo, no el editor.
        if (m_runMode == RunMode::Play) {
            accumulator += frameTime;
            while (accumulator >= kFixedDt) {
                fixedUpdate(static_cast<float>(kFixedDt));
                accumulator -= kFixedDt;
            }
        } else {
            accumulator = 0.0;   // no acumules tiempo mientras está parado (evita un salto al reanudar)
        }

        const float alpha = static_cast<float>(accumulator / kFixedDt);
        variableUpdate(frameTime);

        // Selección a recuperar tras un Stop. Va DESPUÉS de variableUpdate porque el modo
        // del mundo re-vincula ahí la escena restaurada (bindScene) y corre su picking:
        // asignarla antes la dejaría a merced de ese mismo frame.
        if (m_pendingSelIndex >= 0) {
            const std::vector<Entity> all = m_sceneManager.current().allEntities();
            if (m_pendingSelIndex < static_cast<int>(all.size())) {
                m_selection.entity = all[m_pendingSelIndex];   // misma entidad, identidad nueva
                LOG_INFO("Stop: selección recuperada (entidad %u de %zu).",
                         m_selection.entity.id, all.size());
            } else {
                LOG_WARN("Stop: no se pudo recuperar la selección (índice %d de %zu entidades).",
                         m_pendingSelIndex, all.size());
            }
            m_pendingSelIndex = -1;
        }

        m_bus.dispatch();          // entrega los eventos del frame, tras la lógica
#ifdef ENGINE_EDITOR
        m_editor.beginFrame(frameTime, m_renderer.uiExtent());   // NewFrame + construye los paneles
#endif
        render(alpha);             // interpolado entre los dos últimos pasos fijos

#ifdef ENGINE_EDITOR
        // Editor de tiles en ventana OS aparte (T3): abrir bajo demanda, renderizar, cerrar.
        if (m_editor.consumeTileEditorRequest() && !m_tileWindow.isOpen()) {
            m_tileWindow.open(m_editor.uiContext(), "PokeMotor — Editor de tiles", 1280, 1024);
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
    ctx.mode      = m_runMode;              // Edit/Play/Pausa: cada modo se auto-gatea
#ifdef ENGINE_EDITOR
    ctx.uiCapturesMouse = m_editor.wantsInput();   // ratón sobre cualquier panel/barra del editor
    ctx.gizmo = static_cast<GizmoTool>(m_editor.gizmoTool());   // Mover/Rotar/Escalar del dock
    // Rect del viewport → coordenadas del RATÓN. La UI se maqueta en píxeles del
    // framebuffer y el input del juego llega en coordenadas lógicas de ventana: en un
    // display escalado (150 %, etc.) no son la misma unidad y el hit-test fallaría.
    {
        float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;
        m_editor.viewportRect(vx, vy, vw, vh);
        const VkExtent2D ui = m_renderer.uiExtent();
        const float sx = (ui.width  > 0 && m_window.width()  > 0)
                       ? static_cast<float>(m_window.width())  / static_cast<float>(ui.width)  : 1.0f;
        const float sy = (ui.height > 0 && m_window.height() > 0)
                       ? static_cast<float>(m_window.height()) / static_cast<float>(ui.height) : 1.0f;
        ctx.viewportX = vx * sx;  ctx.viewportW = vw * sx;
        ctx.viewportY = vy * sy;  ctx.viewportH = vh * sy;

        // Paneles flotantes del HUD: con el layout overlay el viewport es la ventana entera,
        // así que lo que decide si un clic era para la escena es restarles su rect (misma
        // conversión de píxeles de UI a coordenadas del ratón que el viewport).
        const int n = std::min(m_editor.uiRectCount(), GameContext::kMaxUiRects);
        for (int i = 0; i < n; ++i) {
            float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
            m_editor.uiRectAt(i, rx, ry, rw, rh);
            ctx.uiRects[i] = { rx * sx, ry * sy, rw * sx, rh * sy };
        }
        ctx.uiRectCount = n;
    }
#endif
    ctx.screenW  = m_window.width();
    ctx.screenH  = m_window.height();
    return ctx;
}

void Engine::fixedUpdate(float dt) {
    GameContext ctx = makeGameContext();
    m_game.fixedUpdate(ctx, dt);
}

// Transición de estado pedida por el editor (botones o atajos). De momento solo cambia el
// gate del bucle y avisa por consola; la FASE A2 colgará aquí el snapshot de la escena al
// entrar en Play y su restauración al parar, para que jugar no altere lo que estás editando.
void Engine::applyRunMode(RunMode m) {
    if (m == m_runMode) return;
    const RunMode prev = m_runMode;
    m_runMode = m;
    switch (m) {
        case RunMode::Play:   LOG_INFO("Play: la simulación corre (scripts, pasos, encuentros)."); break;
        case RunMode::Paused: LOG_INFO("Pausa: la simulación está congelada."); break;
        case RunMode::Edit:   LOG_INFO("Stop: vuelta a edición."); break;
    }
    // Empezar a jugar: foto de la escena tal y como la estás editando. Se guarda también
    // qué entidad tenías seleccionada (por ÍNDICE: los Entity de la escena restaurada son
    // otros) y si había cambios sin guardar, para dejarlo todo igual al parar.
    if (prev == RunMode::Edit && m != RunMode::Edit) {
        m_playSnapshot = m_sceneManager.toJson();
        m_playSelIndex = -1;
        const std::vector<Entity> all = m_sceneManager.current().allEntities();
        if (m_selection.has())
            for (size_t i = 0; i < all.size(); ++i)
                if (all[i] == m_selection.entity) { m_playSelIndex = static_cast<int>(i); break; }
#ifdef ENGINE_EDITOR
        m_playDirty = m_editor.sceneDirty();
#endif
        LOG_INFO("Play: foto de la escena (%zu entidades; selección = índice %d).",
                 all.size(), m_playSelIndex);
    }

    if (m == RunMode::Edit) {
        // Parar con un combate/menú/diálogo abierto dejaría ese overlay pegado en pantalla
        // y sin forma de cerrarlo (en edición el juego no recibe input): se cierran hasta
        // dejar el modo base (el overworld). Antes de tocar la escena, que su onExit aún
        // trabaja sobre la que hay.
        GameContext ctx = makeGameContext();
        while (m_game.size() > 1) m_game.pop(ctx);
        // Se olvidan las instancias de script (con sus corrutinas de evento): el siguiente
        // Play vuelve a correr on_start desde cero, como una partida nueva.
        if (m_scriptSystem) m_scriptSystem->clear();

        // Y la escena vuelve a la foto: se deshace TODO lo que la partida movió (posición
        // del jugador, cámara, lo que crearan los scripts). El modo del mundo detecta que
        // el puntero de escena cambió y re-vincula sus entidades por su cuenta.
        if (!m_playSnapshot.is_null()) {
            m_sceneManager.fromJson(m_playSnapshot);
            m_playSnapshot = nlohmann::json{};
            // La selección NO se aplica aquí: este frame todavía tiene que pasar por el
            // modo del mundo (re-vincula la escena y hace su picking), así que se difiere
            // al final del frame — ver m_pendingSelIndex en run().
            m_selection.clear();
            m_pendingSelIndex = m_playSelIndex;
            m_playSelIndex    = -1;
            LOG_INFO("Stop: escena restaurada (%zu entidades); selección pendiente = índice %d.",
                     m_sceneManager.current().allEntities().size(), m_pendingSelIndex);
#ifdef ENGINE_EDITOR
            m_editor.setSceneDirty(m_playDirty);   // jugar no ensucia el proyecto
#endif
        }
    }
#ifdef ENGINE_EDITOR
    m_editor.setRunMode(m_runMode);   // el editor dibuja el estado REAL, no el pedido
#endif
}

void Engine::variableUpdate(float dt) {
    // Cada modo configura el renderer (cámara, sprite, draw items, luces): así el
    // overworld dibuja el tilemap y el combate una pantalla distinta, sin que el
    // Engine sepa en qué modo está.
    GameContext ctx = makeGameContext();
    // El input del JUEGO (menú, interactuar, guardar partida) solo en Play: en edición las
    // teclas son del editor y nada del mundo debe reaccionar a ellas.
    if (m_runMode == RunMode::Play) m_game.handleInput(ctx);
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
    if (m_scriptSystem && m_runMode == RunMode::Play) m_scriptSystem->pumpEvents(dt);
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
