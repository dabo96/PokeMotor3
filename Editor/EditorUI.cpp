#include "Editor/EditorUI.h"

#include "Assets/AssetManager.h"
#include "Core/EventBus.h"
#include "Core/Log.h"
#include "Core/Project.h"
#include "Core/Scripting/ScriptSystem.h"
#include "Editor/EditorTheme.h"
#include "Game/GameEvents.h"
#include "Game/SceneManager.h"
#include "Game/Selection.h"
#include "Game/TileSet.h"
#include "Renderer/Renderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Renderer/Vulkan/VulkanContext.h"

#include <SDL3/SDL.h>

#include "FluentGUI.h"
#include "core/DragDrop.h"
#include "core/FileDialog.h"
#include "core/RenderBackend.h"
#include "Theme/FluentTheme.h"
#include "UI/Icons.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

// Métricas del layout del editor (compartidas por buildPanels y el test de
// oclusión del ratón). El viewport pickeable es el rectángulo central libre.
constexpr float kTopBand  = 66.0f;   // menubar + toolbar
constexpr float kStatusH  = 24.0f;
constexpr float kLeftW    = 250.0f;
constexpr float kRightW   = 300.0f;
constexpr float kConsoleH = 260.0f;   // panel inferior (Consola/Assets): más alto y cómodo

// Nombre de archivo de una ruta (para etiquetar las miniaturas de assets).
std::string baseName(const std::string& path) {
    const size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? path : path.substr(s + 1);
}

// Busca recursivamente la carpeta cuyo path coincide (para mostrar su contenido).
const pk::AssetNode* findFolder(const pk::AssetNode& n, const std::string& path) {
    if (n.path == path) return &n;
    for (const pk::AssetNode& c : n.children)
        if (c.isFolder)
            if (const pk::AssetNode* r = findFolder(c, path)) return r;
    return nullptr;
}

// Recorta un nombre demasiado largo para la tarjeta (ASCII, sin depender de UTF-8).
std::string ellipsize(const std::string& s, size_t maxLen) {
    return s.size() <= maxLen ? s : s.substr(0, maxLen) + "..";
}

// Icono Lucide por tipo de asset (para el árbol del navegador). Este namespace es
// global (no pk), así que el tipo de pk se trae con un using.
using pk::AssetKind;
uint32_t iconForKind(AssetKind k) {
    switch (k) {
        case AssetKind::Folder: return FluentUI::Icons::Folder;
        case AssetKind::Image:  return FluentUI::Icons::FileImage;
        case AssetKind::Model:  return FluentUI::Icons::Box;
        case AssetKind::Data:   return FluentUI::Icons::FileText;
        case AssetKind::Script: return FluentUI::Icons::FileCode;
        case AssetKind::Font:   return FluentUI::Icons::FileType;
        case AssetKind::Audio:  return FluentUI::Icons::FileMusic;
        default:                return FluentUI::Icons::File;
    }
}

}  // namespace

namespace pk {

bool EditorUI::init(VulkanContext& ctx, SDL_Window* window, Renderer* renderer, AssetManager* assets) {
    m_window   = window;
    m_renderer = renderer;
    m_assets   = assets;
    m_device   = ctx.device();

    // El engine cede sus handles Vulkan; FluentUI graba en SU command buffer
    // (modo compartido) con dynamic rendering al formato de la swapchain.
    FluentUI::VulkanSharedContext shared{};
    shared.instance         = static_cast<void*>(ctx.instance());
    shared.physicalDevice   = static_cast<void*>(ctx.physicalDevice());
    shared.device           = static_cast<void*>(ctx.device());
    shared.graphicsQueue    = static_cast<void*>(ctx.graphicsQueue());
    shared.queueFamilyIndex = ctx.graphicsFamily();
    shared.dynamicRendering = true;
    shared.colorFormat      = static_cast<uint32_t>(ctx.swapchain().ImageFormat());
    shared.sampleCount      = 1;

    FluentUI::SetPreferredBackend(FluentUI::RenderBackendType::Vulkan);
    FluentUI::UIContext* c =
        FluentUI::CreateContext(window, FluentUI::RenderBackendType::Vulkan, &shared);
    if (!c) {
        LOG_ERROR("EditorUI: FluentUI CreateContext falló");
        return false;
    }
    c->style = winuiEditorStyle();   // design system WinUI (winui.css)
    c->renderer.LoadIconFont("assets/fonts/lucide.ttf", 16);

    m_uictx       = c;
    m_initialized = true;
    LOG_INFO("Editor (FluentUI) inicializado");
    return true;
}

void EditorUI::beginInputFrame() {
    if (!m_initialized) return;
    auto* c = static_cast<FluentUI::UIContext*>(m_uictx);
    FluentUI::SetCurrentContext(c);   // coexiste con el contexto de la 2ª ventana
    // Limpia los flancos del frame anterior (mousePressed/keysPressed/...) y
    // snapshotea la posición del ratón. DEBE correr antes de ProcessEvent.
    if (c) c->input.Update(m_window);
}

void EditorUI::processEvent(const SDL_Event& e) {
    if (!m_initialized) return;
    auto* c = static_cast<FluentUI::UIContext*>(m_uictx);
    FluentUI::SetCurrentContext(c);
    if (c) {
        SDL_Event copy = e;                 // ProcessEvent puede tomar no-const
        c->input.ProcessEvent(copy);
    }
}

void EditorUI::beginFrame(float dt) {
    if (!m_initialized) return;

    // Pantalla de bienvenida: aún sin proyecto/juego. Solo dibuja el selector; nada de
    // escena/assets/menús (no hay sobre qué operar).
    if (m_welcome) {
        FluentUI::SetCurrentContext(static_cast<FluentUI::UIContext*>(m_uictx));
        int w = 0, h = 0;
        SDL_GetWindowSize(m_window, &w, &h);
        if (auto* c = FluentUI::GetContext()) c->renderer.SetViewport(w, h);
        FluentUI::NewFrame(dt);
        buildWelcomeScreen(w, h);
        return;
    }

    // Carga diferida (hilo principal) de las texturas pedidas por el diálogo de
    // archivos, cuyo callback puede haber corrido en otro hilo.
    {
        std::vector<std::string> pending;
        { std::lock_guard<std::mutex> lk(m_pendingMutex); pending.swap(m_pendingLoads); }
        bool imported = false;
        for (const std::string& p : pending) {
            if (!m_assets) continue;
            // Importa al proyecto (copia a Assets/Textures) y usa la ruta relativa; si ya
            // estaba dentro, la devuelve sin copiar. Así el asset queda en el proyecto.
            const std::string rel = Project::instance().importAsset(p, "Assets/Textures");
            m_assets->loadTexture(rel.empty() ? p : rel);
            if (!rel.empty()) imported = true;
        }
        if (imported) m_assetsScanned = false;   // re-escanea el navegador para mostrar lo importado
    }

    // Acción del menú Archivo (Nuevo/Abrir/Guardar/Guardar como). Se ejecuta aquí, en
    // el hilo principal y ANTES de dibujar, para no cambiar la escena a mitad de frame.
    {
        SceneOp op = SceneOp::None;
        std::string path;
        {
            std::lock_guard<std::mutex> lk(m_pendingMutex);
            op = m_pendingSceneOp; path = m_pendingScenePath;
            m_pendingSceneOp = SceneOp::None; m_pendingScenePath.clear();
        }
        if (op != SceneOp::None && m_sceneMgr) applySceneOp(op, path);
    }

    FluentUI::SetCurrentContext(static_cast<FluentUI::UIContext*>(m_uictx));
    int w = 0, h = 0;
    SDL_GetWindowSize(m_window, &w, &h);
    if (auto* c = FluentUI::GetContext()) c->renderer.SetViewport(w, h);
    FluentUI::NewFrame(dt);
    buildPanels(dt, w, h);
}

// Pantalla de bienvenida: panel centrado con crear/abrir proyecto + recientes. Los diálogos
// nativos eligen la ruta (nada hardcodeado); la elección la consume el Engine.
void EditorUI::buildWelcomeScreen(int w, int h) {
    using FluentUI::Vec2;
    const float pw = 480.0f, ph = 440.0f;
    const float px = (static_cast<float>(w) - pw) * 0.5f;
    const float py = (static_cast<float>(h) - ph) * 0.5f;
    const float bw = pw - 32.0f;

    if (FluentUI::BeginPanel("Bienvenido a PokeMotor", Vec2(pw, ph), false,
                             std::nullopt, std::nullopt, Vec2(px, py))) {
        FluentUI::Label("PokeMotor", std::nullopt, FluentUI::TypographyStyle::TitleLarge);
        FluentUI::Label("Crea un proyecto nuevo o abre uno existente.",
                        std::nullopt, FluentUI::TypographyStyle::Caption);
        FluentUI::Separator();

        if (FluentUI::Button("Nuevo proyecto…", Vec2(bw, 34.0f))) {
            FluentUI::ShowSaveFileDialog(
                m_window,
                std::vector<FluentUI::FileFilter>{ { "Proyecto PokeMotor", "pkproj" } },
                std::string("MiProyecto.pkproj"),
                [this](const std::vector<std::string>& paths, int) {
                    if (paths.empty()) return;
                    std::lock_guard<std::mutex> lk(m_pendingMutex);
                    m_pendingProject = true; m_pendingProjectIsNew = true; m_pendingProjectPath = paths[0];
                });
        }
        if (FluentUI::Button("Abrir proyecto…", Vec2(bw, 34.0f))) {
            FluentUI::ShowOpenFileDialog(
                m_window,
                std::vector<FluentUI::FileFilter>{ { "Proyecto PokeMotor", "pkproj" } },
                std::string{}, false,
                [this](const std::vector<std::string>& paths, int) {
                    if (paths.empty()) return;
                    std::lock_guard<std::mutex> lk(m_pendingMutex);
                    m_pendingProject = true; m_pendingProjectIsNew = false; m_pendingProjectPath = paths[0];
                });
        }

        FluentUI::Separator();
        FluentUI::Label("Recientes", std::nullopt, FluentUI::TypographyStyle::Subtitle);
        const auto& recents = Project::instance().recentProjects();
        if (recents.empty()) {
            FluentUI::Label("(ninguno todavía)", std::nullopt, FluentUI::TypographyStyle::Caption);
        } else {
            for (const std::string& r : recents) {
                // Etiqueta = ruta completa (única, sirve de ID y es informativa).
                if (FluentUI::Button(r, Vec2(bw, 28.0f))) {
                    std::lock_guard<std::mutex> lk(m_pendingMutex);
                    m_pendingProject = true; m_pendingProjectIsNew = false; m_pendingProjectPath = r;
                }
            }
        }
    }
    FluentUI::EndPanel();
}

bool EditorUI::consumeProjectChoice(std::string& outPath, bool& outIsNew) {
    std::lock_guard<std::mutex> lk(m_pendingMutex);
    if (!m_pendingProject) return false;
    m_pendingProject = false;
    outPath  = m_pendingProjectPath;
    outIsNew = m_pendingProjectIsNew;
    m_pendingProjectPath.clear();
    return true;
}

void EditorUI::buildPanels(float dt, int w, int h) {
    using FluentUI::Vec2;
    if (dt > 0.0f) m_fps = m_fps * 0.9f + (1.0f / dt) * 0.1f;  // FPS suavizado

    // Drop de un asset sobre el VIEWPORT (el drag empezó en una tarjeta del navegador).
    // Se comprueba ANTES de redibujar las tarjetas, porque el DragDropSource cancela el
    // drag al soltar fuera de un target FluentUI. El viewport no es un widget, así que
    // leemos el estado de drag a bajo nivel y publicamos el drop para el modo de juego.
    if (auto* c = FluentUI::GetContext()) {
        auto& dd = c->dragDrop;
        if (dd.active && dd.payloadType == "ASSET_TEXTURE" && c->input.IsMouseReleased(0)) {
            const float mx = c->input.MouseX(), my = c->input.MouseY();
            const bool overViewport = mx >= kLeftW && mx <= static_cast<float>(w) - kRightW
                                   && my >= kTopBand && my <= static_cast<float>(h) - kStatusH - kConsoleH;
            if (overViewport && !m_dragSourcePath.empty() && m_eventBus)
                m_eventBus->emit(AssetDroppedEvent{ m_dragSourcePath, pk::Vec2(mx, my) });
        }
        if (c->input.IsMouseReleased(0)) m_dragSourcePath.clear();   // fin del drag
    }

    // --- Barra de menú ---
    if (FluentUI::BeginMenuBar()) {
        if (FluentUI::BeginMenu("Archivo")) {
            if (FluentUI::MenuItem("Nuevo")) {
                std::lock_guard<std::mutex> lk(m_pendingMutex);
                m_pendingSceneOp = SceneOp::New;
            }
            if (FluentUI::MenuItem("Abrir…")) {
                FluentUI::ShowOpenFileDialog(
                    m_window,
                    std::vector<FluentUI::FileFilter>{ { "Escena PokeMotor", "json" }, { "Todos", "*" } },
                    m_sceneMgr->currentPath().empty() ? Project::instance().resolveWrite("Assets/Data/")
                                                      : m_sceneMgr->currentPath(), false,
                    [this](const std::vector<std::string>& paths, int) {
                        if (paths.empty()) return;
                        std::lock_guard<std::mutex> lk(m_pendingMutex);
                        m_pendingSceneOp = SceneOp::Open; m_pendingScenePath = paths[0];
                    });
            }
            if (FluentUI::MenuItem("Guardar")) {
                std::lock_guard<std::mutex> lk(m_pendingMutex);
                m_pendingSceneOp = SceneOp::Save;   // applySceneOp pide ruta si aún no la hay
            }
            if (FluentUI::MenuItem("Guardar como…")) openSaveDialog();
            FluentUI::MenuSeparator();
            if (FluentUI::MenuItem("Salir")) m_requestQuit = true;   // cierre limpio del motor
            FluentUI::EndMenu();
        }
        if (FluentUI::BeginMenu("Proyecto")) {
            // Un PROYECTO es una carpeta con un .pkproj en la raíz; todo el contenido se
            // resuelve relativo a ella (ver Core/Project). Nuevo/Abrir cambian la raíz activa.
            if (FluentUI::MenuItem("Nuevo proyecto…")) {
                FluentUI::ShowSaveFileDialog(
                    m_window,
                    std::vector<FluentUI::FileFilter>{ { "Proyecto PokeMotor", "pkproj" } },
                    std::string("MiProyecto.pkproj"),
                    [this](const std::vector<std::string>& paths, int) {
                        if (paths.empty()) return;
                        std::lock_guard<std::mutex> lk(m_pendingMutex);
                        m_pendingSceneOp = SceneOp::NewProject; m_pendingScenePath = paths[0];
                    });
            }
            if (FluentUI::MenuItem("Abrir proyecto…")) {
                FluentUI::ShowOpenFileDialog(
                    m_window,
                    std::vector<FluentUI::FileFilter>{ { "Proyecto PokeMotor", "pkproj" } },
                    Project::instance().root(), false,
                    [this](const std::vector<std::string>& paths, int) {
                        if (paths.empty()) return;
                        std::lock_guard<std::mutex> lk(m_pendingMutex);
                        m_pendingSceneOp = SceneOp::OpenProject; m_pendingScenePath = paths[0];
                    });
            }
            FluentUI::EndMenu();
        }
        if (FluentUI::BeginMenu("Editar")) {
            FluentUI::MenuItem("Deshacer"); FluentUI::MenuItem("Rehacer"); FluentUI::EndMenu();
        }
        if (FluentUI::BeginMenu("Ver")) {
            FluentUI::MenuItem("Jerarquía"); FluentUI::MenuItem("Inspector"); FluentUI::MenuItem("Consola");
            FluentUI::EndMenu();
        }
        if (FluentUI::BeginMenu("Entidad")) {
            FluentUI::MenuItem("Crear vacía"); FluentUI::MenuItem("Crear luz"); FluentUI::MenuItem("Crear cámara");
            FluentUI::EndMenu();
        }
        if (FluentUI::BeginMenu("Ventana")) {
            if (FluentUI::MenuItem("Editor de tiles")) m_requestTileEditor = true;
            FluentUI::EndMenu();
        }
        if (FluentUI::BeginMenu("Ayuda")) { FluentUI::MenuItem("Acerca de PokeMotor"); FluentUI::EndMenu(); }
        FluentUI::EndMenuBar();
    }

    // --- Toolbar: gizmo + acciones rápidas ---
    static int gizmo = 0;
    FluentUI::BeginToolbar();
    FluentUI::SegmentedControl("gizmo",
        std::vector<std::pair<std::string, uint32_t>>{
            { "", FluentUI::Icons::Move }, { "", FluentUI::Icons::Rotate }, { "", FluentUI::Icons::Scale } },
        &gizmo);
    FluentUI::SameLine(12.0f);
    FluentUI::IconButton(FluentUI::Icons::Eye);    FluentUI::SameLine();
    FluentUI::IconButton(FluentUI::Icons::Camera); FluentUI::SameLine();
    FluentUI::IconButton(FluentUI::Icons::Box);    FluentUI::SameLine();
    FluentUI::IconButton(FluentUI::Icons::Folder);
    // Toggle del camino HD-2D: ON = sprites Smooth (+UI/texto) a resolución completa
    // compuestos sobre el lowRes; OFF = todo por el lowRes (look retro puro).
    if (m_renderer) {
        FluentUI::SameLine(12.0f);
        bool hd = m_renderer->hd2D();
        if (FluentUI::Checkbox("HD-2D", &hd)) m_renderer->setHD2D(hd);
    }
    FluentUI::EndToolbar();

    // Geometría del layout (modelo overlay: el centro queda libre para la escena).
    const float top       = kTopBand;
    const float statusH   = kStatusH;
    const float leftW     = kLeftW;
    const float rightW    = kRightW;
    const float consoleH  = kConsoleH;
    const float midH      = static_cast<float>(h) - top - statusH;
    const float leftBodyH = midH - consoleH;

    // --- Jerarquía (izquierda): lista viva de entidades de la escena ---
    if (FluentUI::BeginPanel("Jerarquía", Vec2(leftW, leftBodyH), false,
                             std::nullopt, std::nullopt, Vec2(0.0f, top))) {
        if (FluentUI::BeginTreeView("hierarchy", Vec2(leftW - 16.0f, leftBodyH - 44.0f))) {
            if (m_sceneMgr && m_selection) {
                Scene& s = m_sceneMgr->current();
                for (Entity e : s.allEntities()) {
                    const std::string label = s.has<NameComponent>(e)
                        ? s.get<NameComponent>(e).value
                        : ("Entidad " + std::to_string(e.id));
                    const std::string nodeId = "e" + std::to_string(e.id) + "_" + std::to_string(e.generation);
                    bool sel = (m_selection->entity == e);
                    const bool was = sel;
                    FluentUI::TreeNode(nodeId, label, FluentUI::Icons::Box, nullptr, &sel);
                    if (sel && !was) m_selection->entity = e;   // recién clicado → selección única
                }
            }
        }
        FluentUI::EndTreeView();
    }
    FluentUI::EndPanel();

    // --- Inspector (derecha): componentes de la entidad seleccionada ---
    if (FluentUI::BeginPanel("Inspector", Vec2(rightW, midH), false,
                             std::nullopt, std::nullopt, Vec2(static_cast<float>(w) - rightW, top))) {
        static bool oTransform = true, oAppear = true, oScript = true, oCamera = true;
        Scene* s = m_sceneMgr ? &m_sceneMgr->current() : nullptr;

        if (m_selection && s && m_selection->has() && s->alive(m_selection->entity)) {
            const Entity e = m_selection->entity;
            const std::string name = s->has<NameComponent>(e) ? s->get<NameComponent>(e).value : "Entidad";
            FluentUI::Label(name, std::nullopt, FluentUI::TypographyStyle::Subtitle);
            FluentUI::Separator();

            if (s->has<Transform>(e) && FluentUI::CollapsingHeader("Transform", &oTransform)) {
                Transform& tr = s->get<Transform>(e);
                float pos[3] = { tr.position.x, tr.position.y, 0.0f };
                if (FluentUI::DragFloat3("Posición", pos, 0.05f)) tr.position = pk::Vec2(pos[0], pos[1]);
                float rot[3] = { 0.0f, 0.0f, tr.rotation * 57.29578f };   // rad → grados (Z en 2D)
                if (FluentUI::DragFloat3("Rotación", rot, 1.0f)) tr.rotation = rot[2] * 0.01745329f;
                float scl[3] = { tr.scale.x, tr.scale.y, 1.0f };
                if (FluentUI::DragFloat3("Escala", scl, 0.01f)) tr.scale = pk::Vec2(scl[0], scl[1]);
            }
            if (s->has<SpriteComponent>(e) && FluentUI::CollapsingHeader("Apariencia", &oAppear)) {
                SpriteComponent& sp = s->get<SpriteComponent>(e);
                FluentUI::Color c(sp.tint.x, sp.tint.y, sp.tint.z, sp.tint.w);
                if (FluentUI::ColorPicker("Tinte", &c)) sp.tint = pk::Vec4(c.r, c.g, c.b, c.a);
                FluentUI::SliderFloat("Opacidad", &sp.tint.w, 0.0f, 1.0f);
                FluentUI::DragInt("Capa", &sp.layer, 0.1f, 0, 16);
                // Calidad/filtro del sprite (por asset): Pixel = pixel-art nearest sin mips;
                // Suave = lineal + mipmaps para arte HD. El sampler y los mips viven en la
                // Texture, así que al cambiarlo RE-RESOLVEMOS la textura con el nuevo
                // FilterMode (AssetManager cachea por "ruta|filtro") y actualizamos el handle
                // para que el cambio se vea en vivo.
                int fil = (sp.filter == FilterMode::Smooth) ? 1 : 0;
                if (FluentUI::SegmentedControl("spriteFilter",
                        std::vector<std::string>{ "Pixel", "Suave" }, &fil)) {
                    sp.filter = (fil == 1) ? FilterMode::Smooth : FilterMode::Pixel;
                    if (m_assets && !sp.texturePath.empty())
                        sp.tex = m_assets->loadTexture(sp.texturePath, true, true, sp.filter);
                }
            }
            // Sección Cámara: aparece en la entidad-cámara (CameraComponent). El CENTRO se
            // edita arriba en Transform→Posición; aquí solo el zoom (px/tile). El SEGUIMIENTO
            // del jugador es un export del Script (camera_follow.lua → 'follow').
            if (s->has<CameraComponent>(e) && FluentUI::CollapsingHeader("Cámara", &oCamera)) {
                CameraComponent& cam = s->get<CameraComponent>(e);
                FluentUI::DragFloat("Zoom (px/tile)", &cam.zoom, 0.5f, 4.0f, 256.0f);
            }
            // Sección Script: aparece si la entidad tiene un .lua adjunto (aunque no
            // declare exports). Muestra la ruta y las variables de su tabla 'exports'
            // (número/bool/texto), editables en vivo.
            if (m_scriptSys && s->has<ScriptComponent>(e) &&
                FluentUI::CollapsingHeader("Script", &oScript)) {
                FluentUI::Label(s->get<ScriptComponent>(e).path);
                std::vector<ScriptExport> exps = m_scriptSys->exportsOf(e);
                if (exps.empty()) {
                    FluentUI::Label("Sin variables export.");
                } else {
                    for (ScriptExport& ex : exps) {
                        switch (ex.type) {
                            case ScriptExport::Type::Number: {
                                float v = static_cast<float>(ex.number);
                                if (FluentUI::DragFloat(ex.name, &v, 0.05f)) {
                                    ex.number = static_cast<double>(v);
                                    m_scriptSys->setExport(e, ex);
                                }
                                break;
                            }
                            case ScriptExport::Type::Bool:
                                if (FluentUI::Checkbox(ex.name, &ex.boolean))
                                    m_scriptSys->setExport(e, ex);
                                break;
                            case ScriptExport::Type::Text:
                                if (FluentUI::TextInput(ex.name, &ex.text))
                                    m_scriptSys->setExport(e, ex);
                                break;
                        }
                    }
                }
            }
        } else {
            FluentUI::Label("Nada seleccionado", std::nullopt, FluentUI::TypographyStyle::Subtitle);
            FluentUI::Separator();
            FluentUI::Label("Click en una entidad de la");
            FluentUI::Label("jerarquía o del viewport.");
        }
    }
    FluentUI::EndPanel();

    // --- Panel inferior (Consola / Assets) ---
    // TabView colocado DIRECTAMENTE (sin envolverlo en un BeginPanel): así no gastamos
    // altura en el header/título del panel y el contenido ocupa toda la franja inferior.
    const float consoleW = static_cast<float>(w) - rightW;
    if (FluentUI::BeginTabView("bottomTabs", &m_bottomTab,
                               std::vector<std::string>{ "Consola", "Assets" },
                               Vec2(consoleW, consoleH), Vec2(0.0f, top + leftBodyH))) {
        if (m_bottomTab == 0) {
            if (FluentUI::BeginScrollView("console_scroll", Vec2(consoleW - 24.0f, consoleH - 56.0f))) {
                // Log real del motor (Core/Log), recientes al final.
                std::vector<LogEntry> entries;
                logSnapshot(entries);
                for (const LogEntry& e : entries) {
                    const char* tag = e.level == LogLevel::Error ? "[err ] "
                                    : e.level == LogLevel::Warn  ? "[warn] "
                                    : e.level == LogLevel::Trace ? "[trace] " : "[info] ";
                    FluentUI::Label(tag + e.text);
                }
            }
            FluentUI::EndScrollView();
        } else {
            // Pestaña Assets: navegador de la carpeta Assets/ DEL PROYECTO activo.
            const std::string assetsDir = Project::instance().resolveWrite("Assets");
            if (!m_assetsScanned) { m_assetBrowser.refresh(assetsDir, "Assets"); m_assetsScanned = true; }

            if (FluentUI::Button("Refrescar")) { m_assetBrowser.refresh(assetsDir, "Assets"); m_selectedFolder = "Assets"; }
            FluentUI::SameLine(8.0f);
            if (FluentUI::Button("Cargar textura…")) {
                FluentUI::ShowOpenFileDialog(
                    m_window,
                    std::vector<FluentUI::FileFilter>{ { "Imágenes", "png;jpg;bmp" }, { "Todos", "*" } },
                    assetsDir, false,
                    [this](const std::vector<std::string>& paths, int) {
                        std::lock_guard<std::mutex> lk(m_pendingMutex);   // callback puede ser de otro hilo
                        for (const std::string& p : paths) m_pendingLoads.push_back(p);
                    });
            }

            const float areaH = consoleH - 76.0f;            // bajo la barra de tabs + botones
            const float treeW = 168.0f;                      // ancho del árbol de carpetas
            const float gridW = consoleW - treeW - 28.0f;    // resto para el grid

            FluentUI::BeginHorizontal(8.0f);
            if (FluentUI::BeginTreeView("foldersTree", Vec2(treeW, areaH)))
                drawFolderTree(m_assetBrowser.root());
            FluentUI::EndTreeView();

            if (FluentUI::BeginScrollView("assetGrid", Vec2(gridW, areaH))) {
                if (const AssetNode* f = findFolder(m_assetBrowser.root(), m_selectedFolder))
                    drawAssetCards(*f, gridW);
            }
            FluentUI::EndScrollView();
            FluentUI::EndHorizontal();
        }
    }
    FluentUI::EndTabView();

    // --- Barra de estado ---
    const char* gz = (gizmo == 1) ? "Rotar" : (gizmo == 2) ? "Escalar" : "Mover";
    char status[128];
    std::snprintf(status, sizeof(status), "PokeMotor · modo 2D · Gizmo: %s · FPS %.0f", gz, m_fps);
    FluentUI::BeginStatusBar(status);
    FluentUI::EndStatusBar();
}

void EditorUI::render(VkCommandBuffer cmd, VkImageView swapchainView, VkExtent2D extent) {
    if (!m_initialized) return;
    FluentUI::SetCurrentContext(static_cast<FluentUI::UIContext*>(m_uictx));

    VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView   = swapchainView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;   // preserva el render de la escena
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea           = { { 0, 0 }, extent };
    ri.layerCount           = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &color;
    vkCmdBeginRendering(cmd, &ri);

    if (FluentUI::RenderBackend* be = FluentUI::GetBackend())
        be->SetFrameCommandBuffer(static_cast<void*>(cmd));
    FluentUI::RenderDeferredDropdowns();   // menús/combos/tooltips por encima
    FluentUI::Render();

    vkCmdEndRendering(cmd);
}

bool EditorUI::wantsInput() const {
    return m_initialized && FluentUI::WantCaptureMouse();
}

bool EditorUI::consumeTileEditorRequest() {
    const bool r = m_requestTileEditor;
    m_requestTileEditor = false;
    return r;
}

bool EditorUI::consumeQuitRequest() {
    const bool r = m_requestQuit;
    m_requestQuit = false;
    return r;
}

void EditorUI::applySceneOp(SceneOp op, const std::string& path) {
    // Cargar/nueva escena cambia la escena activa: hay que olvidar las instancias de
    // script (estado viejo) y la selección (la entidad ya no vive). La ruta actual la
    // recuerda el SceneManager (currentPath), fuente única para "Guardar".
    switch (op) {
        case SceneOp::New:
            m_sceneMgr->newScene();   // limpia currentPath
            if (m_scriptSys) m_scriptSys->clear();
            if (m_selection) m_selection->clear();
            LOG_INFO("Escena nueva.");
            break;
        case SceneOp::Open:
            if (m_sceneMgr->load(path)) {   // fija currentPath
                if (m_scriptSys) m_scriptSys->clear();
                if (m_selection) m_selection->clear();
                LOG_INFO("Escena abierta: %s", path.c_str());
            } else {
                LOG_WARN("No se pudo abrir la escena: %s", path.c_str());
            }
            break;
        case SceneOp::Save:
            // Primera vez (escena sin ruta): se comporta como "Guardar como" y pide
            // dónde. Ya con ruta, guarda ahí directamente, sin preguntar.
            if (m_sceneMgr->currentPath().empty()) { openSaveDialog(); break; }
            if (m_sceneMgr->save(m_sceneMgr->currentPath())) LOG_INFO("Escena guardada: %s", m_sceneMgr->currentPath().c_str());
            else                                             LOG_WARN("No se pudo guardar la escena.");
            break;
        case SceneOp::SaveAs:
            if (m_sceneMgr->save(path)) LOG_INFO("Escena guardada como: %s", path.c_str());   // fija currentPath
            else                        LOG_WARN("No se pudo guardar la escena: %s", path.c_str());
            break;
        case SceneOp::NewProject:
            // Crea el .pkproj + esqueleto de carpetas y fija la raíz; arranca con escena vacía
            // (el overworld siembra el mapa por defecto al re-vincularse; los assets de demo
            // cargan por fallback al motor).
            if (Project::instance().newProject(path, "")) {
                m_sceneMgr->newScene();
                refreshAfterProjectChange();
                LOG_INFO("Proyecto nuevo: %s", path.c_str());
            } else {
                LOG_WARN("No se pudo crear el proyecto: %s", path.c_str());
            }
            break;
        case SceneOp::OpenProject:
            // Fija la raíz = carpeta del .pkproj y carga su escena inicial si la declara.
            if (Project::instance().openProject(path)) {
                const std::string& start = Project::instance().startScene();
                const std::string startAbs = start.empty() ? std::string{} : Project::instance().resolveRead(start);
                if (startAbs.empty() || !m_sceneMgr->load(startAbs)) m_sceneMgr->newScene();
                refreshAfterProjectChange();
                LOG_INFO("Proyecto abierto: %s", path.c_str());
            } else {
                LOG_WARN("No se pudo abrir el proyecto: %s", path.c_str());
            }
            break;
        case SceneOp::None:
            break;
    }
}

// Tras cambiar de proyecto (Nuevo/Abrir): la escena ya se reseteó/cargó arriba. Aquí
// olvidamos scripts/selección (estado de la escena vieja), re-resolvemos el TileSet contra
// la nueva raíz, avisamos al overworld para que recargue el tileset, y re-escaneamos el
// navegador de assets (apunta a la carpeta del proyecto).
void EditorUI::refreshAfterProjectChange() {
    if (m_scriptSys) m_scriptSys->clear();
    if (m_selection) m_selection->clear();
    TileSet::instance().load("Assets/Data/tileset.json");   // registro de tipos del nuevo proyecto (o fallback)
    if (m_eventBus) m_eventBus->emit(MapSavedEvent{});       // overworld: recarga tileset + reconstruye sprites
    m_assetsScanned = false;                                 // el grid de assets se re-escanea al siguiente frame
}

void EditorUI::openSaveDialog() {
    FluentUI::ShowSaveFileDialog(
        m_window,
        std::vector<FluentUI::FileFilter>{ { "Escena PokeMotor", "json" } },
        m_sceneMgr->currentPath().empty() ? Project::instance().resolveWrite("Assets/Data/scene.json")
                                          : m_sceneMgr->currentPath(),
        [this](const std::vector<std::string>& paths, int) {
            if (paths.empty()) return;                       // el usuario canceló
            std::lock_guard<std::mutex> lk(m_pendingMutex);  // callback puede ser de otro hilo
            m_pendingSceneOp = SceneOp::SaveAs; m_pendingScenePath = paths[0];
        });
}

void* EditorUI::thumbnailFor(VkImageView view) {
    if (view == VK_NULL_HANDLE) return nullptr;
    auto it = m_thumb.find(view);
    if (it != m_thumb.end()) return it->second;
    // Envuelve la VkImageView del AssetManager en un descriptor de FluentUI (no la
    // destruye: external=true). Se cachea por view y se libera en shutdown().
    void* h = FluentUI::RegisterExternalTexture(reinterpret_cast<void*>(view), nullptr, 0);
    m_thumb[view] = h;
    return h;
}

// Columna izquierda: árbol de SOLO carpetas. Al seleccionar una, su contenido se
// muestra como tarjetas en la columna derecha (m_selectedFolder).
void EditorUI::drawFolderTree(const AssetNode& node) {
    for (const AssetNode& child : node.children) {
        if (!child.isFolder) continue;
        bool& open = m_folderOpen[child.path];           // colapsada por defecto
        bool  sel  = (m_selectedFolder == child.path);
        FluentUI::TreeNode(child.path, child.name, FluentUI::Icons::Folder, &open, &sel);
        if (sel) m_selectedFolder = child.path;
        if (open) {                                       // 'open' leído antes de recursar
            FluentUI::TreeNodePush();
            drawFolderTree(child);
            FluentUI::TreeNodePop();
        }
    }
}

// Columna derecha: el contenido de la carpeta seleccionada como tarjetas uniformes
// (miniatura para imágenes — carga perezosa —, icono para el resto). Layout propio en
// filas de N columnas (no usa el BeginGrid, que no contiene las celdas).
void EditorUI::drawAssetCards(const AssetNode& folder, float gridW) {
    using FluentUI::Vec2;
    constexpr float kCardW = 88.0f, kCardH = 96.0f, kThumb = 64.0f;
    const int cols = std::max(1, static_cast<int>(gridW / (kCardW + 6.0f)));

    int i = 0;
    for (const AssetNode& child : folder.children) {
        if (i % cols == 0) {
            if (i != 0) FluentUI::EndHorizontal();
            FluentUI::BeginHorizontal(6.0f);
        }

        FluentUI::BeginVertical(2.0f, Vec2(kCardW, kCardH));
        if (child.kind == AssetKind::Image && m_assets) {
            // Miniatura: carga perezosa (cacheada por ruta) y envuelta para FluentUI.
            const TextureHandle h = m_assets->loadTexture(child.path);
            Texture*    t  = m_assets->getTexture(h);
            void*       th = t ? thumbnailFor(t->view()) : nullptr;
            if (th) {
                FluentUI::Image("ic_" + child.path, th, Vec2(kThumb, kThumb));
                // Marca la tarjeta CLICADA como fuente del drag: todas comparten el
                // payloadType "ASSET_TEXTURE", así que sin esto el payload/preview se
                // sobrescribe con la última tarjeta del loop.
                if (auto* cc = FluentUI::GetContext(); cc && cc->input.IsMousePressed(0)) {
                    const Vec2 ip = cc->lastItemPos, is = cc->lastItemSize;
                    const float mx = cc->input.MouseX(), my = cc->input.MouseY();
                    if (mx >= ip.x && mx <= ip.x + is.x && my >= ip.y && my <= ip.y + is.y)
                        m_dragSourcePath = child.path;
                }
                FluentUI::DragDropSource src("ASSET_TEXTURE");   // arrástrala al viewport
                if (src.IsActive() && child.path == m_dragSourcePath) {
                    src.SetPayload(child.path);
                    // Preview: la miniatura sigue al cursor mientras arrastras.
                    src.DragPreview([th]() {
                        FluentUI::Image("drag_preview", th, FluentUI::Vec2(48.0f, 48.0f));
                    });
                }
            } else {
                FluentUI::IconButton(iconForKind(child.kind), kThumb);
            }
        } else if (child.isFolder) {
            // Tarjeta de carpeta: click → entrar (además del árbol de la izquierda).
            if (FluentUI::IconButton(FluentUI::Icons::Folder, kThumb))
                m_selectedFolder = child.path;
        } else {
            FluentUI::IconButton(iconForKind(child.kind), kThumb);
        }
        FluentUI::Label(ellipsize(child.name, 11));
        FluentUI::EndVertical();
        ++i;
    }
    if (i != 0) FluentUI::EndHorizontal();
}

void EditorUI::shutdown() {
    if (!m_initialized) return;
    FluentUI::SetCurrentContext(static_cast<FluentUI::UIContext*>(m_uictx));
    if (m_device) vkDeviceWaitIdle(m_device);
    for (auto& kv : m_thumb)
        if (kv.second) FluentUI::DestroyExternalTexture(kv.second);
    m_thumb.clear();
    FluentUI::DestroyContext();
    m_initialized = false;
}

}  // namespace pk
