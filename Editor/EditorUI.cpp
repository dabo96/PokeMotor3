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
#include "core/PlatformBackend.h"    // CreateHostPlatform + CustomTitleBarHitTest (TitleBar)
#include "core/RenderBackend.h"
#include "core/SDLPlatform.h"   // ProcessSDLEvent: traduce SDL_Event → UIEvent (core desacoplado de SDL)
#include "Theme/FluentTheme.h"
#include "UI/Icons.h"
#include "UI/WidgetHelpers.h"   // PushID/PopID (brief 21): no lo reexporta el umbrella FluentGUI.h

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

// Métricas del layout del editor (compartidas por buildPanels y el test de
// oclusión del ratón). El viewport pickeable es el rectángulo central libre.
constexpr float kTopBand  = 74.0f;   // fallback del alto del chrome (TitleBar+toolbar) si no hay contexto
constexpr float kStatusH  = 24.0f;
constexpr float kPanePad  = 8.0f;    // margen interior de los panes del Splitter

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

// "Assets/Textures/Chars" → { "Assets", "Textures", "Chars" } (migas de pan).
std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t sep = path.find_first_of("/\\", start);
        const std::string part = path.substr(start, sep == std::string::npos ? std::string::npos
                                                                             : sep - start);
        if (!part.empty()) out.push_back(part);
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    if (out.empty()) out.push_back(path);
    return out;
}

// Carpeta que contiene a `path` ("Assets/Textures/a.png" → "Assets/Textures").
std::string parentDir(const std::string& path) {
    const size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? std::string("Assets") : path.substr(0, s);
}

// Rutas de assets (no carpetas) cuyo NOMBRE contiene `q`, sin distinguir mayúsculas.
// Recorre el árbol completo: es el buscador global del navegador.
void collectAssetMatches(const pk::AssetNode& node, const std::string& qLower,
                         std::vector<std::string>& out, size_t maxHits) {
    for (const pk::AssetNode& c : node.children) {
        if (out.size() >= maxHits) return;
        if (c.isFolder) { collectAssetMatches(c, qLower, out, maxHits); continue; }
        std::string nameLower = c.name;
        for (char& ch : nameLower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (qLower.empty() || nameLower.find(qLower) != std::string::npos) out.push_back(c.path);
    }
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

// Escribe el esqueleto de un script de entidad en `absPath` (que aún no existe). Es el
// punto de partida de "Nuevo script…": documenta el ciclo de vida (on_start/on_update/
// on_interact), la tabla `exports` que el inspector edita y el hot-reload, para no tener
// que copiar otro .lua a mano.
bool writeScriptTemplate(const std::string& absPath) {
    std::ofstream f(absPath, std::ios::binary);
    if (!f) return false;
    const std::string name = baseName(absPath);
    f << "-- " << name << " — script de entidad (PokeMotor).\n"
      << "-- Corre en su propio entorno: lo que definas aquí no afecta a otros scripts.\n"
      << "-- Guardar el archivo lo RECARGA en caliente, sin reiniciar el motor.\n"
      << "\n"
      << "-- Variables editables desde el inspector (número, booleano o texto).\n"
      << "exports = {\n"
      << "    velocidad = 4.0,\n"
      << "}\n"
      << "\n"
      << "-- Una vez, al adjuntarse el script (y en cada recarga en caliente).\n"
      << "function on_start(self)\n"
      << "    log(\"" << name << ": on_start\")\n"
      << "end\n"
      << "\n"
      << "-- Cada frame. dt = segundos desde el frame anterior.\n"
      << "function on_update(self, dt)\n"
      << "    -- local dx, dy = move_axis()      -- teclado (WASD / flechas)\n"
      << "    -- self:move(dx * exports.velocidad * dt, dy * exports.velocidad * dt)\n"
      << "    -- self:try_step(dx, dy)           -- un paso de casilla (respeta la colisión)\n"
      << "end\n"
      << "\n"
      << "-- Al interactuar de frente (Enter/Espacio). Puede PAUSAR: show_text, show_choice\n"
      << "-- y wait ceden, y el script sigue cuando la UI se cierra.\n"
      << "-- function on_interact(self)\n"
      << "--     show_text(\"¡Hola!\")\n"
      << "-- end\n";
    return f.good();
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

    // Puerto de plataforma: el motor ya inicializó SDL, así que usamos CreateHostPlatform
    // (NO posee SDL → no hace SDL_Quit al destruirse). Da al contexto las operaciones de
    // ventana (min/max/cerrar), cursores del SO y el hit-test → habilita la TitleBar propia.
    m_platform = FluentUI::CreateHostPlatform().release();
    if (m_platform) {
        c->platform = m_platform;
        // La ventana ya nace SIN borde bajo ENGINE_EDITOR (Platform/Window.cpp); aquí solo se
        // PULE el chrome (esquinas redondeadas + color de borde por DWM en Windows) y se cablea
        // el hit-test que el SO consulta para arrastrar/redimensionar, leyendo las zonas que
        // publica FluentUI::TitleBar en ctx->titleBarHit.
        m_platform->ApplyBorderlessChrome(window);
        m_platform->SetWindowHitTest(window, &FluentUI::CustomTitleBarHitTest, c);
    } else {
        LOG_WARN("EditorUI: CreateHostPlatform falló; se mantiene la barra de título nativa");
    }

    m_uictx       = c;
    m_initialized = true;
    LOG_INFO("Editor (FluentUI) inicializado");
    return true;
}

// El bus llega tras init(): aquí se engancha la escucha de lo que el VIEWPORT edita (arrastrar
// un gizmo). Sin esto, mover una entidad con el ratón no marcaba el proyecto como sucio.
void EditorUI::setEventBus(EventBus* bus) {
    m_eventBus = bus;
    if (!bus) return;
    m_sceneEditedSub = bus->subscribe<SceneEditedEvent>(
        [this](const SceneEditedEvent&) { m_sceneDirty = true; });
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
    // El core ya no consume SDL_Event; ProcessSDLEvent traduce a UIEvent internamente.
    if (c) FluentUI::ProcessSDLEvent(c->input, e);
}

void EditorUI::beginFrame(float dt, VkExtent2D fbExtent) {
    if (!m_initialized) return;

    // Espacio de maquetación = el del attachment sobre el que grabará render() (ver el
    // contrato en el .h). Fallback en PÍXELES: SDL_GetWindowSize devuelve coordenadas
    // lógicas, que en un display escalado NO son los píxeles de la swapchain — usarlas
    // maquetaría la UI en un rectángulo menor que la imagen y filtraría la escena.
    if (fbExtent.width == 0 || fbExtent.height == 0) {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
        fbExtent.width  = static_cast<uint32_t>(pw > 0 ? pw : 0);
        fbExtent.height = static_cast<uint32_t>(ph > 0 ? ph : 0);
    }
    m_buildExtent = fbExtent;
    const int fbw = static_cast<int>(fbExtent.width);
    const int fbh = static_cast<int>(fbExtent.height);
    const bool drawable = (fbw > 0 && fbh > 0);   // false = ventana minimizada

    // Pantalla de bienvenida: aún sin proyecto/juego. Solo dibuja el selector; nada de
    // escena/assets/menús (no hay sobre qué operar).
    if (m_welcome) {
        if (!drawable) return;
        FluentUI::SetCurrentContext(static_cast<FluentUI::UIContext*>(m_uictx));
        const int w = fbw, h = fbh;
        if (auto* c = FluentUI::GetContext()) c->renderer.SetViewport(w, h);
        FluentUI::NewFrame(dt);
        // Sin viewport de escena aquí: el fondo tapa la ventana entera (el pass es
        // loadOp=LOAD y si no, se vería lo que haya renderizado el motor detrás).
        if (auto* c = FluentUI::GetContext())
            c->renderer.DrawRectFilled(FluentUI::Vec2(0.0f, 0.0f),
                                       FluentUI::Vec2(static_cast<float>(w), static_cast<float>(h)),
                                       c->style.backgroundColor, 0.0f);
        drawTitleBar(/*withCommands=*/false);   // la ventana sin borde necesita chrome (arrastre/cerrar) también aquí
        buildWelcomeScreen(w, h);
        if (auto* c = FluentUI::GetContext()) FluentUI::RenderToasts(c);
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

    // Script pedido desde un diálogo (asignar uno existente o crear uno nuevo). Se aplica
    // aquí, en el hilo principal y ANTES del bloque de SceneOp: la entidad pertenece a la
    // escena ACTUAL (la que estaba al abrir el diálogo); si también hubiera una carga de
    // escena encolada, procesarla después evita asignar sobre la escena equivocada. El .lua
    // externo se importa al proyecto (Assets/Scripts) y el componente guarda la ruta RELATIVA.
    {
        ScriptOp op = ScriptOp::None; Entity target{}; std::string src;
        {
            std::lock_guard<std::mutex> lk(m_pendingMutex);
            op = m_pendingScriptOp; target = m_pendingScriptEntity; src = m_pendingScriptPath;
            m_pendingScriptOp = ScriptOp::None; m_pendingScriptPath.clear();
        }
        if (op == ScriptOp::Create && !src.empty()) {
            // El diálogo de guardar ya avisa si el archivo existe; aun así NO lo pisamos:
            // crear un script no debe poder borrar el trabajo de otro.
            if (std::filesystem::exists(src)) {
                LOG_WARN("El script '%s' ya existe: se adjunta sin tocar su contenido.", src.c_str());
            } else if (!writeScriptTemplate(src)) {
                LOG_ERROR("No se pudo crear el script '%s'.", src.c_str());
                src.clear();                       // nada que adjuntar
            } else {
                LOG_INFO("Script creado: %s", src.c_str());
            }
            m_assetsScanned = false;               // el .lua nuevo debe salir en el navegador
        }
        if (op != ScriptOp::None && !src.empty() && m_sceneMgr) {
            const std::string rel  = Project::instance().importAsset(src, "Assets/Scripts");
            const std::string path = rel.empty() ? src : rel;   // si el import falla, ruta tal cual
            m_assetsScanned = false;               // el import pudo añadir el .lua al navegador
            if (m_sceneMgr->current().alive(target)) assignScript(target, path);
        }
    }

    // Textura pedida desde "Apariencia" (o desde "Agregar componente"). Mismo camino y
    // mismas razones que el bloque de scripts: hilo principal, antes de la SceneOp. La
    // imagen externa se importa a Assets/Textures del proyecto → ruta RELATIVA guardable.
    {
        Entity target{}; std::string src;
        {
            std::lock_guard<std::mutex> lk(m_pendingMutex);
            target = m_pendingTextureEntity; src = m_pendingTexturePath;
            m_pendingTexturePath.clear();
        }
        if (!src.empty() && m_sceneMgr) {
            const std::string rel  = Project::instance().importAsset(src, "Assets/Textures");
            const std::string path = rel.empty() ? src : rel;
            m_assetsScanned = false;               // el import pudo añadir la imagen al navegador
            if (m_sceneMgr->current().alive(target)) assignSprite(target, path);
        }
    }

    // Imagen elegida en "Crear sprite…" (menú Entidad): se importa al proyecto y se pide el
    // alta al modo. Mismo camino que la textura de arriba, pero aquí no hay entidad todavía.
    {
        std::string src;
        {
            std::lock_guard<std::mutex> lk(m_pendingMutex);
            src = m_pendingNewSpritePath;
            m_pendingNewSpritePath.clear();
        }
        if (!src.empty()) {
            const std::string rel = Project::instance().importAsset(src, "Assets/Textures");
            m_assetsScanned = false;
            requestNewEntity(NewEntityKind::Sprite, rel.empty() ? src : rel);
        }
    }

    // El contexto del editor debe estar activo antes de applySceneOp: sus resultados
    // encolan toasts (ShowToast) en ESTE contexto. Si la 2ª ventana consumió el último
    // evento del frame, el contexto activo sería el suyo y el toast no aparecería aquí.
    FluentUI::SetCurrentContext(static_cast<FluentUI::UIContext*>(m_uictx));

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

    // Minimizada: las tareas de arriba (cargas diferidas, ops del menú Archivo) ya han
    // corrido; maquetar con un extent nulo no tiene sentido y el Engine no dibujará.
    if (!drawable) return;

    FluentUI::SetCurrentContext(static_cast<FluentUI::UIContext*>(m_uictx));
    const int w = fbw, h = fbh;
    if (auto* c = FluentUI::GetContext()) c->renderer.SetViewport(w, h);
    FluentUI::NewFrame(dt);
    buildPanels(dt, w, h);
    // Notificaciones transitorias (guardar/cargar/errores) apiladas abajo-derecha. Se
    // renderizan una vez por frame, tras construir los paneles (capa Overlay, por encima).
    if (auto* c = FluentUI::GetContext()) FluentUI::RenderToasts(c);
}

// Nombre de plantilla → etiqueta de la UI. Solo se traducen las dos que trae el motor; una
// plantilla nueva se muestra con el nombre de su carpeta (nada que tocar aquí para añadirla).
std::string EditorUI::templateLabel(const std::string& name) {
    if (name == "Empty") return "Proyecto vacío";
    if (name == "Demo")  return "Mundo de ejemplo";
    return name;
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

        // Con qué nace el proyecto: en blanco (solo los scripts base) o con el mundo de
        // ejemplo. La lista sale de las carpetas de Templates/, no de una lista en el código.
        if (m_templateNames.empty()) m_templateNames = Project::instance().templateNames();
        std::vector<std::string> labels;
        labels.reserve(m_templateNames.size());
        for (const std::string& t : m_templateNames) labels.push_back(templateLabel(t));
        FluentUI::ComboBox("Plantilla", &m_templateSel, labels, bw);

        if (FluentUI::Button("Nuevo proyecto…", Vec2(bw, 34.0f))) {
            const std::string tpl =
                (m_templateSel >= 0 && m_templateSel < static_cast<int>(m_templateNames.size()))
                    ? m_templateNames[m_templateSel] : std::string("Empty");
            FluentUI::ShowSaveFileDialog(
                m_window,
                std::vector<FluentUI::FileFilter>{ { "Proyecto PokeMotor", "pkproj" } },
                std::string("MiProyecto.pkproj"),
                [this, tpl](const std::vector<std::string>& paths, int) {
                    if (paths.empty()) return;
                    std::lock_guard<std::mutex> lk(m_pendingMutex);
                    m_pendingProject = true; m_pendingProjectIsNew = true; m_pendingProjectPath = paths[0];
                    m_newProjectTemplate = tpl;
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

bool EditorUI::consumeProjectChoice(std::string& outPath, bool& outIsNew, std::string& outTemplate) {
    std::lock_guard<std::mutex> lk(m_pendingMutex);
    if (!m_pendingProject) return false;
    m_pendingProject = false;
    outPath     = m_pendingProjectPath;
    outIsNew    = m_pendingProjectIsNew;
    outTemplate = m_newProjectTemplate;
    m_pendingProjectPath.clear();
    return true;
}

// Reparte a partes iguales lo que sobre: definida junto a las funciones de acoplado.
static void normalizeRatios(std::vector<float>& r);

void EditorUI::buildPanels(float dt, int w, int h) {
    using FluentUI::Vec2;
    if (dt > 0.0f) m_fps = m_fps * 0.9f + (1.0f / dt) * 0.1f;  // FPS suavizado
    auto* c = FluentUI::GetContext();
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);

    handleShortcuts();   // Ctrl+K (paleta), Ctrl+1/2/3 (paneles)

    // Visibilidad de los paneles: se lee una vez, cuando ya hay proyecto abierto.
    if (!m_hudLoaded && Project::instance().isOpen()) { loadHudState(); m_hudLoaded = true; }

    // Drop de un asset sobre el VIEWPORT (el drag empezó en una tarjeta del navegador). El
    // Splitter calcula las regiones al DIBUJAR, así que usamos el rect del viewport del frame
    // ANTERIOR (m_viewportRect) — 1 frame de lag, imperceptible al soltar. Se comprueba ANTES de
    // redibujar las tarjetas (el DragDropSource cancela el drag al soltar fuera de un target).
    m_hierDropPath.clear();   // el drop sobre la jerarquía dura un frame (ella lo consume)
    // Fin del arrastre con UN FRAME DE RETRASO: quien limpia el estado de dragDrop es el
    // destructor del DragDropSource de la tarjeta, y esa tarjeta solo lo construye mientras
    // sea m_dragSourcePath. Borrarlo en el mismo frame del soltar dejaría el drag "pegado"
    // (dd.active a true para siempre, sin ningún source vivo que lo cierre).
    if (m_dragEnded) { m_dragSourcePath.clear(); m_dragEnded = false; }
    if (c) {
        auto& dd = c->dragDrop;
        if (dd.active && c->input.IsMouseReleased(0) && !m_dragSourcePath.empty()) {
            const float mx = c->input.MouseX(), my = c->input.MouseY();
            // "Sobre la escena" = dentro del viewport Y fuera del HUD: con el layout overlay
            // el viewport es la ventana entera, así que soltar sobre un panel flotante caería
            // dentro de él si no se restaran (crearía una entidad detrás del panel).
            const bool onViewport = m_viewportRect.Contains(Vec2(mx, my)) && !pointOverHud(mx, my);
            if (dd.payloadType == "ASSET_TEXTURE") {
                if (onViewport && m_eventBus) {
                    m_eventBus->emit(AssetDroppedEvent{ m_dragSourcePath, pk::Vec2(mx, my) });
                    m_sceneDirty = true;   // el drop crea una entidad en la escena
                } else {
                    // Fuera del viewport: si cae sobre una fila de la jerarquía, la imagen
                    // pasa a ser la textura de ESA entidad (en vez de crear una nueva).
                    m_hierDropPath     = m_dragSourcePath;
                    m_hierDropPos      = Vec2(mx, my);
                    m_hierDropIsScript = false;
                }
            } else if (dd.payloadType == "ASSET_SCRIPT") {
                // Un script no CREA entidad: se adjunta a una existente. Sobre el viewport
                // la resuelve el modo por picking (el editor no conoce las posiciones del
                // mundo); sobre la jerarquía la fila dice cuál es.
                if (onViewport && m_eventBus) {
                    m_eventBus->emit(ScriptDroppedEvent{ m_dragSourcePath, pk::Vec2(mx, my) });
                    m_sceneDirty = true;
                } else {
                    m_hierDropPath     = m_dragSourcePath;
                    m_hierDropPos      = Vec2(mx, my);
                    m_hierDropIsScript = true;
                }
            }
        }
        if (c->input.IsMouseReleased(0)) m_dragEnded = true;   // se limpia al frame siguiente
    }

    // Fondo del chrome ANTES que nada: tapa la escena en todo lo que no sea el viewport.
    paintChromeBackground(fw, static_cast<float>(h));


    // --- TitleBar propia (chrome de ventana WinUI) con el MENÚ categorizado ---
    // Es el ÚNICO chrome fijo del editor: menú a la izquierda, estado de la escena a la
    // derecha, el resto zona de arrastre. Todo lo demás flota SOBRE el juego.
    drawTitleBar(/*withCommands=*/true);

    // El viewport es todo lo que queda bajo la barra de título: el juego se ve entero y a
    // escala completa, y los paneles se dibujan encima (modelo overlay, pass loadOp=LOAD).
    const float top = c ? c->cursorPos.y : kTopBand;
    m_viewportRect = FluentUI::Rect(Vec2(0.0f, top), Vec2(fw, std::max(0.0f, fh - top)));

    // Rects del HUD de ESTE frame: se acumulan al dibujar y se publican al final; el picking
    // del modo consulta los del frame anterior (mismo lag que m_viewportRect).
    m_uiRects.clear();

    const float dpi = c ? c->dpiScale : 1.0f;
    const float pad = 16.0f * dpi;
    const float headH = 32.0f * dpi;   // alto de la cabecera de un overlay (ver beginOverlay)

    // Con el panel inferior maximizado, escena e inspector quedan DETRÁS de él: dibujarlos
    // solo aporta texto fantasma visto a través del cristal. Se omiten sin tocar su estado
    // de plegado, así que vuelven tal como estaban al restaurar.
    const bool bottomCovers = m_showConsole && m_bottomMax;

    FluentUI::Rect bottomRect;   // lo que ocupa el panel inferior (el dock lo esquiva)

    // --- Geometría por defecto de cada panel ----------------------------------------
    // Cada panel sabe dónde va cuando está solo; un GRUPO usa la de su panel principal.
    struct PanelPlace { Vec2 pos, size; bool anchorRight = false, anchorBottom = false; };
    auto placeOf = [&](PanelId p) -> PanelPlace {
        switch (p) {
        case PanelId::Hierarchy: {
            // El alto lo fija el CONTENIDO, no la ventana: un panel de 440 px con tres
            // entidades dentro tapa juego para nada, que es justo lo que este layout evita.
            const size_t entCount = m_sceneMgr ? m_sceneMgr->current().allEntities().size() : 0;
            const float content = m_hierContentH > 0.0f ? m_hierContentH
                                                        : static_cast<float>(entCount) * 34.0f * dpi;
            const float hierH = std::clamp(headH + kPanePad * 3.0f * dpi + content,
                                           headH + 56.0f * dpi,
                                           std::max(headH + 56.0f * dpi,
                                                    fh - top - pad * 2.0f - 96.0f * dpi));
            return { Vec2(pad, top + pad), overlaySize("ovl_hier", Vec2(248.0f * dpi, hierH)) };
        }
        case PanelId::Inspector: {
            Scene* sc = m_sceneMgr ? &m_sceneMgr->current() : nullptr;
            const bool hasSel = m_selection && sc && m_selection->has() && sc->alive(m_selection->entity);
            // Sin selección solo hay un mensaje de dos líneas: el panel se encoge a eso.
            const float insH = hasSel
                ? std::max(180.0f * dpi, std::min(520.0f * dpi, fh - top - pad * 2.0f - 72.0f * dpi))
                : headH + 76.0f * dpi;
            const Vec2 size = overlaySize("ovl_insp", Vec2(320.0f * dpi, insH));
            return { Vec2(fw - size.x - pad, top + pad), size, /*anchorRight=*/true };
        }
        default: {
            const bool assets = (m_bottomTab != 0);
            float w     = assets ? std::clamp(fw * 0.62f, 760.0f * dpi,
                                              std::max(760.0f * dpi, fw - pad * 2.0f))
                                 : 560.0f * dpi;
            float bodyH = assets ? std::clamp(fh * 0.42f, 340.0f * dpi,
                                              std::max(340.0f * dpi, fh - top - pad * 2.0f - 140.0f * dpi))
                                 : 268.0f * dpi;
            const float dockBand = 64.0f * dpi;
            float bottomY = fh - pad;
            if (m_bottomMax && m_showConsole) {
                w       = std::max(360.0f * dpi, fw - pad * 2.0f);
                bottomY = fh - pad - dockBand;
                bodyH   = std::max(340.0f * dpi, bottomY - (top + pad) - headH);
            }
            const Vec2 base(w, headH + bodyH);
            const Vec2 size = (m_bottomMax && m_showConsole) ? base : overlaySize("ovl_console", base);
            // Plegado solo se ve la cabecera: ancho compacto y ALTO de cabecera. Devolver el
            // alto desplegado descolocaba el panel, porque al estar anclado abajo la posición
            // se deriva del tamaño.
            if (!m_showConsole)
                return { Vec2(pad, bottomY - headH), Vec2(560.0f * dpi, headH), false, true };
            return { Vec2(pad, bottomY - size.y), size, false, /*anchorBottom=*/true };
        }
        }
    };

    // El inspector persigue a la selección solo si está SOLO en su grupo y sin mover a mano:
    // dentro de un grupo manda la geometría del grupo, no la entidad.
    Vec2 anchor(0.0f, 0.0f);
    bool anchored = false;

    // --- Dibujado: un cluster = un rectángulo que sus miembros se reparten -----------
    m_headerRects.clear();
    m_panelRects.clear();
    int  splitterCluster = -1, splitterIndex = -1;   // divisor bajo el ratón (para arrastrarlo)
    for (size_t ci = 0; ci < m_clusters.size(); ++ci) {
        DockCluster& cl = m_clusters[ci];
        if (cl.members.empty()) continue;
        if (cl.ratios.size() != cl.members.size()) {
            cl.ratios.assign(cl.members.size(), 1.0f / static_cast<float>(cl.members.size()));
        }
        const PanelId  clMain  = clusterAnchor(cl);
        const char*    clId    = panelOverlayId(clMain);
        const bool     lone    = (cl.members.size() == 1);
        PanelPlace     clPlace = placeOf(clMain);
        // El rect del cluster crece con sus miembros: cada uno necesita al menos su tamaño
        // por defecto en el eje del reparto.
        if (!lone) {
            float need = 0.0f, cross = 0.0f;
            for (const PanelGroup& g : cl.members) {
                const PanelPlace p = placeOf(g.tabs[0]);
                need  += cl.vertical ? p.size.y : p.size.x;
                cross  = std::max(cross, cl.vertical ? p.size.x : p.size.y);
                if (g.tabs.size() > 1) cross += 0.0f;
            }
            if (cl.vertical) { clPlace.size.y = std::max(clPlace.size.y, need * 0.62f);
                               clPlace.size.x = std::max(clPlace.size.x, cross); }
            else             { clPlace.size.x = std::max(clPlace.size.x, need * 0.62f);
                               clPlace.size.y = std::max(clPlace.size.y, cross); }
            if (clPlace.anchorRight)  clPlace.pos.x = fw - clPlace.size.x - pad;
            if (clPlace.anchorBottom) clPlace.pos.y = fh - pad - clPlace.size.y;
        }
        // El offset/tamaño manual del cluster viven en el estado del PRIMER miembro, que es
        // también quien lo mueve al arrastrar su cabecera.
        const Vec2 clSize = overlaySize(clId, clPlace.size);
        Vec2 clPos = clPlace.pos;
        if (clPlace.anchorRight)  clPos.x = fw - clSize.x - pad;
        if (clPlace.anchorBottom) clPos.y = fh - pad - clSize.y;
        // OJO: el offset manual NO se suma aquí. beginOverlay ya lo aplica al panel que lo
        // posee (el primer miembro); sumarlo también en el layout lo aplicaba dos veces y el
        // arrastre se realimentaba consigo mismo. Para colocar a los demás miembros dentro
        // del cluster sí hace falta conocerlo, así que se lee aparte.
        Vec2 clOffset(0.0f, 0.0f);
        {
            auto itS = m_overlayState.find(clId);
            if (itS != m_overlayState.end()) clOffset = itS->second.offset;
        }

        const float divider = 6.0f * dpi;
        float used = 0.0f;
        for (size_t mi = 0; mi < cl.members.size(); ++mi) {
            PanelGroup& g = cl.members[mi];
            if (g.tabs.empty()) continue;
            g.active = std::clamp(g.active, 0, static_cast<int>(g.tabs.size()) - 1);
            const PanelId main   = g.tabs[0];
            const PanelId shown  = g.tabs[static_cast<size_t>(g.active)];
            const bool    single = (g.tabs.size() == 1);
            const char*   ovlId  = panelOverlayId(main);
            const bool isConsoleGroup = (main == PanelId::Console);
            const bool isFirst = (main == clMain);   // el ancla es quien mueve el cluster

            // Rect del miembro dentro del cluster (reparto por ratios, menos los divisores).
            Vec2 pos = clPos, size = clSize;
            if (!isFirst) pos = pos + clOffset;   // el primero lo recibe de beginOverlay
            if (!lone) {
                const float total = (cl.vertical ? clSize.y : clSize.x)
                                  - divider * static_cast<float>(cl.members.size() - 1);
                const float span  = std::max(80.0f * dpi, total * cl.ratios[mi]);
                const Vec2 origin = clPos + (isFirst ? Vec2(0.0f, 0.0f) : clOffset);
                if (cl.vertical) { pos.y = origin.y + used; size.y = span; }
                else             { pos.x = origin.x + used; size.x = span; }
                used += span + divider;
            } else if (!clPlace.anchorRight && !clPlace.anchorBottom) {
                // (un solo miembro: pos/size ya son los del cluster)
            }

            // El inspector persigue a la selección solo estando SOLO (ni acoplado ni agrupado).
            PanelPlace place{ pos, size, clPlace.anchorRight, clPlace.anchorBottom };
            if (main == PanelId::Inspector && single && lone && m_selection &&
                m_selection->screenValid && !overlayMoved("ovl_insp")) {
                int lw = 0, lh = 0;
                SDL_GetWindowSize(m_window, &lw, &lh);
                const float sx = (lw > 0) ? fw / static_cast<float>(lw) : 1.0f;
                const float sy = (lh > 0) ? fh / static_cast<float>(lh) : 1.0f;
                anchor = Vec2(m_selection->screenX * sx, m_selection->screenY * sy);
                // Hueco = lo que ocupa la entidad con su gizmo (lo publica el modo) más un
                // respiro. Un hueco fijo y pequeño dejaba el panel encima de las flechas.
                const float gap = std::max(64.0f * dpi, m_selection->screenRadius * sx + 28.0f * dpi);
                float x = anchor.x + gap;
                if (x + place.size.x + pad > fw) x = anchor.x - gap - place.size.x;
                place.pos = Vec2(std::clamp(x, pad, std::max(pad, fw - place.size.x - pad)),
                                 std::clamp(anchor.y - place.size.y * 0.35f, top + pad,
                                            std::max(top + pad, fh - place.size.y - pad)));
                place.anchorRight = false;
                anchored = true;
            }
            // Agrupado en pestañas: el tamaño tiene que dar cabida a cualquiera de ellas.
            if (!single && lone) {
                for (size_t t = 1; t < g.tabs.size(); ++t) {
                    const PanelPlace other = placeOf(g.tabs[t]);
                    place.size.x = std::max(place.size.x, other.size.x);
                    place.size.y = std::max(place.size.y, other.size.y);
                }
                place.size.y += 28.0f * dpi;
                if (place.anchorRight)  place.pos.x = fw - place.size.x - pad;
                if (place.anchorBottom) place.pos.y = fh - pad - place.size.y;
            }

            if (bottomCovers && !isConsoleGroup) continue;

            bool* openFlag = (main == PanelId::Hierarchy) ? &m_showHierarchy
                           : (main == PanelId::Inspector) ? &m_showInspector : &m_showConsole;
            char note[64] = {};
            LogCounts lc{};
            if (isConsoleGroup) {
                lc = logCounts();
                if (lc.error)     std::snprintf(note, sizeof(note), "%u errores", lc.error);
                else if (lc.warn) std::snprintf(note, sizeof(note), "%u avisos",  lc.warn);
            }
            const float areaFrac = (place.size.x * place.size.y) / std::max(1.0f, fw * fh);
            const float bgAlpha  = std::clamp(0.93f - areaFrac * 0.40f, 0.76f, 0.93f);
            // Dónde se DIBUJA de verdad: beginOverlay le suma su offset al panel que lo posee
            // (el ancla), así que aquí hay que sumarlo para todo lo que se sitúe respecto al
            // panel — el divisor, los rects publicados y la franja que esquiva el dock. Sin
            // esto, el bloque se movía pero su divisor y sus zonas de drop se quedaban atrás.
            const Vec2 shownPos = place.pos + (isFirst ? clOffset : Vec2(0.0f, 0.0f));

            if (isConsoleGroup)
                bottomRect = FluentUI::Rect(shownPos,
                                            Vec2(place.size.x, *openFlag ? place.size.y : headH));

            // Cualquier miembro mueve el bloque: su arrastre se acumula en el ancla.
            if (beginOverlay({ .id = ovlId, .pos = place.pos, .size = place.size,
                               .title = panelTitle(shown), .icon = panelIcon(shown),
                               .open = lone ? openFlag : nullptr,
                               .note = note[0] ? note : nullptr, .noteAlert = lc.error > 0,
                               .maximized = (isConsoleGroup && lone) ? &m_bottomMax : nullptr,
                               .bgAlpha = bgAlpha,
                               .anchorRight = place.anchorRight, .anchorBottom = place.anchorBottom,
                               .resizable = lone && !(isConsoleGroup && m_bottomMax),
                               .draggable = true,
                               .moveId    = isFirst ? nullptr : panelOverlayId(clMain),
                               .inCluster = !lone })) {
                if (!single) buildGroupTabs(m_clusters[ci].members[mi], place.size.x,
                                            static_cast<int>(ci), static_cast<int>(mi));
                if (ci < m_clusters.size() && mi < m_clusters[ci].members.size() &&
                    !m_clusters[ci].members[mi].tabs.empty()) {
                    const PanelGroup& cur = m_clusters[ci].members[mi];
                    buildPanelContent(cur.tabs[static_cast<size_t>(
                        std::clamp(cur.active, 0, static_cast<int>(cur.tabs.size()) - 1))]);
                }
                endOverlay();
            }
            // El rect que se publica es el REALMENTE dibujado: plegado es solo la cabecera.
            // Registrar el tamaño desplegado daba zonas de drop en un área que no existe.
            const bool drawnOpen = !openFlag || *openFlag;
            m_headerRects[ovlId] = FluentUI::Rect(shownPos, Vec2(place.size.x, headH));
            m_panelRects[ovlId]  = FluentUI::Rect(shownPos,
                                     Vec2(place.size.x, drawnOpen ? place.size.y : headH));

            // Divisor con el miembro siguiente: arrastrarlo reparte el espacio.
            if (!lone && mi + 1 < cl.members.size() && c) {
                const Vec2 dp = cl.vertical ? Vec2(shownPos.x, shownPos.y + place.size.y)
                                            : Vec2(shownPos.x + place.size.x, shownPos.y);
                const Vec2 ds = cl.vertical ? Vec2(place.size.x, divider)
                                            : Vec2(divider, place.size.y);
                const float mx2 = c->input.MouseX(), my2 = c->input.MouseY();
                const bool hov = mx2 >= dp.x && mx2 <= dp.x + ds.x &&
                                 my2 >= dp.y && my2 <= dp.y + ds.y;
                FluentUI::Color dc = c->style.panel.borderColor;
                if (hov) { dc = c->style.accentColor; splitterCluster = static_cast<int>(ci);
                           splitterIndex = static_cast<int>(mi);
                           c->desiredCursor = cl.vertical ? FluentUI::UIContext::CursorType::ResizeV
                                                          : FluentUI::UIContext::CursorType::ResizeH; }
                c->renderer.DrawRectFilled(dp, ds, dc, 0.0f);
                pushUiRect(FluentUI::Rect(dp, ds));
            }
        }
    }

    // Los rects dibujados pasan a ser los consultables (el drop del frame siguiente).
    m_headerRectsPrev.swap(m_headerRects);
    m_panelRectsPrev.swap(m_panelRects);

    // Arrastre del divisor: mueve espacio de un miembro al siguiente.
    if (c) {
        if (splitterCluster >= 0 && c->input.IsMousePressed(0) && m_splitDrag.cluster < 0) {
            m_splitDrag.cluster = splitterCluster;
            m_splitDrag.index   = splitterIndex;
        }
        if (m_splitDrag.cluster >= 0) {
            if (c->input.IsMouseDown(0) &&
                m_splitDrag.cluster < static_cast<int>(m_clusters.size())) {
                DockCluster& cl = m_clusters[static_cast<size_t>(m_splitDrag.cluster)];
                const int i = m_splitDrag.index;
                if (i >= 0 && i + 1 < static_cast<int>(cl.ratios.size())) {
                    const float total = cl.vertical ? std::max(1.0f, fh) : std::max(1.0f, fw);
                    const float dmove = (cl.vertical ? c->input.MouseY() : c->input.MouseX());
                    // Reparto proporcional al desplazamiento del ratón sobre el rect del cluster.
                    const float step = (dmove - m_splitDrag.lastPos) / total;
                    if (m_splitDrag.lastPos != 0.0f) {
                        cl.ratios[static_cast<size_t>(i)]     += step;
                        cl.ratios[static_cast<size_t>(i + 1)] -= step;
                        cl.ratios[static_cast<size_t>(i)]     = std::max(0.12f, cl.ratios[static_cast<size_t>(i)]);
                        cl.ratios[static_cast<size_t>(i + 1)] = std::max(0.12f, cl.ratios[static_cast<size_t>(i + 1)]);
                        normalizeRatios(cl.ratios);
                    }
                    m_splitDrag.lastPos = dmove;
                }
            } else {
                m_splitDrag = SplitDrag{};
            }
        }
    }

    // Guía de la entidad al inspector anclado.
    if (anchored && c && m_showInspector && !bottomCovers) {
        auto it = m_headerRectsPrev.find("ovl_insp");
        if (it != m_headerRectsPrev.end()) {
            const FluentUI::Rect& hr = it->second;
            const float ly = std::clamp(anchor.y, hr.pos.y + 8.0f * dpi, hr.pos.y + 40.0f * dpi);
            const float x0 = (hr.pos.x > anchor.x) ? anchor.x : hr.pos.x + hr.size.x;
            const float x1 = (hr.pos.x > anchor.x) ? hr.pos.x : anchor.x;
            FluentUI::Color guide = c->style.accentColor; guide.a = 0.55f;
            if (x1 > x0)
                c->renderer.DrawRectFilled(Vec2(x0, ly), Vec2(x1 - x0, 1.0f * dpi), guide, 0.0f);
            c->renderer.DrawRectFilled(Vec2(anchor.x - 2.0f * dpi, anchor.y - 2.0f * dpi),
                                       Vec2(4.0f * dpi, 4.0f * dpi), c->style.accentColor, 0.0f);
        }
    }

    // --- Acoplar: la pista se pinta mientras se arrastra; el drop se resuelve aquí -----
    if (m_dockHintSide != DockSide::None && c) {
        FluentUI::Color hi = c->style.accentColor; hi.a = 0.28f;
        c->renderer.DrawRectFilled(m_dockHintRect.pos, m_dockHintRect.size, hi,
                                   c->style.panel.cornerRadius * dpi);
        c->renderer.DrawRect(m_dockHintRect.pos, m_dockHintRect.size, c->style.accentColor,
                             c->style.panel.cornerRadius * dpi);
    }
    m_dockHintSide = DockSide::None;
    m_dockHintId.clear();
    if (m_undockCluster >= 0) {
        if (m_undockTab >= 0) undockTab(m_undockCluster, m_undockMember, m_undockTab);
        else                  undockGroup(m_undockCluster, m_undockMember);
        m_undockCluster = m_undockMember = m_undockTab = -1;
    }
    if (!m_dropSourceId.empty()) {
        // La zona se recalcula AQUÍ con el punto donde se soltó: la pista se limpia cada
        // frame y el soltar llega al siguiente, así que fiarse de ella dejaba el drop sin
        // destino siempre.
        std::string dropTarget;
        FluentUI::Rect ignored;
        const DockSide dropSide = dockZoneAt(m_dropSourceId, m_dropPos, dropTarget, ignored);
        if (!dropTarget.empty() && dropTarget != m_dropSourceId) {
            if (dropSide == DockSide::Tab) dockAsTab(m_dropSourceId, dropTarget);
            else                           dockBeside(m_dropSourceId, dropTarget, dropSide);
        }
        m_dropSourceId.clear();
    }

    // --- Transporte + herramientas: dock flotante centrado abajo ---
    // Con el navegador de assets desplegado (o el panel maximizado) el dock se va arriba del
    // todo en vez de quedarse pegado al borde del panel.
    buildTransportDock(fw, fh, top, bottomRect, m_showConsole && (m_bottomTab != 0 || m_bottomMax));

    // Paleta de comandos: encima de todo, es lo que tiene el foco mientras está abierta.
    buildCommandPalette(fw, top);

    // Los rects dibujados este frame pasan a ser los publicados (el rect de un panel solo se
    // conoce tras maquetarlo, igual que el del viewport).
    m_uiRectsPrev.swap(m_uiRects);

    // Confirmación de descarte. Se construye SIEMPRE (dibuja solo si está abierto y
    // necesita los frames posteriores al cierre para su animación de salida).
    buildDirtyDialog();
}

// Fondo opaco de TODA la UI menos el viewport. El pass del editor usa loadOp=LOAD para
// preservar la escena (modelo overlay), así que cualquier píxel que ningún widget pinte
// deja ver el juego: los huecos entre paneles, los márgenes de un ScrollView, la franja
// de los caption buttons… Los ejemplos (examples/App.cpp, EngineEditor.cpp) no necesitan
// esto porque FluentApp arranca el frame con loadOp=CLEAR y el fondo ya viene pintado.
// Se recorta con el rect del viewport del frame ANTERIOR (el Splitter lo calcula al
// dibujar) — mismo 1 frame de lag que ya se usa para el drop de assets.
void EditorUI::paintChromeBackground(float fw, float fh) {
    auto* c = FluentUI::GetContext();
    if (!c) return;
    using FluentUI::Vec2;
    // POKEMOTOR_DEBUG_CHROME=1 pinta este fondo en magenta: sirve para distinguir de un
    // vistazo si el fondo del chrome llega a la pantalla o si algo lo tapa después.
    static const bool dbgChrome = [] {
        const char* e = std::getenv("POKEMOTOR_DEBUG_CHROME");
        return e && e[0] == '1';
    }();
    const FluentUI::Color bg = dbgChrome ? FluentUI::Color(1.0f, 0.0f, 1.0f, 1.0f)
                                         : c->style.backgroundColor;
    const FluentUI::Rect& v  = m_viewportRect;

    if (dbgChrome) {   // una vez: qué rects se van a pintar realmente
        static int n = 0;
        if (n < 2) {
            ++n;
            LOG_INFO("DBG chrome: win=%.0fx%.0f vpRect=(%.0f,%.0f %.0fx%.0f) → "
                     "arriba(%.0fx%.0f) abajo(%.0fx%.0f) izq(%.0fx%.0f) der(%.0fx%.0f)",
                     fw, fh, v.pos.x, v.pos.y, v.size.x, v.size.y,
                     fw, v.pos.y, fw, fh - v.Bottom(), v.pos.x, v.size.y,
                     fw - v.Right(), v.size.y);
        }
    }

    if (v.size.x <= 0.0f || v.size.y <= 0.0f) {   // aún sin layout: tapa todo
        c->renderer.DrawRectFilled(Vec2(0.0f, 0.0f), Vec2(fw, fh), bg, 0.0f);
        return;
    }
    // El agujero se RECORTA a la ventana antes de restar. Sin esto, un m_viewportRect
    // que se salga por la derecha/abajo (viene del frame anterior y de availableSpace,
    // que no está garantizado que quepa) da rects de tamaño NEGATIVO en "derecha" y
    // "abajo": no pintan nada y dejan justo las franjas que muestran el juego.
    const float vx = std::clamp(v.pos.x,  0.0f, fw);
    const float vy = std::clamp(v.pos.y,  0.0f, fh);
    const float vr = std::clamp(v.Right(),  vx, fw);
    const float vb = std::clamp(v.Bottom(), vy, fh);
    c->renderer.DrawRectFilled(Vec2(0.0f, 0.0f),  Vec2(fw, vy),            bg, 0.0f);  // arriba
    c->renderer.DrawRectFilled(Vec2(0.0f, vb),    Vec2(fw, fh - vb),       bg, 0.0f);  // abajo
    c->renderer.DrawRectFilled(Vec2(0.0f, vy),    Vec2(vx, vb - vy),       bg, 0.0f);  // izquierda
    c->renderer.DrawRectFilled(Vec2(vr,   vy),    Vec2(fw - vr, vb - vy),  bg, 0.0f);  // derecha
}

// Tamaño del pane actual del Splitter (layoutStack.back().availableSpace = tamaño exacto; el
// Splitter hace BeginVertical con padding 0). Fallback al viewport completo. El contenido se
// dibuja FLUYENDO dentro del pane (reserveLayoutSpace=true), NO como panel flotante con pos
// explícita — un flotante dentro del BeginVertical del pane corrompía el layout de los panes
// siguientes (solo se dibujaba el primero).
static FluentUI::Vec2 paneSize(FluentUI::UIContext* c) {
    return c->layoutStack.empty() ? c->renderer.GetViewportSize()
                                  : c->layoutStack.back().availableSpace;
}

// --- Jerarquía (pane izquierdo): lista viva de entidades de la escena ---
void EditorUI::buildHierarchyPanel() {
    using FluentUI::Vec2;
    auto* c = FluentUI::GetContext();
    if (!c) return;
    const Vec2 ps = paneSize(c);
    // Padding del pane: sin él el título queda pegado al borde izquierdo de la ventana (y
    // el primer carácter se recorta). El Splitter da el pane en crudo, el padding es del
    // consumidor.
    c->cursorPos = c->cursorPos + Vec2(kPanePad, kPanePad);
    const Vec2 origin = c->cursorPos;
    const float innerW = std::max(0.0f, ps.x - kPanePad * 2.0f);
    // Contenido DIRECTO, sin BeginPanel (que es un widget-ventana con header/padding/auto-size
    // propios): el pane del Splitter es el contenedor. Título + árbol llenan el pane, como el demo
    // examples/EngineEditor.cpp.
    const float treeH = std::max(0.0f, ps.y - (c->cursorPos.y - origin.y) - kPanePad * 2.0f);
    const float treeTop = c->cursorPos.y;   // para medir cuánto ocupa de verdad el árbol
    if (FluentUI::BeginTreeView("hierarchy", Vec2(innerW, treeH))) {
        if (m_sceneMgr && m_selection) {
            Scene& s = m_sceneMgr->current();
            // Entidad sobre la que se soltó un asset este frame (ver m_hierDropPath). Se
            // resuelve durante el recorrido y se aplica DESPUÉS, ya fuera del bucle.
            Entity dropTarget{};
            for (Entity e : s.allEntities()) {
                const std::string label = s.has<NameComponent>(e)
                    ? s.get<NameComponent>(e).value
                    : ("Entidad " + std::to_string(e.id));
                const std::string nodeId = "e" + std::to_string(e.id) + "_" + std::to_string(e.generation);
                bool sel = (m_selection->entity == e);
                const bool was = sel;
                // El TreeNode dibuja en el cursor y solo publica su TAMAÑO (lastItemSize),
                // así que la posición de la fila se toma antes de dibujarla.
                const Vec2 rowPos = c->cursorPos;
                FluentUI::TreeNode(nodeId, label, FluentUI::Icons::Box, nullptr, &sel);
                if (sel && !was) m_selection->entity = e;   // recién clicado → selección única
                if (!m_hierDropPath.empty()) {
                    const Vec2 rowSize = c->lastItemSize;
                    if (m_hierDropPos.x >= rowPos.x && m_hierDropPos.x <= rowPos.x + rowSize.x &&
                        m_hierDropPos.y >= rowPos.y && m_hierDropPos.y <= rowPos.y + rowSize.y)
                        dropTarget = e;
                }
            }
            if (dropTarget.valid()) {
                if (m_hierDropIsScript) assignScript(dropTarget, m_hierDropPath);
                else                    assignSprite(dropTarget, m_hierDropPath);
                m_selection->entity = dropTarget;   // el inspector muestra ya la sección tocada
                m_hierDropPath.clear();
            }
        }
    }
    // Alto real del contenido (filas + espaciados): lo lee el layout para ajustar el panel.
    m_hierContentH = std::max(0.0f, c->cursorPos.y - treeTop);
    FluentUI::EndTreeView();
}

// NumberBox sobre un float del motor (el widget trabaja en double). Devuelve true solo
// cuando el valor cambió de verdad, para no marcar la escena sucia por un repintado.
static bool numberField(const char* label, float* v, double min, double max,
                        double step, const char* fmt) {
    double d = static_cast<double>(*v);
    if (!FluentUI::NumberBox(label, &d, min, max, step, fmt)) return false;
    const float nv = static_cast<float>(d);
    if (nv == *v) return false;
    *v = nv;
    return true;
}

// --- Inspector (pane derecho): componentes de la entidad seleccionada ---
void EditorUI::buildInspectorPanel() {
    auto* c = FluentUI::GetContext();
    if (!c) return;
    const FluentUI::Vec2 ps = paneSize(c);
    c->cursorPos = c->cursorPos + FluentUI::Vec2(kPanePad, kPanePad);   // padding del pane
    const FluentUI::Vec2 origin = c->cursorPos;
    const float innerW = std::max(0.0f, ps.x - kPanePad * 2.0f);
    // Contenido DIRECTO, sin BeginPanel (ver buildHierarchyPanel). Título + scroll llenan el pane;
    // el scroll clipa/desplaza el contenido del inspector cuando es más alto que el pane.
    const float bodyH = std::max(0.0f, ps.y - (c->cursorPos.y - origin.y) - kPanePad * 2.0f);
    if (FluentUI::BeginScrollView("inspector_scroll", FluentUI::Vec2(innerW, bodyH))) {
        static bool oTransform = true, oAppear = true, oScript = true, oCamera = true;
        Scene* s = m_sceneMgr ? &m_sceneMgr->current() : nullptr;

        if (m_selection && s && m_selection->has() && s->alive(m_selection->entity)) {
            const Entity e = m_selection->entity;
            // Nombre editable en línea (si hay NameComponent; se añade con "Agregar
            // componente"). Renombrar se refleja al instante en la jerarquía.
            if (s->has<NameComponent>(e)) {
                if (FluentUI::TextInput("Nombre", &s->get<NameComponent>(e).value))
                    m_sceneDirty = true;
            } else {
                FluentUI::Label("Entidad " + std::to_string(e.id), std::nullopt,
                                FluentUI::TypographyStyle::Subtitle);
            }
            FluentUI::Separator();

            // Los componentes van en Expander (card con header + icono), no en
            // CollapsingHeader: es el contenedor de sección de WinUI y da el mismo
            // agrupado visual que el resto del editor. Contrato B2: EndExpander SOLO
            // si BeginExpander devolvió true.
            if (s->has<Transform>(e) &&
                FluentUI::BeginExpander("insp_transform", "Transform", FluentUI::Icons::Move, &oTransform)) {
                Transform& tr = s->get<Transform>(e);
                // Campos numéricos reales (NumberBox: parsea en Enter/blur, clampa al rango,
                // spinners y rueda) en vez del DragFloat3, que pintaba un tercer componente Z
                // inexistente en 2D — y en Rotación dos componentes que se ignoraban.
                if (numberField("Posición X", &tr.position.x, -1e6, 1e6, 1.0, "%.2f")) m_sceneDirty = true;
                if (numberField("Posición Y", &tr.position.y, -1e6, 1e6, 1.0, "%.2f")) m_sceneDirty = true;
                double deg = static_cast<double>(tr.rotation) * 57.29578;   // rad → grados (Z en 2D)
                if (FluentUI::NumberBox("Rotación (°)", &deg, -360.0, 360.0, 1.0, "%.1f")) {
                    tr.rotation = static_cast<float>(deg) * 0.01745329f;
                    m_sceneDirty = true;
                }
                if (numberField("Escala X", &tr.scale.x, 0.01, 100.0, 0.1, "%.2f")) m_sceneDirty = true;
                if (numberField("Escala Y", &tr.scale.y, 0.01, 100.0, 0.1, "%.2f")) m_sceneDirty = true;
                FluentUI::EndExpander();
            }
            if (s->has<SpriteComponent>(e) &&
                FluentUI::BeginExpander("insp_appear", "Apariencia", FluentUI::Icons::FileImage, &oAppear)) {
                SpriteComponent& sp = s->get<SpriteComponent>(e);
                // Imagen del sprite: la ruta que se serializa + el selector para cambiarla
                // (o darle una a un sprite que no la tenga; también se puede arrastrar una
                // imagen del navegador sobre la fila de la jerarquía).
                FluentUI::Label(sp.texturePath.empty() ? "(sin textura)" : sp.texturePath,
                                std::nullopt, FluentUI::TypographyStyle::Caption);
                if (FluentUI::Button("Cambiar textura…")) openTextureDialog(e);
                FluentUI::Color col(sp.tint.x, sp.tint.y, sp.tint.z, sp.tint.w);
                if (FluentUI::ColorPicker("Tinte", &col)) {
                    sp.tint = pk::Vec4(col.r, col.g, col.b, col.a);
                    m_sceneDirty = true;
                }
                if (FluentUI::SliderFloat("Opacidad", &sp.tint.w, 0.0f, 1.0f)) m_sceneDirty = true;
                double layer = static_cast<double>(sp.layer);
                if (FluentUI::NumberBox("Capa", &layer, 0.0, 16.0, 1.0, "%.0f")) {
                    sp.layer = static_cast<int>(layer);
                    m_sceneDirty = true;
                }
                // Calidad/filtro del sprite (por asset): Pixel = nearest sin mips; Suave = lineal +
                // mipmaps. Al cambiarlo re-resolvemos la textura con el nuevo FilterMode (el
                // AssetManager cachea por "ruta|filtro") y actualizamos el handle para verlo en vivo.
                int fil = (sp.filter == FilterMode::Smooth) ? 1 : 0;
                if (FluentUI::SegmentedControl("spriteFilter",
                        std::vector<std::string>{ "Pixel", "Suave" }, &fil)) {
                    sp.filter = (fil == 1) ? FilterMode::Smooth : FilterMode::Pixel;
                    if (m_assets && !sp.texturePath.empty())
                        sp.tex = m_assets->loadTexture(sp.texturePath, true, true, sp.filter);
                    m_sceneDirty = true;
                }
                FluentUI::EndExpander();
            }
            // Cámara: aparece en la entidad-cámara (CameraComponent). El CENTRO se edita arriba
            // en Transform→Posición; aquí solo el zoom (px/tile).
            if (s->has<CameraComponent>(e) &&
                FluentUI::BeginExpander("insp_camera", "Cámara", FluentUI::Icons::Camera, &oCamera)) {
                CameraComponent& cam = s->get<CameraComponent>(e);
                double zoom = static_cast<double>(cam.zoom);
                if (FluentUI::NumberBox("Zoom (px/tile)", &zoom, 4.0, 256.0, 1.0, "%.1f")) {
                    cam.zoom = static_cast<float>(zoom);
                    m_sceneDirty = true;
                }
                FluentUI::EndExpander();
            }
            // Script: aparece si la entidad tiene un .lua adjunto (el alta va por "Agregar
            // componente", abajo). Muestra la ruta, Cambiar/Quitar y las variables de su
            // tabla 'exports' (número/bool/texto), editables en vivo. La asignación pasa
            // por el diálogo → pendiente → beginFrame; el ScriptSystem instancia solo al
            // frame siguiente.
            if (m_scriptSys && s->has<ScriptComponent>(e) &&
                FluentUI::BeginExpander("insp_script", "Script", FluentUI::Icons::FileCode, &oScript)) {
                bool showExports = true;
                const std::string scriptPath = s->get<ScriptComponent>(e).path;
                FluentUI::Label(scriptPath, std::nullopt, FluentUI::TypographyStyle::Caption);
                // Fallo de carga/ejecución del script: hasta ahora solo se veía en la consola,
                // mezclado con todo lo demás. El aviso desaparece al corregir el .lua y
                // guardarlo (el hot-reload limpia el error de la instancia).
                if (const std::string err = m_scriptSys->errorOf(e); !err.empty())
                    FluentUI::InfoBar("insp_script_err", FluentUI::InfoSeverity::Error,
                                      "Error en el script", err, false);
                if (FluentUI::Button("Abrir")) {
                    // Con la app asociada del sistema (VS Code, Notepad++…). Ruta ABSOLUTA
                    // resuelta por el proyecto: en Debug el .lua vivo es el del árbol de
                    // fuentes, que es justo el que vigila el hot-reload.
                    const std::string abs = Project::instance().resolveRead(scriptPath);
                    if (!SDL_OpenURL(abs.c_str()))
                        LOG_WARN("No se pudo abrir '%s' (%s).", abs.c_str(), SDL_GetError());
                }
                FluentUI::SameLine(8.0f);
                if (FluentUI::Button("Cambiar…")) openScriptDialog(e);
                FluentUI::SameLine(8.0f);
                if (FluentUI::Button("Quitar")) {
                    // Componente fuera + instancia fuera (ver ScriptSystem::detach); los
                    // exports ya no se dibujan este frame (el componente no existe).
                    s->remove<ScriptComponent>(e);
                    m_scriptSys->detach(e);
                    m_sceneDirty = true;
                    showExports = false;
                }
                std::vector<ScriptExport> exps =
                    showExports ? m_scriptSys->exportsOf(e) : std::vector<ScriptExport>{};
                if (showExports && exps.empty()) {
                    FluentUI::Label("Sin variables export.", std::nullopt,
                                    FluentUI::TypographyStyle::Caption);
                } else if (showExports) {
                    // Un scope de id por export: dos scripts distintos pueden exportar el
                    // mismo nombre y los widgets colisionarían (los ids salen del label).
                    int idx = 0;
                    for (ScriptExport& ex : exps) {
                        FluentUI::PushID(idx++);
                        switch (ex.type) {
                            case ScriptExport::Type::Number: {
                                double v = ex.number;
                                if (FluentUI::NumberBox(ex.name, &v, -1e9, 1e9, 1.0, "%.3f")) {
                                    ex.number = v;
                                    m_scriptSys->setExport(e, ex);
                                    m_sceneDirty = true;
                                }
                                break;
                            }
                            case ScriptExport::Type::Bool:
                                // ToggleSwitch en vez de Checkbox: es un ajuste on/off en vivo
                                // (el patrón WinUI para propiedades booleanas de un inspector).
                                if (FluentUI::ToggleSwitch(ex.name, &ex.boolean, "Sí", "No")) {
                                    m_scriptSys->setExport(e, ex);
                                    m_sceneDirty = true;
                                }
                                break;
                            case ScriptExport::Type::Text:
                                if (FluentUI::TextInput(ex.name, &ex.text)) {
                                    m_scriptSys->setExport(e, ex);
                                    m_sceneDirty = true;
                                }
                                break;
                        }
                        FluentUI::PopID();
                    }
                }
                FluentUI::EndExpander();
            }

            // "Agregar componente" (patrón Unity): un desplegable con los componentes
            // disponibles; los que la entidad ya tiene salen deshabilitados. Ni Script ni
            // Apariencia añaden nada hasta que su diálogo confirma un archivo (así no
            // queda un componente vacío si se cancela).
            FluentUI::Separator();
            const std::vector<FluentUI::CommandItem> addable = {
                { "Apariencia…", FluentUI::Icons::FileImage,
                  [this, e] { openTextureDialog(e); },
                  true, !s->has<SpriteComponent>(e) },
                { "Script…", FluentUI::Icons::FileCode,
                  [this, e] { openScriptDialog(e); },
                  true, !s->has<ScriptComponent>(e) },
                { "Script nuevo…", FluentUI::Icons::FilePlus,
                  [this, e] { openNewScriptDialog(e); },
                  true, !s->has<ScriptComponent>(e) },
                { "Cámara", FluentUI::Icons::Camera,
                  [this, s, e] { s->add<CameraComponent>(e, CameraComponent{}); m_sceneDirty = true; },
                  true, !s->has<CameraComponent>(e) },
                { "Nombre", FluentUI::Icons::Tag,
                  [this, s, e] { s->add<NameComponent>(e, NameComponent{ "Entidad" }); m_sceneDirty = true; },
                  true, !s->has<NameComponent>(e) },
            };
            FluentUI::DropDownButton("Agregar componente", FluentUI::Icons::Plus, addable);
        } else {
            FluentUI::Label("Nada seleccionado", std::nullopt, FluentUI::TypographyStyle::Subtitle);
            FluentUI::Separator();
            FluentUI::Label("Click en una entidad de la");
            FluentUI::Label("jerarquía o del viewport.");
        }
    }
    FluentUI::EndScrollView();
}

// --- Panel inferior (pane inferior del centro): pestañas Consola / Assets ---
void EditorUI::buildConsoleTabs() {
    using FluentUI::Vec2;
    auto* c = FluentUI::GetContext();
    if (!c) return;
    const Vec2 ps = paneSize(c);
    // TabView directo (sin BeginPanel): el pane del Splitter es el contenedor.
    if (FluentUI::BeginTabView("bottomTabs", &m_bottomTab,
                               std::vector<std::string>{ "Consola", "Assets" }, ps)) {
        if (m_bottomTab == 0) {
            if (FluentUI::BeginScrollView("console_scroll", Vec2(ps.x - 24.0f, ps.y - 56.0f))) {
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

            // Ruta de la carpeta abierta como migas de pan: clicar una sube a ese nivel
            // (antes solo se podía navegar por el árbol de la izquierda).
            std::vector<std::string> crumbs = splitPath(m_selectedFolder);
            const int crumbHit = FluentUI::BreadcrumbBar("assetsCrumbs", crumbs);
            if (crumbHit >= 0) {
                std::string p = crumbs[0];
                for (int i = 1; i <= crumbHit; ++i) p += "/" + crumbs[static_cast<size_t>(i)];
                m_selectedFolder = p;
            }

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
            FluentUI::SameLine(8.0f);
            // Crear un .lua vacío (con la plantilla) sin salir del editor. Aquí no hay
            // entidad destino: se crea y aparece en el navegador; para adjuntarlo, se
            // arrastra a una entidad o se usa "Agregar componente" en el inspector.
            if (FluentUI::Button("Nuevo script…")) openNewScriptDialog(Entity{});
            FluentUI::SameLine(8.0f);
            // Buscador global: escribe y elige; el navegador salta a la carpeta del asset.
            const std::string picked = FluentUI::AutoSuggestBox(
                "assetSearch", &m_assetFilter,
                [this](const std::string& q) {
                    std::string lower = q;
                    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    std::vector<std::string> hits;
                    collectAssetMatches(m_assetBrowser.root(), lower, hits, 12);
                    return hits;
                },
                "Buscar asset…");
            if (!picked.empty()) {
                m_selectedFolder = parentDir(picked);
                m_assetFilter.clear();
            }

            const float areaH = ps.y - 76.0f;                // bajo la barra de tabs + botones
            const float treeW = 168.0f;                      // ancho del árbol de carpetas
            const float gridW = ps.x - treeW - 28.0f;        // resto para el grid

            FluentUI::BeginHorizontal(8.0f);
            if (FluentUI::BeginTreeView("foldersTree", Vec2(treeW, areaH)))
                drawFolderTree(m_assetBrowser.root());
            FluentUI::EndTreeView();

            // Contenedor de tamaño fijo (NO scroll): el GridView de drawAssetCards ya gestiona su
            // propio scroll/virtualización; un ScrollView externo duplicaría el desplazamiento.
            FluentUI::BeginVertical(0.0f, Vec2(gridW, areaH));
            if (const AssetNode* f = findFolder(m_assetBrowser.root(), m_selectedFolder))
                drawAssetCards(*f, gridW);
            FluentUI::EndVertical();
            FluentUI::EndHorizontal();
        }
    }
    FluentUI::EndTabView();
}

// ─── HUD flotante ────────────────────────────────────────────────────────────────
// Rect que la UI tapa este frame. El picking del modo los resta del viewport (que ahora
// es la ventana entera): sin esto, clicar un panel movería al jugador por detrás.
void EditorUI::pushUiRect(const FluentUI::Rect& r) {
    if (r.size.x > 0.0f && r.size.y > 0.0f) m_uiRects.push_back(r);
}

// Superficie flotante del HUD: sombra + fondo TRANSLÚCIDO + cabecera con título y chevron.
// El alpha es real: el pass del editor usa loadOp=LOAD, así que el fondo se compone sobre
// el juego ya renderizado. Recorta el contenido a su rect y publica el rect para el picking.
// Plegado (open != nullptr && !*open) dibuja solo la cabecera y devuelve false.
// ¿Ese panel lo ha movido el usuario? (lo consulta el layout: un inspector arrastrado deja
// de perseguir a la selección).
bool EditorUI::overlayMoved(const char* id) const {
    auto it = m_overlayState.find(id);
    return it != m_overlayState.end() &&
           (it->second.offset.x != 0.0f || it->second.offset.y != 0.0f);
}

void EditorUI::resetOverlayPositions() {
    // Restablecer es volver al punto de partida: paneles sueltos, en su sitio y su tamaño.
    // También se BORRA el layout guardado: si lo que había en él dejaba el editor torcido,
    // esta es la salida de emergencia (se vuelve a escribir al cerrar, ya con el estado sano).
    {
        std::error_code ec;
        std::filesystem::remove(Project::instance().resolveWrite("editor_layout.ini"), ec);
    }
    m_clusters = { { { { { PanelId::Hierarchy }, 0 } }, { 1.0f }, false },
                   { { { { PanelId::Inspector }, 0 } }, { 1.0f }, false },
                   { { { { PanelId::Console   }, 0 } }, { 1.0f }, false } };
    for (auto& kv : m_overlayState) {
        kv.second.offset    = FluentUI::Vec2(0.0f, 0.0f);
        kv.second.sizeDelta = FluentUI::Vec2(0.0f, 0.0f);
        kv.second.dragging  = false;
        kv.second.edge      = 0;
    }
}

// Tamaño con el que hay que dibujar (y colocar) el panel: el del layout más lo estirado.
FluentUI::Vec2 EditorUI::overlaySize(const char* id, FluentUI::Vec2 base) const {
    auto it = m_overlayState.find(id);
    if (it == m_overlayState.end()) return base;
    const FluentUI::Vec2 d = it->second.sizeDelta;
    // Sin estirado manual manda el layout TAL CUAL. Los mínimos son para que el usuario no
    // pueda encoger un panel hasta lo inmanejable, no para el tamaño calculado: aplicarlos
    // siempre inflaba a 96 px el alto de un panel PLEGADO (32) y lo dejaba flotando por
    // encima del borde inferior, desalineado del dock.
    if (d.x == 0.0f && d.y == 0.0f) return base;
    auto* c = FluentUI::GetContext();
    const float dpi = c ? c->dpiScale : 1.0f;
    return FluentUI::Vec2(std::max(220.0f * dpi, base.x + d.x),
                          std::max(96.0f  * dpi, base.y + d.y));
}

bool EditorUI::beginOverlay(const OverlayDesc& d) {
    using FluentUI::Vec2;
    auto* c = FluentUI::GetContext();
    if (!c) return false;

    // --- Arrastre por la cabecera ---------------------------------------------------
    // La posición que da el layout es el SITIO POR DEFECTO; encima se aplica lo que el
    // usuario haya arrastrado. El offset se recalcula contra ese sitio en cada frame, así
    // que al redimensionar la ventana el panel sigue anclado donde lo dejaste.
    OverlayState& st = m_overlayState[d.id];
    const float dpiS  = c->dpiScale;
    const float headS = 32.0f * dpiS;
    const float mxs = c->input.MouseX(), mys = c->input.MouseY();
    const Vec2  view = c->renderer.GetViewportSize();
    // --- Redimensión por bordes y esquinas -----------------------------------------
    // El borde agarrado sigue al cursor. Con un panel anclado a un borde de la ventana (el
    // inspector a la derecha, la consola abajo) su posición se DERIVA del tamaño, así que
    // ahí estirar por el lado contrario es lo que lo agranda; por el lado del ancla, lo mueve.
    if (d.resizable && (!d.open || *d.open)) {
        const Vec2 p0 = d.pos + st.offset;
        const float m = 6.0f * dpiS;
        const bool inX = mxs >= p0.x - m && mxs <= p0.x + d.size.x + m;
        const bool inY = mys >= p0.y - m && mys <= p0.y + d.size.y + m;
        int edge = 0;
        if (inY && std::fabs(mxs - p0.x) <= m)              edge |= 1;   // izquierda
        if (inY && std::fabs(mxs - (p0.x + d.size.x)) <= m) edge |= 2;   // derecha
        if (inX && std::fabs(mys - p0.y) <= m)              edge |= 4;   // arriba
        if (inX && std::fabs(mys - (p0.y + d.size.y)) <= m) edge |= 8;   // abajo

        const int active = st.edge ? st.edge : edge;
        if (active) {
            const bool horiz = (active & 3) != 0, vert = (active & 12) != 0;
            c->desiredCursor = (horiz && vert)
                ? (((active & 1) && (active & 4)) || ((active & 2) && (active & 8))
                       ? FluentUI::UIContext::CursorType::ResizeNWSE
                       : FluentUI::UIContext::CursorType::ResizeNESW)
                : (horiz ? FluentUI::UIContext::CursorType::ResizeH
                         : FluentUI::UIContext::CursorType::ResizeV);
        }
        if (edge && c->input.IsMousePressed(0) && !st.edge && !st.dragging) {
            st.edge        = edge;
            st.dragStart   = Vec2(mxs, mys);
            st.startOffset = st.offset;
            st.startSize   = st.sizeDelta;
        }
        if (st.edge) {
            if (c->input.IsMouseDown(0)) {
                const float dx = mxs - st.dragStart.x, dy = mys - st.dragStart.y;
                st.sizeDelta = st.startSize;
                st.offset    = st.startOffset;
                if (st.edge & 1) {                       // borde izquierdo
                    if (d.anchorRight) { st.sizeDelta.x = st.startSize.x - dx; }
                    else               { st.sizeDelta.x = st.startSize.x - dx; st.offset.x = st.startOffset.x + dx; }
                }
                if (st.edge & 2) {                       // borde derecho
                    if (d.anchorRight) st.offset.x    = st.startOffset.x + dx;
                    else               st.sizeDelta.x = st.startSize.x + dx;
                }
                if (st.edge & 4) {                       // borde superior
                    if (d.anchorBottom) { st.sizeDelta.y = st.startSize.y - dy; }
                    else                { st.sizeDelta.y = st.startSize.y - dy; st.offset.y = st.startOffset.y + dy; }
                }
                if (st.edge & 8) {                       // borde inferior
                    if (d.anchorBottom) st.offset.y    = st.startOffset.y + dy;
                    else                st.sizeDelta.y = st.startSize.y + dy;
                }
            } else {
                st.edge = 0;
            }
        }
    }

    {
        Vec2 p = d.pos + st.offset;
        // La franja de la derecha son los botones (chevron / maximizar): ahí no se arrastra.
        // Se reserva SOLO lo que ocupan de verdad: un panel acoplado no tiene ninguno, y con
        // 64 px fijos la mitad de su cabecera dejaba de arrastrar al estrecharse.
        const float btnZone = ((d.open ? 26.0f : 0.0f) + (d.maximized ? 26.0f : 0.0f)
                               + (d.open || d.maximized ? 8.0f : 0.0f)) * dpiS;
        const float grabW = std::max(40.0f * dpiS, d.size.x - btnZone);
        const bool overHeader = mxs >= p.x && mxs <= p.x + grabW &&
                                mys >= p.y && mys <= p.y + headS;
        if (overHeader && c->input.IsMousePressed(0) && !st.dragging && !st.edge) {
            // Doble clic: acoplado, lo SACA del bloque; suelto, lo devuelve a su sitio.
            if (c->time - st.lastClick < 0.35f) {
                if (d.inCluster) {
                    int ci2 = -1, mi2 = -1;
                    locateGroup(d.id, ci2, mi2);
                    if (ci2 >= 0 && mi2 >= 0) {   // se aplica al terminar el dibujado
                        m_undockCluster = ci2; m_undockMember = mi2; m_undockTab = -1;
                    }
                } else {
                    st.offset = Vec2(0.0f, 0.0f);
                }
                st.lastClick = -10.0f;
            } else {
                st.lastClick = c->time;
                st.dragging  = true;
                st.grab      = Vec2(mxs - p.x, mys - p.y);
            }
        }
        if (st.dragging) {
            if (c->input.IsMouseDown(0)) {
                // El desplazamiento se acumula en el ANCLA del cluster: arrastrar la cabecera
                // de cualquier miembro mueve el bloque entero en vez de partirlo.
                OverlayState& stMove = (d.moveId && d.moveId[0]) ? m_overlayState[d.moveId] : st;
                const Vec2 want(mxs - st.grab.x, mys - st.grab.y);   // dónde debería quedar
                stMove.offset = stMove.offset + (want - p);
                c->desiredCursor = FluentUI::UIContext::CursorType::Hand;   // no hay SizeAll
                // ¿Sobre otro panel? La zona decide el acople y pinta la silueta.
                m_dockHintSide = dockZoneAt(d.id, Vec2(mxs, mys), m_dockHintId, m_dockHintRect);
            } else {
                st.dragging = false;
                // El drop se resuelve fuera del bucle de dibujado (mueve grupos de sitio).
                m_dropSourceId = d.id;
                m_dropPos      = Vec2(mxs, mys);
            }
        }
        // Nunca fuera de la ventana: siempre queda cabecera suficiente para volver a cogerlo.
        // Solo se clampa el panel que POSEE el desplazamiento (un miembro acoplado no debe
        // recortar la posición del bloque entero).
        if (!d.moveId || !d.moveId[0]) {
            p = d.pos + st.offset;
            const float minX = -d.size.x + 120.0f * dpiS, maxX = view.x - 120.0f * dpiS;
            // En vertical se intenta que quepa ENTERO (si cabe): dejar solo la cabecera dentro
            // es lo que hacía aparecer un panel "plegado" pegado a un borde.
            const float minY = 0.0f;
            const float maxY = std::max(0.0f, view.y - std::max(headS, d.size.y));
            const Vec2 clamped(std::clamp(p.x, minX, maxX), std::clamp(p.y, minY, maxY));
            st.offset = clamped - d.pos;
        }
    }

    const FluentUI::Vec2 pos = d.pos + st.offset, size = d.size;
    bool* const open = d.open;
    bool* const maximized = d.maximized;
    const char* const note = d.note;
    const bool noteAlert = d.noteAlert;
    const float dpi    = c->dpiScale;
    const float headH  = 32.0f * dpi;
    const float radius = c->style.panel.cornerRadius * dpi;
    const bool  opened = (open == nullptr) || *open;
    const float h      = opened ? std::max(size.y, headH) : headH;

    // El rect se publica también plegado: la cabecera tapa igual.
    pushUiRect(FluentUI::Rect(pos, Vec2(size.x, h)));

    FluentUI::Color bg   = c->style.panel.background;        bg.a   = d.bgAlpha;
    FluentUI::Color head = c->style.panel.headerBackground;  head.a = 0.97f;
    c->renderer.DrawRectShadow(pos, Vec2(size.x, h), radius, 18.0f * dpi,
                               FluentUI::Color(0.0f, 0.0f, 0.0f, 0.45f), Vec2(0.0f, 5.0f * dpi));
    c->renderer.DrawRectFilled(pos, Vec2(size.x, h), bg, radius);
    c->renderer.DrawRectFilled(pos, Vec2(size.x, headH), head, radius);
    // La cabecera solo redondea ARRIBA cuando hay cuerpo debajo: se tapa su mitad inferior.
    if (opened)
        c->renderer.DrawRectFilled(Vec2(pos.x, pos.y + headH * 0.5f),
                                   Vec2(size.x, headH * 0.5f), head, 0.0f);
    c->renderer.DrawRect(pos, Vec2(size.x, h), c->style.panel.borderColor, radius);

    // Contenido de la cabecera: icono + título a la izquierda, chevron de plegado a la derecha.
    c->cursorPos = Vec2(pos.x + 10.0f * dpi, pos.y + (headH - 20.0f * dpi) * 0.5f);
    FluentUI::BeginHorizontal(8.0f, Vec2(size.x - 46.0f * dpi, 20.0f * dpi), Vec2(0.0f, 0.0f));
    FluentUI::IconLabel(d.icon, 16.0f);
    FluentUI::Label(d.title, std::nullopt, FluentUI::TypographyStyle::BodyStrong);
    FluentUI::EndHorizontal();
    // Aviso de la cabecera (a la izquierda del chevron): con el panel plegado es lo único
    // que se ve, así que es donde tiene que aparecer "3 errores".
    // Los botones de la cabecera se cuentan de derecha a izquierda: chevron y, si el panel
    // puede maximizarse, el de maximizar/restaurar. El aviso empieza donde acaban ellos.
    const float btnW = 26.0f * dpi;
    const int   btns = (open ? 1 : 0) + ((maximized && opened) ? 1 : 0);
    if (note && note[0]) {
        const float fs = c->style.typography.caption.fontSize * dpi;
        const FluentUI::Vec2 ns = c->renderer.MeasureText(note, fs);
        const FluentUI::Color col = noteAlert ? FluentUI::Color::FromHex("#ff99a4")
                                              : c->style.typography.caption.color;
        c->renderer.DrawText(Vec2(pos.x + size.x - 10.0f * dpi - btnW * static_cast<float>(btns) - ns.x,
                                  pos.y + (headH - ns.y) * 0.5f), note, col, fs);
    }
    if (maximized && opened) {
        c->cursorPos = Vec2(pos.x + size.x - 30.0f * dpi - btnW, pos.y + (headH - 24.0f * dpi) * 0.5f);
        if (FluentUI::IconButton(*maximized ? FluentUI::Icons::Shrink : FluentUI::Icons::Expand, 24.0f))
            *maximized = !*maximized;
    }
    if (open) {
        c->cursorPos = Vec2(pos.x + size.x - 30.0f * dpi, pos.y + (headH - 24.0f * dpi) * 0.5f);
        if (FluentUI::IconButton(opened ? FluentUI::Icons::ChevronDown
                                        : FluentUI::Icons::ChevronRight, 24.0f))
            *open = !opened;
    }
    if (!opened) return false;

    // Cuerpo: el contenido fluye dentro de un layout del tamaño exacto del panel (es lo que
    // leen paneSize/BeginScrollView) y se recorta a él.
    const Vec2 body(pos.x, pos.y + headH);
    const Vec2 bodySize(size.x, h - headH);
    // Los contenedores de dentro (TabView, ScrollView, TreeView) pintan su fondo desde
    // style.panel.background: bajarle el alpha mientras dura el overlay es lo que hace que la
    // translucidez se vea de verdad — si no, tapan el fondo translúcido con uno opaco.
    m_overlayPrevPanelAlpha = c->style.panel.background.a;
    c->style.panel.background.a = d.bgAlpha;
    c->renderer.PushClipRect(body, bodySize);
    c->cursorPos = body;
    FluentUI::BeginVertical(0.0f, bodySize, Vec2(0.0f, 0.0f));
    return true;
}

void EditorUI::endOverlay() {
    auto* c = FluentUI::GetContext();
    if (!c) return;
    FluentUI::EndVertical(/*advanceParent=*/false);
    c->renderer.PopClipRect();
    c->style.panel.background.a = m_overlayPrevPanelAlpha;   // el alpha es del overlay, no global
}

// Transporte (Play/Pausa/Stop) + herramientas de estado, en un dock flotante centrado abajo:
// siempre a mano y sin ocupar un panel. El editor solo PIDE la transición; quien para de
// verdad la simulación es el Engine (ver consumeRunModeRequest).
void EditorUI::buildTransportDock(float fw, float fh, float top,
                                  const FluentUI::Rect& bottom, bool preferTop) {
    using FluentUI::Vec2;
    auto* c = FluentUI::GetContext();
    if (!c) return;
    const float dpi = c->dpiScale;
    const float h   = 44.0f * dpi;
    const float pad = 10.0f * dpi;   // margen interior a cada lado del contenido
    // El dock se ajusta a lo que DIBUJA: los widgets (botones, separador, segmentos) se
    // autodimensionan por su contenido, así que un ancho fijo o les quedaba grande o —al
    // sumar la herramienta de gizmo— los dejaba pegados al borde. Se mide el ancho real
    // consumido y se usa en el frame siguiente (mismo lag de 1 frame que el resto del HUD,
    // imperceptible: el contenido del dock solo cambia si cambian sus botones).
    const float w = std::max(180.0f * dpi, m_dockContentW + pad * 2.0f);
    // Mismo margen inferior que el panel de consola: así comparten línea base.
    Vec2 pos(fw * 0.5f - w * 0.5f, fh - 16.0f * dpi - h);
    if (preferTop) {
        // Panel inferior grande: el dock cruza la ventana entera y se ancla arriba, bajo la
        // barra de título, donde se lee como lo que es (control del editor, no del panel).
        pos.y = top + 16.0f * dpi;
    } else {
        // Si el panel inferior llega hasta donde va el dock, el dock se coloca justo encima:
        // es el HUD el que se aparta, nunca el juego el que pierde sitio.
        const bool overlaps = bottom.size.x > 0.0f && bottom.size.y > 0.0f &&
                              pos.x < bottom.Right() && pos.x + w > bottom.pos.x &&
                              pos.y < bottom.Bottom();
        if (overlaps) pos.y = std::max(0.0f, bottom.pos.y - 12.0f * dpi - h);
    }
    const float radius = c->style.panel.cornerRadius * dpi;
    pushUiRect(FluentUI::Rect(pos, Vec2(w, h)));

    FluentUI::Color bg = c->style.panel.headerBackground; bg.a = 0.95f;
    c->renderer.DrawRectShadow(pos, Vec2(w, h), radius, 20.0f * dpi,
                               FluentUI::Color(0.0f, 0.0f, 0.0f, 0.5f), Vec2(0.0f, 6.0f * dpi));
    c->renderer.DrawRectFilled(pos, Vec2(w, h), bg, radius);
    c->renderer.DrawRect(pos, Vec2(w, h), c->style.panel.borderColor, radius);

    const float contentX0 = pos.x + pad;
    c->cursorPos = Vec2(contentX0, pos.y + (h - 28.0f * dpi) * 0.5f);
    FluentUI::BeginHorizontal(6.0f, Vec2(w - pad * 2.0f, 28.0f * dpi), Vec2(0.0f, 0.0f));
    // El estado se lee de qué botón está disponible: en edición solo se puede dar a Play;
    // jugando, a Pausa o Stop.
    if (FluentUI::IconButton(FluentUI::Icons::Play, 0.0f, std::nullopt,
                             m_runMode != RunMode::Play)) {
        m_runModeRequest   = RunMode::Play;    // desde Edit arranca; desde Pausa reanuda
        m_runModeRequested = true;
    }
    if (FluentUI::IconButton(FluentUI::Icons::Pause, 0.0f, std::nullopt,
                             m_runMode == RunMode::Play)) {
        m_runModeRequest   = RunMode::Paused;
        m_runModeRequested = true;
    }
    if (FluentUI::IconButton(FluentUI::Icons::Stop, 0.0f, std::nullopt,
                             m_runMode != RunMode::Edit)) {
        m_runModeRequest   = RunMode::Edit;
        m_runModeRequested = true;
    }
    FluentUI::Separator();
    // Herramienta de manipulación: es lo que se cambia sin parar mientras se edita, así que
    // vive aquí. Lo de HD-2D era una opción del PROYECTO y se movió al menú Proyecto.
    FluentUI::SegmentedControl("gizmoTool",
        std::vector<std::pair<std::string, uint32_t>>{
            { "", FluentUI::Icons::Pointer },
            { "", FluentUI::Icons::Move },
            { "", FluentUI::Icons::RotateCw },
            { "", FluentUI::Icons::Scaling },   // Scale en Lucide es una BALANZA, no escalar
        }, &m_gizmoTool);
    // Ancho realmente ocupado por la fila: lo lee el próximo frame para dimensionar el dock.
    // Se toma ANTES de EndHorizontal, que devuelve el cursor al layout padre.
    m_dockContentW = std::max(0.0f, c->cursorPos.x - contentX0);
    FluentUI::EndHorizontal();
}

// Visibilidad de los paneles del HUD, persistida por proyecto (ver shutdown). Si el archivo
// no existe o no parsea se mantienen los valores por defecto.
void EditorUI::loadHudState() {
    std::ifstream f(Project::instance().resolveWrite("editor_layout.ini"));
    int hier = 0, insp = 0, cons = 0;
    if (f && (f >> hier >> insp >> cons)) {
        m_showHierarchy = hier != 0;
        m_showInspector = insp != 0;
        m_showConsole   = cons != 0;
        // Pestaña y maximizado son opcionales: un .ini de la versión anterior solo trae los
        // tres primeros, y entonces se quedan los valores por defecto.
        int tab = 0, mx = 0;
        if (f >> tab >> mx) { m_bottomTab = (tab == 1) ? 1 : 0; m_bottomMax = mx != 0; }
        for (const char* id : { "ovl_hier", "ovl_insp", "ovl_console" }) {
            float ox = 0.0f, oy = 0.0f, sx = 0.0f, sy = 0.0f;
            if (!(f >> ox >> oy >> sx >> sy)) break;
            m_overlayState[id].offset    = FluentUI::Vec2(ox, oy);
            m_overlayState[id].sizeDelta = FluentUI::Vec2(sx, sy);
        }

        // Disposición de paneles. Se acepta solo si cada panel aparece EXACTAMENTE una vez;
        // cualquier otra cosa (un .ini a medio escribir, editado a mano) deja la disposición
        // por defecto en vez de dejar el HUD sin un panel.
        std::string layout;
        if (f >> layout) {
            std::vector<DockCluster> parsed;
            int seen[3] = { 0, 0, 0 };
            bool ok = true;
            DockCluster cur;
            PanelGroup  grp;
            bool anchorNext = false;
            auto flushGroup = [&]() { if (!grp.tabs.empty()) { cur.members.push_back(grp); grp = PanelGroup{}; } };
            auto flushCluster = [&]() {
                flushGroup();
                if (!cur.members.empty()) {
                    cur.ratios.assign(cur.members.size(), 1.0f / static_cast<float>(cur.members.size()));
                    parsed.push_back(cur);
                }
                cur = DockCluster{};
            };
            for (char ch : layout) {
                if (ch == ';')      { flushCluster(); continue; }
                if (ch == ',')      { flushGroup();   continue; }
                if (ch == 'h' || ch == 'v') { cur.vertical = (ch == 'v'); continue; }
                if (ch == '*') { anchorNext = true; continue; }
                const int idx = (ch == 'H') ? 0 : (ch == 'I') ? 1 : (ch == 'C') ? 2 : -1;
                if (idx < 0 || ++seen[idx] > 1) { ok = false; break; }
                const PanelId parsedId = idx == 0 ? PanelId::Hierarchy
                                       : idx == 1 ? PanelId::Inspector : PanelId::Console;
                if (anchorNext) { cur.anchor = parsedId; anchorNext = false; }
                grp.tabs.push_back(parsedId);
            }
            flushCluster();
            if (ok && seen[0] == 1 && seen[1] == 1 && seen[2] == 1 && !parsed.empty())
                m_clusters = std::move(parsed);
        }

        // Saneamiento: la posición/tamaño manuales se guardaron para paneles SUELTOS. Si al
        // reabrir hay paneles acoplados, esa geometría ya no describe nada real y deja el
        // editor arrancando torcido — se descarta y el bloque toma su sitio por defecto.
        bool anyDocked = false;
        for (const DockCluster& cl : m_clusters)
            if (cl.members.size() > 1) { anyDocked = true; break; }
        if (anyDocked) {
            for (auto& kv : m_overlayState) {
                kv.second.offset    = FluentUI::Vec2(0.0f, 0.0f);
                kv.second.sizeDelta = FluentUI::Vec2(0.0f, 0.0f);
            }
        } else {
            // Sueltos: se respetan, pero acotados. Un .ini con valores disparatados (o de una
            // ventana mucho mayor) no debe dejar un panel fuera de la pantalla.
            for (auto& kv : m_overlayState) {
                kv.second.offset.x    = std::clamp(kv.second.offset.x,    -4000.0f, 4000.0f);
                kv.second.offset.y    = std::clamp(kv.second.offset.y,    -4000.0f, 4000.0f);
                kv.second.sizeDelta.x = std::clamp(kv.second.sizeDelta.x, -2000.0f, 2000.0f);
                kv.second.sizeDelta.y = std::clamp(kv.second.sizeDelta.y, -2000.0f, 2000.0f);
            }
        }
    }
}

// ─── Paneles del HUD: identidad y contenido ──────────────────────────────────────
const char* EditorUI::panelOverlayId(PanelId p) {
    switch (p) {
        case PanelId::Hierarchy: return "ovl_hier";
        case PanelId::Inspector: return "ovl_insp";
        default:                 return "ovl_console";
    }
}

const char* EditorUI::panelTitle(PanelId p) const {
    switch (p) {
        case PanelId::Hierarchy: return "Escena";
        case PanelId::Inspector: return "Inspector";
        // La consola cambia de nombre con su pestaña interna (Consola / Assets).
        default:                 return (m_bottomTab != 0) ? "Assets" : "Consola";
    }
}

uint32_t EditorUI::panelIcon(PanelId p) const {
    switch (p) {
        case PanelId::Hierarchy: return FluentUI::Icons::ListTree;
        case PanelId::Inspector: return FluentUI::Icons::Settings;
        default: return (m_bottomTab != 0) ? FluentUI::Icons::FolderOpen : FluentUI::Icons::Terminal;
    }
}

void EditorUI::buildPanelContent(PanelId p) {
    switch (p) {
        case PanelId::Hierarchy: buildHierarchyPanel(); break;
        case PanelId::Inspector: buildInspectorPanel(); break;
        default:                 buildConsoleTabs();    break;
    }
}

// El ancla vale mientras siga siendo miembro del cluster; si se fue, manda el primero.
EditorUI::PanelId EditorUI::clusterAnchor(const DockCluster& cl) const {
    for (const PanelGroup& g : cl.members)
        for (PanelId p : g.tabs)
            if (p == cl.anchor) return cl.anchor;
    return cl.members.empty() || cl.members[0].tabs.empty() ? PanelId::Hierarchy
                                                            : cl.members[0].tabs[0];
}

EditorUI::DockSide EditorUI::dockZoneAt(const std::string& srcId, FluentUI::Vec2 p,
                                        std::string& outTarget, FluentUI::Rect& outRect) const {
    for (const auto& kv : m_panelRectsPrev) {
        if (kv.first == srcId) continue;
        const FluentUI::Rect& r = kv.second;
        if (!r.Contains(p)) continue;
        outTarget = kv.first;
        const float fx = (p.x - r.pos.x) / std::max(1.0f, r.size.x);
        const float fy = (p.y - r.pos.y) / std::max(1.0f, r.size.y);
        auto head = m_headerRectsPrev.find(kv.first);
        const bool onHeader = head != m_headerRectsPrev.end() && head->second.Contains(p);
        // La cabecera (o el centro) acopla como PESTAÑA; el cuarto exterior de cada lado,
        // a ese lado. La silueta muestra dónde acabaría el panel.
        if (onHeader || (fx > 0.25f && fx < 0.75f && fy > 0.25f && fy < 0.75f)) {
            outRect = (head != m_headerRectsPrev.end()) ? head->second : r;
            return DockSide::Tab;
        }
        if (fx <= 0.25f) {
            outRect = FluentUI::Rect(r.pos, FluentUI::Vec2(r.size.x * 0.5f, r.size.y));
            return DockSide::Left;
        }
        if (fx >= 0.75f) {
            outRect = FluentUI::Rect(FluentUI::Vec2(r.pos.x + r.size.x * 0.5f, r.pos.y),
                                     FluentUI::Vec2(r.size.x * 0.5f, r.size.y));
            return DockSide::Right;
        }
        if (fy <= 0.25f) {
            outRect = FluentUI::Rect(r.pos, FluentUI::Vec2(r.size.x, r.size.y * 0.5f));
            return DockSide::Top;
        }
        outRect = FluentUI::Rect(FluentUI::Vec2(r.pos.x, r.pos.y + r.size.y * 0.5f),
                                 FluentUI::Vec2(r.size.x, r.size.y * 0.5f));
        return DockSide::Bottom;
    }
    outTarget.clear();
    return DockSide::None;
}

void EditorUI::locateGroup(const std::string& overlayId, int& cluster, int& member) const {
    cluster = member = -1;
    for (size_t ci = 0; ci < m_clusters.size(); ++ci)
        for (size_t mi = 0; mi < m_clusters[ci].members.size(); ++mi) {
            const PanelGroup& g = m_clusters[ci].members[mi];
            if (!g.tabs.empty() && overlayId == panelOverlayId(g.tabs[0])) {
                cluster = static_cast<int>(ci);
                member  = static_cast<int>(mi);
                return;
            }
        }
}

// Normaliza los ratios de un cluster para que sumen 1 (tras insertar o quitar miembros).
static void normalizeRatios(std::vector<float>& r) {
    float sum = 0.0f;
    for (float v : r) sum += std::max(0.05f, v);
    if (sum <= 0.0f) { for (float& v : r) v = 1.0f / static_cast<float>(r.size()); return; }
    for (float& v : r) v = std::max(0.05f, v) / sum;
}

// Acopla como PESTAÑA: las pestañas del grupo movido pasan al grupo destino.
void EditorUI::dockAsTab(const std::string& movedId, const std::string& targetId) {
    int mc, mm, tc, tm;
    locateGroup(movedId, mc, mm);
    locateGroup(targetId, tc, tm);
    if (mc < 0 || tc < 0 || (mc == tc && mm == tm)) return;
    PanelGroup moved = m_clusters[static_cast<size_t>(mc)].members[static_cast<size_t>(mm)];
    // Quitar primero el movido (puede estar en el mismo cluster que el destino).
    {
        DockCluster& src = m_clusters[static_cast<size_t>(mc)];
        src.members.erase(src.members.begin() + mm);
        src.ratios.erase(src.ratios.begin() + mm);
        if (src.members.empty()) m_clusters.erase(m_clusters.begin() + mc);
        else                     normalizeRatios(src.ratios);
        if (mc < tc) --tc;                       // el destino pudo desplazarse
        else if (mc == tc && mm < tm) --tm;
    }
    PanelGroup& to = m_clusters[static_cast<size_t>(tc)].members[static_cast<size_t>(tm)];
    for (PanelId p : moved.tabs) to.tabs.push_back(p);
    to.active = static_cast<int>(to.tabs.size()) - 1;
    m_overlayState.erase(panelOverlayId(moved.tabs[0]));   // ya no tiene superficie propia
}

// Acopla POR UN LADO: el grupo movido entra en el cluster del destino, que se reparte su
// rectángulo. Si el cluster del destino ya tiene varios miembros con OTRA orientación, se
// saca el destino a un cluster nuevo con los dos: para tres paneles es más predecible que
// anidar divisiones.
void EditorUI::dockBeside(const std::string& movedId, const std::string& targetId, DockSide side) {
    if (side != DockSide::Left && side != DockSide::Right &&
        side != DockSide::Top  && side != DockSide::Bottom) return;
    int mc, mm, tc, tm;
    locateGroup(movedId, mc, mm);
    locateGroup(targetId, tc, tm);
    if (mc < 0 || tc < 0 || (mc == tc && mm == tm)) return;

    const bool vertical = (side == DockSide::Top || side == DockSide::Bottom);
    const bool before   = (side == DockSide::Left || side == DockSide::Top);

    PanelGroup moved = m_clusters[static_cast<size_t>(mc)].members[static_cast<size_t>(mm)];
    // Su posición/tamaño manuales dejan de valer: dentro del cluster manda el reparto. Sin
    // esto, el panel se seguía dibujando desplazado por donde el ratón lo dejó — es decir,
    // fuera del hueco que le corresponde.
    if (!moved.tabs.empty()) m_overlayState.erase(panelOverlayId(moved.tabs[0]));
    {
        DockCluster& src = m_clusters[static_cast<size_t>(mc)];
        src.members.erase(src.members.begin() + mm);
        src.ratios.erase(src.ratios.begin() + mm);
        if (src.members.empty()) m_clusters.erase(m_clusters.begin() + mc);
        else                     normalizeRatios(src.ratios);
        if (mc < tc) --tc;
        else if (mc == tc && mm < tm) --tm;
    }

    DockCluster& dst = m_clusters[static_cast<size_t>(tc)];
    if (dst.members.size() > 1 && dst.vertical != vertical) {
        // Orientación incompatible: el destino se independiza y forma cluster con el movido.
        PanelGroup target = dst.members[static_cast<size_t>(tm)];
        dst.members.erase(dst.members.begin() + tm);
        dst.ratios.erase(dst.ratios.begin() + tm);
        normalizeRatios(dst.ratios);
        DockCluster fresh;
        fresh.vertical = vertical;
        if (before) { fresh.members = { moved, target }; }
        else        { fresh.members = { target, moved }; }
        fresh.ratios = { 0.5f, 0.5f };
        fresh.anchor = target.tabs.empty() ? PanelId::Hierarchy : target.tabs[0];
        m_clusters.push_back(std::move(fresh));
        return;
    }
    dst.vertical = vertical;
    // El bloque resultante se coloca por la geometría POR DEFECTO de su ancla: si conserva
    // los desplazamientos/estirados que cualquiera de los dos tuviera de antes, el conjunto
    // aparece descolocado (y así se quedaba guardado en el .ini).
    m_overlayState.erase(panelOverlayId(clusterAnchor(dst)));
    const int at = before ? tm : tm + 1;
    // El nuevo miembro entra con el mismo peso que el destino, que cede la mitad del suyo.
    // El ratio del destino se lee y se ajusta ANTES de insertar: hacerlo después obligaba a
    // corregir el índice a mano y se salía del vector cuando el cluster tenía un solo miembro.
    const float share = dst.ratios[static_cast<size_t>(tm)] * 0.5f;
    dst.ratios[static_cast<size_t>(tm)] = share;
    dst.members.insert(dst.members.begin() + at, moved);
    dst.ratios.insert(dst.ratios.begin() + at, share);
    normalizeRatios(dst.ratios);
}

// Saca un grupo de su cluster a uno propio (vuelve a flotar en su sitio por defecto).
void EditorUI::undockGroup(int cluster, int member) {
    if (cluster < 0 || cluster >= static_cast<int>(m_clusters.size())) return;
    DockCluster& src = m_clusters[static_cast<size_t>(cluster)];
    if (src.members.size() <= 1 || member < 0 ||
        member >= static_cast<int>(src.members.size())) return;
    PanelGroup g = src.members[static_cast<size_t>(member)];
    src.members.erase(src.members.begin() + member);
    src.ratios.erase(src.ratios.begin() + member);
    normalizeRatios(src.ratios);
    m_overlayState.erase(panelOverlayId(g.tabs[0]));   // recupera su posición y tamaño de origen
    // El que SE QUEDA también: conservaba el desplazamiento del bloque y, al recuperar su
    // altura completa, se salía de la ventana (se veía solo su cabecera en una esquina).
    if (!src.members.empty()) m_overlayState.erase(panelOverlayId(clusterAnchor(src)));
    DockCluster fresh;
    fresh.members = { g };
    fresh.ratios  = { 1.0f };
    fresh.anchor  = g.tabs.empty() ? PanelId::Hierarchy : g.tabs[0];
    m_clusters.push_back(std::move(fresh));
}

// Separa una pestaña a su propia superficie flotante.
void EditorUI::undockTab(int cluster, int member, int tab) {
    if (cluster < 0 || cluster >= static_cast<int>(m_clusters.size())) return;
    DockCluster& cl = m_clusters[static_cast<size_t>(cluster)];
    if (member < 0 || member >= static_cast<int>(cl.members.size())) return;
    PanelGroup& g = cl.members[static_cast<size_t>(member)];
    if (g.tabs.size() <= 1 || tab < 0 || tab >= static_cast<int>(g.tabs.size())) return;
    const PanelId p = g.tabs[static_cast<size_t>(tab)];
    g.tabs.erase(g.tabs.begin() + tab);
    g.active = std::clamp(g.active, 0, static_cast<int>(g.tabs.size()) - 1);
    m_overlayState.erase(panelOverlayId(p));
    DockCluster fresh;
    fresh.members = { PanelGroup{ { p }, 0 } };
    fresh.ratios  = { 1.0f };
    fresh.anchor  = p;
    m_clusters.push_back(std::move(fresh));
}

// Fila de pestañas de un grupo agrupado. Doble clic en una pestaña la saca del grupo.
bool EditorUI::buildGroupTabs(PanelGroup& g, float width, int cluster, int member) {
    using FluentUI::Vec2;
    auto* c = FluentUI::GetContext();
    if (!c || g.tabs.size() <= 1) return false;
    const float dpi = c->dpiScale;
    const float h   = 28.0f * dpi;
    const Vec2  pos = c->cursorPos;
    const float tabW = std::min(150.0f * dpi, width / static_cast<float>(g.tabs.size()));
    const float mx = c->input.MouseX(), my = c->input.MouseY();
    bool changed = false;
    int  undock  = -1;
    for (size_t i = 0; i < g.tabs.size(); ++i) {
        const Vec2 tp(pos.x + tabW * static_cast<float>(i), pos.y);
        const bool hover = mx >= tp.x && mx <= tp.x + tabW && my >= tp.y && my <= tp.y + h;
        const bool on    = (static_cast<int>(i) == g.active);
        if (hover) {
            FluentUI::Color hv = c->style.panel.headerBackground; hv.a = 0.8f;
            c->renderer.DrawRectFilled(tp, Vec2(tabW, h), hv, 0.0f);
        }
        if (on)   // subrayado de acento, igual que el TabView del tema
            c->renderer.DrawRectFilled(Vec2(tp.x, tp.y + h - 2.0f * dpi), Vec2(tabW, 2.0f * dpi),
                                       c->style.accentColor, 0.0f);
        c->cursorPos = Vec2(tp.x + 10.0f * dpi, tp.y + (h - 18.0f * dpi) * 0.5f);
        FluentUI::BeginHorizontal(6.0f, Vec2(tabW - 16.0f * dpi, 18.0f * dpi), Vec2(0.0f, 0.0f));
        FluentUI::IconLabel(panelIcon(g.tabs[i]), 14.0f);
        FluentUI::Label(panelTitle(g.tabs[i]), std::nullopt,
                        on ? FluentUI::TypographyStyle::BodyStrong : FluentUI::TypographyStyle::Body,
                        !on);
        FluentUI::EndHorizontal();
        if (hover && c->input.IsMousePressed(0)) {
            // Doble clic sobre la pestaña = sacarla del grupo (mismo gesto que restablecer).
            auto& st = m_overlayState[std::string("tab_") + panelOverlayId(g.tabs[i])];
            if (c->time - st.lastClick < 0.35f) { undock = static_cast<int>(i); st.lastClick = -10.0f; }
            else { st.lastClick = c->time; g.active = static_cast<int>(i); changed = true; }
        }
    }
    c->cursorPos = Vec2(pos.x, pos.y + h);
    if (undock >= 0) {   // diferido: m_clusters no se toca a mitad de dibujado
        m_undockCluster = cluster; m_undockMember = member; m_undockTab = undock;
        changed = true;
    }
    return changed;
}

// ─── Paleta de comandos ──────────────────────────────────────────────────────────
// Atajos del editor. Van ANTES de construir el HUD para que el frame ya se dibuje con el
// estado nuevo (abrir la paleta y escribir en el mismo frame).
void EditorUI::handleShortcuts() {
    auto* c = FluentUI::GetContext();
    if (!c) return;
    const bool ctrl = c->input.CtrlDown();
    if (ctrl && c->input.IsKeyPressed(FluentUI::UIKey::K)) {
        m_paletteOpen  = !m_paletteOpen;
        m_paletteFocus = m_paletteOpen;   // el campo se enfoca solo al abrir
        m_paletteSel   = 0;
        m_paletteQuery.clear();
    }
    if (m_paletteOpen && c->input.IsKeyPressed(FluentUI::UIKey::Escape)) m_paletteOpen = false;
    // Plegar/desplegar los paneles del HUD sin pasar por el menú.
    if (ctrl && c->input.IsKeyPressed(FluentUI::UIKey::Num1)) m_showHierarchy = !m_showHierarchy;
    if (ctrl && c->input.IsKeyPressed(FluentUI::UIKey::Num2)) m_showInspector = !m_showInspector;
    if (ctrl && c->input.IsKeyPressed(FluentUI::UIKey::Num3)) m_showConsole   = !m_showConsole;
    // Supr borra la entidad seleccionada, salvo mientras se escribe en la paleta (su campo
    // de texto usa Supr para editar).
    if (!m_paletteOpen && c->input.IsKeyPressed(FluentUI::UIKey::Delete)) deleteSelectedEntity();
}

// Todo lo que la paleta puede ejecutar, ya filtrado por `query` (subcadena sin distinguir
// mayúsculas sobre etiqueta + pista). El orden es el de relevancia: comandos, entidades de
// la escena y assets del proyecto.
void EditorUI::collectPaletteItems(const std::string& query, std::vector<PaletteItem>& out) {
    constexpr size_t kMaxItems = 12;
    std::string q = query;
    for (char& ch : q) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    auto matches = [&q](const std::string& label, const std::string& hint) {
        if (q.empty()) return true;
        std::string hay = label + " " + hint;
        for (char& ch : hay) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return hay.find(q) != std::string::npos;
    };
    auto add = [&](const std::string& label, const std::string& hint, uint32_t icon,
                   std::function<void()> run) {
        if (out.size() >= kMaxItems || !matches(label, hint)) return;
        out.push_back(PaletteItem{ label, hint, icon, std::move(run) });
    };

    // Jugando, la escena en memoria es una partida que se descartará al parar: lo que la
    // guarde o la sustituya se deshabilita, igual que en el menú.
    const bool editing = (m_runMode == RunMode::Edit);
    if (editing) {
        add("Guardar escena", "Comando", FluentUI::Icons::Save, [this] { saveScene(); });
        add("Guardar escena como…", "Comando", FluentUI::Icons::Save, [this] { openSaveDialog(); });
        add("Nueva escena", "Comando", FluentUI::Icons::New, [this] { requestGuarded(GuardedAction::NewScene); });
        add("Abrir escena…", "Comando", FluentUI::Icons::Open, [this] { requestGuarded(GuardedAction::OpenScene); });
        add("Editor de tiles", "Ventana", FluentUI::Icons::LayoutGrid, [this] { m_requestTileEditor = true; });
        add("Nuevo proyecto…", "Proyecto", FluentUI::Icons::New,
            [this] { m_newProjectTemplate = "Empty"; requestGuarded(GuardedAction::NewProject); });
        add("Abrir proyecto…", "Proyecto", FluentUI::Icons::Open, [this] { requestGuarded(GuardedAction::OpenProject); });
    }
    if (m_runMode != RunMode::Play)
        add("Jugar", "Transporte", FluentUI::Icons::Play,
            [this] { m_runModeRequest = RunMode::Play; m_runModeRequested = true; });
    if (m_runMode == RunMode::Play)
        add("Pausar", "Transporte", FluentUI::Icons::Pause,
            [this] { m_runModeRequest = RunMode::Paused; m_runModeRequested = true; });
    if (m_runMode != RunMode::Edit)
        add("Detener", "Transporte", FluentUI::Icons::Stop,
            [this] { m_runModeRequest = RunMode::Edit; m_runModeRequested = true; });

    add(m_showHierarchy ? "Plegar escena" : "Desplegar escena", "Ctrl+1",
        FluentUI::Icons::ListTree, [this] { m_showHierarchy = !m_showHierarchy; });
    add(m_showInspector ? "Plegar inspector" : "Desplegar inspector", "Ctrl+2",
        FluentUI::Icons::Settings, [this] { m_showInspector = !m_showInspector; });
    add(m_showConsole ? "Plegar consola" : "Desplegar consola", "Ctrl+3",
        FluentUI::Icons::Terminal, [this] { m_showConsole = !m_showConsole; });

    // Acciones sobre la selección: solo tienen sentido con una entidad viva seleccionada.
    Scene* sc = m_sceneMgr ? &m_sceneMgr->current() : nullptr;
    const bool hasSel = m_selection && sc && m_selection->has() && sc->alive(m_selection->entity);
    if (hasSel && editing) {
        const Entity sel = m_selection->entity;
        add("Asignar script…", "Selección", FluentUI::Icons::Code, [this, sel] { openScriptDialog(sel); });
        add("Nuevo script…", "Selección", FluentUI::Icons::FilePlus, [this, sel] { openNewScriptDialog(sel); });
        add("Cambiar textura…", "Selección", FluentUI::Icons::FileImage, [this, sel] { openTextureDialog(sel); });
    }

    // Entidades de la escena: la paleta también es el buscador de la jerarquía.
    if (sc && m_selection) {
        for (Entity e : sc->allEntities()) {
            if (out.size() >= kMaxItems) break;
            const std::string name = sc->has<NameComponent>(e)
                ? sc->get<NameComponent>(e).value
                : ("Entidad " + std::to_string(e.id));
            add(name, "Entidad", FluentUI::Icons::Box,
                [this, e] { if (m_selection) m_selection->entity = e; m_showInspector = true; });
        }
    }

    // Assets del proyecto: un .lua se adjunta a la selección; una imagen pasa a ser su
    // textura. Sin selección no se ofrecen (no habría a qué aplicarlos).
    if (hasSel && editing && m_assetsScanned) {
        const Entity sel = m_selection->entity;
        std::vector<std::string> hits;
        collectAssetMatches(m_assetBrowser.root(), q, hits, static_cast<int>(kMaxItems));
        for (const std::string& path : hits) {
            if (out.size() >= kMaxItems) break;
            const std::string ext = path.size() > 4 ? path.substr(path.find_last_of('.') + 1) : "";
            if (ext == "lua")
                add(baseName(path), "Script · " + path, FluentUI::Icons::Code,
                    [this, sel, path] { assignScript(sel, path); });
            else if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp")
                add(baseName(path), "Textura · " + path, FluentUI::Icons::FileImage,
                    [this, sel, path] { assignSprite(sel, path); });
        }
    }
}

// Paleta de comandos: campo de búsqueda + lista de resultados, centrada arriba. Es el
// sustituto de la barra de herramientas en este layout — se abre con Ctrl+K, se navega con
// ↑↓ y se ejecuta con Enter (o clic).
void EditorUI::buildCommandPalette(float fw, float top) {
    using FluentUI::Vec2;
    auto* c = FluentUI::GetContext();
    if (!c || !m_paletteOpen) return;

    std::vector<PaletteItem> items;
    collectPaletteItems(m_paletteQuery, items);

    const float dpi    = c->dpiScale;
    const float w      = 560.0f * dpi;
    const float fieldH = 48.0f * dpi;
    const float rowH   = 30.0f * dpi;
    const float listH  = items.empty() ? 0.0f : rowH * static_cast<float>(items.size()) + 8.0f * dpi;
    const float h      = fieldH + listH;
    const Vec2  pos(fw * 0.5f - w * 0.5f, top + 56.0f * dpi);
    const float radius = c->style.panel.cornerRadius * dpi;
    pushUiRect(FluentUI::Rect(pos, Vec2(w, h)));

    FluentUI::Color bg = c->style.panel.background; bg.a = 0.97f;
    c->renderer.DrawRectShadow(pos, Vec2(w, h), radius, 28.0f * dpi,
                               FluentUI::Color(0.0f, 0.0f, 0.0f, 0.55f), Vec2(0.0f, 10.0f * dpi));
    c->renderer.DrawRectFilled(pos, Vec2(w, h), bg, radius);
    c->renderer.DrawRect(pos, Vec2(w, h), c->style.panel.borderColor, radius);
    if (listH > 0.0f)
        c->renderer.DrawRectFilled(Vec2(pos.x + 1.0f, pos.y + fieldH), Vec2(w - 2.0f, 1.0f),
                                   c->style.separator.color, 0.0f);

    // Campo de búsqueda (label "##…" = sin etiqueta visible). Al abrir se le da el foco a
    // mano: la paleta se abre por teclado, así que exigir un clic la haría inútil.
    const uint32_t fieldId = FluentUI::GenerateId("TXT:", "##palette");
    if (m_paletteFocus) {
        c->activeWidgetId   = fieldId;
        c->activeWidgetType = FluentUI::ActiveWidgetType::TextInput;
        c->focusedWidgetId  = fieldId;
        m_paletteFocus = false;
    }
    FluentUI::TextInput("##palette", &m_paletteQuery, w - 24.0f * dpi, false,
                        Vec2(pos.x + 12.0f * dpi, pos.y + 10.0f * dpi),
                        "Buscar comando, entidad o asset…");

    if (items.empty()) {
        m_paletteSel = 0;
        return;
    }
    // Navegación con teclado sobre la lista (el campo se queda con el texto).
    const int n = static_cast<int>(items.size());
    if (c->input.IsKeyPressed(FluentUI::UIKey::Down)) m_paletteSel = (m_paletteSel + 1) % n;
    if (c->input.IsKeyPressed(FluentUI::UIKey::Up))   m_paletteSel = (m_paletteSel + n - 1) % n;
    m_paletteSel = std::clamp(m_paletteSel, 0, n - 1);

    int  activate = -1;
    if (c->input.IsKeyPressed(FluentUI::UIKey::Enter) ||
        c->input.IsKeyPressed(FluentUI::UIKey::KeypadEnter)) activate = m_paletteSel;

    const float mx = c->input.MouseX(), my = c->input.MouseY();
    for (int i = 0; i < n; ++i) {
        const Vec2 rp(pos.x + 4.0f * dpi, pos.y + fieldH + 4.0f * dpi + rowH * static_cast<float>(i));
        const Vec2 rs(w - 8.0f * dpi, rowH);
        const bool hover = mx >= rp.x && mx <= rp.x + rs.x && my >= rp.y && my <= rp.y + rs.y;
        if (hover) m_paletteSel = i;
        if (hover && c->input.IsMousePressed(0)) activate = i;
        if (i == m_paletteSel) {
            FluentUI::Color sel = c->style.accentColor; sel.a = 0.18f;
            c->renderer.DrawRectFilled(rp, rs, sel, 3.0f * dpi);
            c->renderer.DrawRectFilled(rp, Vec2(2.0f * dpi, rs.y), c->style.accentColor, 0.0f);
        }
        c->cursorPos = Vec2(rp.x + 10.0f * dpi, rp.y + (rowH - 18.0f * dpi) * 0.5f);
        FluentUI::BeginHorizontal(8.0f, Vec2(rs.x - 20.0f * dpi, 18.0f * dpi), Vec2(0.0f, 0.0f));
        FluentUI::IconLabel(items[static_cast<size_t>(i)].icon, 14.0f);
        FluentUI::Label(items[static_cast<size_t>(i)].label);
        FluentUI::EndHorizontal();
        // La pista va alineada a la derecha de la fila (categoría, ruta o atajo).
        const std::string& hint = items[static_cast<size_t>(i)].hint;
        const FluentUI::Vec2 hs = c->renderer.MeasureText(hint, c->style.typography.caption.fontSize * dpi);
        c->renderer.DrawText(Vec2(rp.x + rs.x - 10.0f * dpi - hs.x, rp.y + (rowH - hs.y) * 0.5f),
                             hint, c->style.typography.caption.color,
                             c->style.typography.caption.fontSize * dpi);
    }

    if (activate >= 0 && activate < n) {
        auto run = items[static_cast<size_t>(activate)].run;   // copia: ejecutar puede tocar el HUD
        m_paletteOpen = false;
        m_paletteQuery.clear();
        m_paletteSel  = 0;
        if (run) run();
    }
}

void EditorUI::render(VkCommandBuffer cmd, VkImageView swapchainView, VkExtent2D extent) {
    if (!m_initialized) return;
    FluentUI::SetCurrentContext(static_cast<FluentUI::UIContext*>(m_uictx));

    // Invariante del modelo overlay: la UI se maquetó para EXACTAMENTE este attachment.
    // Si no, hay píxeles que ni la escena ni la UI reescriben este frame y, con
    // loadOp=LOAD, muestran contenido viejo (franjas de juego en los bordes). Se avisa
    // una vez en lugar de dejarlo como un artefacto visual sin causa aparente.
    if (!m_extentMismatchWarned && m_buildExtent.width != 0 && m_buildExtent.height != 0 &&
        (m_buildExtent.width != extent.width || m_buildExtent.height != extent.height)) {
        m_extentMismatchWarned = true;
        int lw = 0, lh = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(m_window, &lw, &lh);
        SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
        LOG_WARN("EditorUI: la UI se maquetó a %ux%u pero el attachment es %ux%u "
                 "(ventana logica=%dx%d pixels=%dx%d). Los bordes no cubiertos dejaran "
                 "ver la escena. Si logica != pixels, ademas el raton (que SDL entrega en "
                 "coordenadas logicas) quedara desalineado respecto al layout.",
                 m_buildExtent.width, m_buildExtent.height, extent.width, extent.height,
                 lw, lh, pw, ph);
    }


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

    // GetBackend() ya no es función libre: el backend cuelga del renderer del contexto.
    if (auto* c = FluentUI::GetContext())
        if (FluentUI::RenderBackend* be = c->renderer.GetBackend())
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

// Transición pedida con los botones de transporte. El Engine la aplica (y solo él sabe si
// pudo) y confirma el estado con setRunMode.
bool EditorUI::consumeRunModeRequest(RunMode& out) {
    if (!m_runModeRequested) return false;
    m_runModeRequested = false;
    out = m_runModeRequest;
    return true;
}

void EditorUI::applySceneOp(SceneOp op, const std::string& path) {
    // Punto ÚNICO por el que pasan las ops de escena (menú, toolbar, diálogo de cambios sin
    // guardar). Jugando, la escena en memoria es una partida que se descartará al parar:
    // guardarla escribiría ese estado y cargar otra la perdería sin remedio. Los comandos
    // ya salen deshabilitados, pero la guarda vive aquí para cubrir cualquier vía.
    if (m_runMode != RunMode::Edit) {
        LOG_WARN("Operación de escena ignorada: para el juego (Stop) antes de crear, abrir o guardar.");
        FluentUI::ShowToast("Detén el juego primero",
                            "Las escenas solo se editan con la simulación parada.",
                            { FluentUI::InfoSeverity::Warning, 5.0f });
        return;
    }

    // Cargar/nueva escena cambia la escena activa: hay que olvidar las instancias de
    // script (estado viejo) y la selección (la entidad ya no vive). La ruta actual la
    // recuerda el SceneManager (currentPath), fuente única para "Guardar".
    switch (op) {
        case SceneOp::New:
            m_sceneMgr->newScene();   // limpia currentPath
            if (m_scriptSys) m_scriptSys->clear();
            if (m_selection) m_selection->clear();
            m_sceneDirty = false;
            LOG_INFO("Escena nueva.");
            FluentUI::ShowToast("Escena nueva", "", { FluentUI::InfoSeverity::Success });
            break;
        case SceneOp::Open:
            if (m_sceneMgr->load(path)) {   // fija currentPath
                if (m_scriptSys) m_scriptSys->clear();
                if (m_selection) m_selection->clear();
                m_sceneDirty = false;
                LOG_INFO("Escena abierta: %s", path.c_str());
                FluentUI::ShowToast("Escena abierta", path, { FluentUI::InfoSeverity::Success });
            } else {
                LOG_WARN("No se pudo abrir la escena: %s", path.c_str());
                FluentUI::ShowToast("No se pudo abrir la escena", path, { FluentUI::InfoSeverity::Error, 7.0f });
            }
            break;
        case SceneOp::Save:
            // Primera vez (escena sin ruta): se comporta como "Guardar como" y pide
            // dónde. Ya con ruta, guarda ahí directamente, sin preguntar.
            if (m_sceneMgr->currentPath().empty()) { openSaveDialog(); break; }
            if (m_sceneMgr->save(m_sceneMgr->currentPath())) {
                m_sceneDirty = false;
                LOG_INFO("Escena guardada: %s", m_sceneMgr->currentPath().c_str());
                FluentUI::ShowToast("Escena guardada", m_sceneMgr->currentPath(), { FluentUI::InfoSeverity::Success });
            } else {
                LOG_WARN("No se pudo guardar la escena.");
                FluentUI::ShowToast("No se pudo guardar la escena", "", { FluentUI::InfoSeverity::Error, 7.0f });
            }
            break;
        case SceneOp::SaveAs: {
            const bool ok = m_sceneMgr->save(path);   // fija currentPath
            if (ok) {
                m_sceneDirty = false;
                LOG_INFO("Escena guardada como: %s", path.c_str());
                FluentUI::ShowToast("Escena guardada", path, { FluentUI::InfoSeverity::Success });
            } else {
                LOG_WARN("No se pudo guardar la escena: %s", path.c_str());
                FluentUI::ShowToast("No se pudo guardar la escena", path, { FluentUI::InfoSeverity::Error, 7.0f });
            }
            // "Guardar y continuar" con escena sin ruta: la acción esperaba a este guardado.
            GuardedAction after = GuardedAction::None;
            { std::lock_guard<std::mutex> lk(m_pendingMutex);
              after = m_afterSaveAction; m_afterSaveAction = GuardedAction::None; }
            if (ok && after != GuardedAction::None) runGuarded(after);
            break;
        }
        case SceneOp::NewProject:
            // Crea el .pkproj + esqueleto de carpetas, vuelca la plantilla elegida y fija la
            // raíz. Si la plantilla trae escena (el mundo de ejemplo), se carga; si no, el
            // proyecto arranca VACÍO: se puebla desde el menú Entidad.
            if (Project::instance().newProject(path, "", m_newProjectTemplate)) {
                const std::string& start = Project::instance().startScene();
                if (start.empty() || !m_sceneMgr->load(Project::instance().resolveRead(start)))
                    m_sceneMgr->newScene();
                refreshAfterProjectChange();
                LOG_INFO("Proyecto nuevo: %s", path.c_str());
                FluentUI::ShowToast("Proyecto creado", path, { FluentUI::InfoSeverity::Success });
            } else {
                LOG_WARN("No se pudo crear el proyecto: %s", path.c_str());
                FluentUI::ShowToast("No se pudo crear el proyecto", path, { FluentUI::InfoSeverity::Error, 7.0f });
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
                FluentUI::ShowToast("Proyecto abierto", path, { FluentUI::InfoSeverity::Success });
            } else {
                LOG_WARN("No se pudo abrir el proyecto: %s", path.c_str());
                FluentUI::ShowToast("No se pudo abrir el proyecto", path, { FluentUI::InfoSeverity::Error, 7.0f });
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
    m_sceneDirty = false;   // la escena del proyecto anterior ya no está en memoria
    TileSet::instance().load("Assets/Data/tileset.json");   // registro de tipos del nuevo proyecto (o fallback)
    if (m_eventBus) m_eventBus->emit(MapSavedEvent{});       // overworld: recarga tileset + reconstruye sprites
    m_assetsScanned = false;                                 // el grid de assets se re-escanea al siguiente frame
    m_selectedAsset.clear();                                 // la tarjeta marcada era del proyecto anterior
}

// Acción que descarta la escena en memoria: si no hay cambios pendientes se ejecuta ya;
// si los hay, se aparca y decide el usuario en el diálogo.
void EditorUI::requestGuarded(GuardedAction a) {
    if (a == GuardedAction::None) return;
    if (!m_sceneDirty) { runGuarded(a); return; }
    m_dirtyPending = a;
    m_dirtyDlgOpen = true;
}

// Ejecuta la acción sin preguntar. Las que mutan la escena solo ENCOLAN la op (se aplica
// al principio del siguiente frame), nunca a mitad de construcción de la UI.
void EditorUI::runGuarded(GuardedAction a) {
    switch (a) {
        case GuardedAction::NewScene:    newScene();          break;
        case GuardedAction::OpenScene:   openSceneDialog();   break;
        case GuardedAction::NewProject:  newProjectDialog();  break;
        case GuardedAction::OpenProject: openProjectDialog(); break;
        case GuardedAction::Quit:        m_requestQuit = true; break;
        case GuardedAction::None:        break;
    }
}

// Diálogo estándar "cambios sin guardar" (Guardar y continuar / Descartar / Cancelar).
void EditorUI::buildDirtyDialog() {
    const std::string scene = (m_sceneMgr && !m_sceneMgr->currentPath().empty())
                                  ? baseName(m_sceneMgr->currentPath())
                                  : std::string("escena sin guardar");

    const FluentUI::DialogResult r = FluentUI::ContentDialog(
        "dlg_dirty", &m_dirtyDlgOpen, "Cambios sin guardar",
        [&] {
            FluentUI::Label("\"" + scene + "\" tiene cambios sin guardar.");
            FluentUI::Label("Si continúas sin guardar, se perderán.", std::nullopt,
                            FluentUI::TypographyStyle::Caption);
        },
        "Guardar y continuar", "Descartar", "Cancelar");

    if (r == FluentUI::DialogResult::None) return;   // sigue abierto (o cerrado del todo)

    const GuardedAction act = m_dirtyPending;
    m_dirtyPending = GuardedAction::None;

    if (r == FluentUI::DialogResult::Close) return;              // Cancelar: no se hace nada
    if (r == FluentUI::DialogResult::Secondary) {                // Descartar
        m_sceneDirty = false;
        runGuarded(act);
        return;
    }

    // Guardar y continuar. Guardar NO muta la escena, así que puede hacerse en el mismo
    // frame; si aún no hay ruta hay que preguntarla y la acción espera al SaveAs.
    if (m_sceneMgr && m_sceneMgr->currentPath().empty()) {
        // El lock se suelta ANTES de abrir el diálogo: su callback lo vuelve a tomar y,
        // si el diálogo del SO resuelve en este mismo hilo, un mutex no recursivo
        // bloquearía el editor.
        { std::lock_guard<std::mutex> lk(m_pendingMutex); m_afterSaveAction = act; }
        openSaveDialog();
        return;
    }
    if (m_sceneMgr && m_sceneMgr->save(m_sceneMgr->currentPath())) {
        m_sceneDirty = false;
        LOG_INFO("Escena guardada: %s", m_sceneMgr->currentPath().c_str());
        FluentUI::ShowToast("Escena guardada", m_sceneMgr->currentPath(),
                            { FluentUI::InfoSeverity::Success });
        runGuarded(act);
    } else {
        LOG_WARN("No se pudo guardar la escena.");
        FluentUI::ShowToast("No se pudo guardar la escena", "",
                            { FluentUI::InfoSeverity::Error, 7.0f });
    }
}

void EditorUI::openSaveDialog() {
    FluentUI::ShowSaveFileDialog(
        m_window,
        std::vector<FluentUI::FileFilter>{ { "Escena PokeMotor", "json" } },
        m_sceneMgr->currentPath().empty() ? Project::instance().resolveWrite("Assets/Data/scene.json")
                                          : m_sceneMgr->currentPath(),
        [this](const std::vector<std::string>& paths, int) {
            std::lock_guard<std::mutex> lk(m_pendingMutex);  // callback puede ser de otro hilo
            if (paths.empty()) {                             // el usuario canceló
                m_afterSaveAction = GuardedAction::None;     // se cancela también lo diferido
                return;
            }
            m_pendingSceneOp = SceneOp::SaveAs; m_pendingScenePath = paths[0];
        });
}

// Asignar/cambiar el script de una entidad: diálogo nativo (filtro .lua) que arranca en
// los scripts del proyecto (o los del motor vía fallback de resolveRead). La elección
// queda PENDIENTE y beginFrame la aplica en el hilo principal (add/reasignación de
// ScriptComponent + import al proyecto); el ScriptSystem instancia al frame siguiente.
void EditorUI::openScriptDialog(Entity target) {
    FluentUI::ShowOpenFileDialog(
        m_window,
        std::vector<FluentUI::FileFilter>{ { "Script Lua", "lua" } },
        Project::instance().resolveRead("Assets/Scripts"), false,
        [this, target](const std::vector<std::string>& paths, int) {
            if (paths.empty()) return;                       // canceló
            std::lock_guard<std::mutex> lk(m_pendingMutex);  // callback puede ser de otro hilo
            m_pendingScriptOp     = ScriptOp::Assign;
            m_pendingScriptEntity = target;
            m_pendingScriptPath   = paths[0];
        });
}

// Crear un .lua nuevo: el diálogo elige dónde (por defecto los scripts DEL PROYECTO) y
// beginFrame escribe la plantilla. Con `target` válida el script recién creado se adjunta
// a esa entidad; con una entidad inválida (botón de la pestaña Assets) solo se crea.
void EditorUI::openNewScriptDialog(Entity target) {
    // El diálogo nativo de guardar toma el nombre por defecto; se le antepone la carpeta
    // de scripts del proyecto para que aparezca ya situado ahí (resolveWrite la crea).
    const std::string suggested =
        (std::filesystem::path(Project::instance().resolveWrite("Assets/Scripts")) /
         "nuevo_script.lua").string();
    FluentUI::ShowSaveFileDialog(
        m_window,
        std::vector<FluentUI::FileFilter>{ { "Script Lua", "lua" } },
        suggested,
        [this, target](const std::vector<std::string>& paths, int) {
            if (paths.empty()) return;                       // canceló
            std::string p = paths[0];
            // El diálogo puede devolver el nombre sin extensión si el usuario la borra.
            if (std::filesystem::path(p).extension() != ".lua") p += ".lua";
            std::lock_guard<std::mutex> lk(m_pendingMutex);
            m_pendingScriptOp     = ScriptOp::Create;
            m_pendingScriptEntity = target;
            m_pendingScriptPath   = p;
        });
}

// Adjunta el .lua a la entidad. Reasignar solo cambia la ruta: el ScriptSystem ve el path
// distinto y re-instancia (nuevo entorno + on_start) por su cuenta al frame siguiente.
void EditorUI::assignScript(Entity e, const std::string& path) {
    if (!m_sceneMgr || path.empty()) return;
    Scene& sc = m_sceneMgr->current();
    if (!sc.alive(e)) return;
    if (sc.has<ScriptComponent>(e)) sc.get<ScriptComponent>(e).path = path;
    else                            sc.add<ScriptComponent>(e, ScriptComponent{ path });
    m_sceneDirty = true;
    LOG_INFO("Script '%s' adjuntado a la entidad %u.", path.c_str(), e.id);
}

// Elegir la imagen de un sprite: diálogo nativo que arranca en las texturas del proyecto.
// La elección queda PENDIENTE (el callback puede venir de otro hilo) y beginFrame la
// importa + aplica. Sirve tanto para cambiar la textura como para dar de alta el
// SpriteComponent desde "Agregar componente".
void EditorUI::openTextureDialog(Entity target) {
    FluentUI::ShowOpenFileDialog(
        m_window,
        std::vector<FluentUI::FileFilter>{ { "Imagen", "png;jpg;jpeg;bmp;tga" }, { "Todos", "*" } },
        Project::instance().resolveRead("Assets/Textures"), false,
        [this, target](const std::vector<std::string>& paths, int) {
            if (paths.empty()) return;                       // canceló
            std::lock_guard<std::mutex> lk(m_pendingMutex);
            m_pendingTextureEntity = target;
            m_pendingTexturePath   = paths[0];
        });
}

// Pone `path` como textura del sprite de la entidad (creando el SpriteComponent si no lo
// tenía). Guarda la RUTA además del handle: es lo que persiste en la escena y lo que el
// SceneManager vuelve a resolver al cargar.
void EditorUI::assignSprite(Entity e, const std::string& path) {
    if (!m_sceneMgr || !m_assets || path.empty()) return;
    Scene& sc = m_sceneMgr->current();
    if (!sc.alive(e)) return;
    if (!sc.has<SpriteComponent>(e)) sc.add<SpriteComponent>(e, SpriteComponent{});
    SpriteComponent& sp = sc.get<SpriteComponent>(e);
    sp.texturePath = path;
    sp.tex         = m_assets->loadTexture(path, true, true, sp.filter);   // respeta Pixel/Suave
    m_sceneDirty = true;
    LOG_INFO("Textura '%s' asignada a la entidad %u.", path.c_str(), e.id);
}

// --- Menú Entidad ---
// El editor no crea entidades a mano: publica la INTENCIÓN y el modo activo la materializa
// (él sabe dónde está mirando la vista, qué scripts lleva un jugador y qué mapa por defecto
// sembrar). El modo se encarga también de seleccionarla y de marcar la escena como sucia.
void EditorUI::requestNewEntity(NewEntityKind kind, const std::string& path) {
    if (!m_eventBus) { LOG_WARN("Sin bus de eventos: no se pudo crear la entidad."); return; }
    m_eventBus->emit(CreateEntityEvent{ kind, path });
}

// "Crear sprite…": elige la imagen antes de crear nada. La elección queda PENDIENTE (el
// callback puede venir de otro hilo) y beginFrame la importa al proyecto y la publica.
void EditorUI::openNewSpriteDialog() {
    FluentUI::ShowOpenFileDialog(
        m_window,
        std::vector<FluentUI::FileFilter>{ { "Imagen", "png;jpg;jpeg;bmp;tga" }, { "Todos", "*" } },
        Project::instance().resolveRead("Assets/Textures"), false,
        [this](const std::vector<std::string>& paths, int) {
            if (paths.empty()) return;                       // canceló
            std::lock_guard<std::mutex> lk(m_pendingMutex);
            m_pendingNewSpritePath = paths[0];
        });
}

// Elimina la entidad seleccionada. Es el editor quien lo hace (no el modo): tiene la escena
// y la selección, y no hay nada que decidir sobre el mundo. El modo detecta por su cuenta
// que su mapa/jugador dejó de existir (compara punteros/alive cada frame).
void EditorUI::deleteSelectedEntity() {
    if (!m_sceneMgr || !m_selection || !m_selection->has()) return;
    if (m_runMode != RunMode::Edit) return;   // jugando, la escena es una partida en curso
    Scene& sc = m_sceneMgr->current();
    const Entity e = m_selection->entity;
    if (!sc.alive(e)) { m_selection->clear(); return; }
    const std::string name = sc.has<NameComponent>(e) ? sc.get<NameComponent>(e).value
                                                      : ("Entidad " + std::to_string(e.id));
    sc.destroyEntity(e);
    m_selection->clear();
    m_sceneDirty = true;
    LOG_INFO("Entidad '%s' eliminada.", name.c_str());
}

void EditorUI::newScene() {
    std::lock_guard<std::mutex> lk(m_pendingMutex);
    m_pendingSceneOp = SceneOp::New;
}

void EditorUI::saveScene() {
    std::lock_guard<std::mutex> lk(m_pendingMutex);
    m_pendingSceneOp = SceneOp::Save;   // applySceneOp pide ruta si aún no la hay
}

void EditorUI::openSceneDialog() {
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

// Un PROYECTO es una carpeta con un .pkproj en la raíz; todo se resuelve relativo a ella
// (ver Core/Project). Nuevo/Abrir proyecto cambian la raíz activa.
void EditorUI::newProjectDialog() {
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

void EditorUI::openProjectDialog() {
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
// (miniatura para imágenes — carga perezosa —, icono para el resto) usando el GridView de
// la librería: reflow por ancho, virtualización, scroll y selección propios.
// Marca la tarjeta recién dibujada como origen del arrastre si el ratón se pulsó sobre
// ella. El DragDropSource de FluentUI no distingue por sí solo qué widget armó el drag:
// al cruzar el umbral, cualquier source vivo se adueña del payloadType/payload. Con este
// dato, solo la tarjeta pulsada crea su source y el drag conserva el tipo correcto.
void EditorUI::markDragSource(const std::string& path) {
    auto* c = FluentUI::GetContext();
    if (!c || !c->input.IsMousePressed(0)) return;
    const FluentUI::Vec2 ip = c->lastItemPos, is = c->lastItemSize;
    const float mx = c->input.MouseX(), my = c->input.MouseY();
    if (mx >= ip.x && mx <= ip.x + is.x && my >= ip.y && my <= ip.y + is.y)
        m_dragSourcePath = path;
}

void EditorUI::drawAssetCards(const AssetNode& folder, float gridW) {
    constexpr float kCardW = 88.0f, kCardH = 108.0f, kThumb = 60.0f, kPad = 8.0f;
    (void)gridW;   // el GridView calcula las columnas por el ancho disponible del contenedor

    // Cambiar de carpeta descarta la tarjeta marcada (la selección es de la vista actual).
    if (folder.path != m_assetFolderShown) { m_assetFolderShown = folder.path; m_selectedAsset.clear(); }

    // GridView: mosaico que reflowa por ancho y VIRTUALIZA (solo construye las filas
    // visibles), con scroll/scrollbar y selección propios. Cada celda la dibuja
    // itemBuilder(index): reusamos el cuerpo de tarjeta de antes (miniatura/icono + label +
    // drag-source). GridView fija el ancho de celda (Fixed) y coloca el cursor por celda,
    // así que los widgets internos siguen recibiendo input (entrar carpeta, arrastrar al
    // viewport); su resaltado de selección se suma sin robar el click.
    // Id por carpeta: el GridView guarda selección y scroll bajo su id, así que con un id
    // fijo la celda resaltada (y el desplazamiento) se arrastraban de una carpeta a otra.
    // Con la ruta dentro, cada carpeta arranca limpia y recuerda su propio scroll al volver.
    const int n = static_cast<int>(folder.children.size());
    FluentUI::GridView("assetCards:" + folder.path, n, FluentUI::Vec2(kCardW, kCardH),
                       [this, &folder](int index) {
        const AssetNode& child = folder.children[static_cast<size_t>(index)];

        // Miniatura de la textura (carga perezosa, cacheada por ruta y envuelta para FluentUI).
        void* th = nullptr;
        if (child.kind == AssetKind::Image && m_assets) {
            const TextureHandle h = m_assets->loadTexture(child.path);
            if (Texture* t = m_assets->getTexture(h)) th = thumbnailFor(t->view());
        }

        // El cuerpo de la tarjeta: miniatura (o icono) centrada + nombre. Va dentro del
        // scope de la Card, que ya abre su propio layout vertical con padding.
        auto body = [&] {
            auto* cc = FluentUI::GetContext();
            const float innerW = kCardW - kPad * 2.0f;
            if (cc) cc->cursorPos.x += (innerW - kThumb) * 0.5f;   // centra el icono en la tarjeta
            if (th) FluentUI::Image("ic_" + child.path, th, FluentUI::Vec2(kThumb, kThumb));
            else    FluentUI::IconLabel(child.isFolder ? FluentUI::Icons::Folder
                                                       : iconForKind(child.kind), kThumb);
            FluentUI::Label(ellipsize(child.name, 9));
        };

        // Tarjeta WinUI en vez de un apilado suelto: da hover, pressed, foco y —para los
        // assets— marca de selección (borde de acento), y publica su rect como último ítem,
        // que es de donde el DragDropSource toma la zona arrastrable (así se arrastra desde
        // TODA la tarjeta, no solo desde los 64 px de la miniatura).
        FluentUI::CardConfig cfg;
        cfg.style      = FluentUI::CardStyle::Filled;
        cfg.clickable  = true;
        cfg.selectable = !child.isFolder;   // una carpeta no se marca: se entra en ella
        cfg.size       = FluentUI::Vec2(kCardW, kCardH);
        cfg.padding    = kPad;
        cfg.radius     = 6.0f;

        if (child.isFolder) {
            // Click → entrar (además del árbol de la izquierda). La fachada Card devuelve
            // la activación de ESTE frame; el cambio de carpeta se aplica al siguiente.
            if (FluentUI::Card("asset_" + child.path, cfg, body))
                m_selectedFolder = child.path;
            return;
        }

        // Selección ÚNICA: la Card conmuta el bool que le pasamos; el editor guarda la ruta
        // marcada, así que basta con derivarlo de ella cada frame (marcar otra desmarca esta).
        bool sel = (child.path == m_selectedAsset);
        const bool wasSel = sel;
        FluentUI::BeginCard("asset_" + child.path, cfg, &sel);
        body();
        FluentUI::EndCard();
        if (sel != wasSel) m_selectedAsset = sel ? child.path : std::string{};

        // Arrastre: solo texturas (crean entidad) y scripts (se adjuntan a una existente).
        const char* payload = child.kind == AssetKind::Image  ? "ASSET_TEXTURE"
                            : child.kind == AssetKind::Script ? "ASSET_SCRIPT"
                                                              : nullptr;
        if (!payload) return;
        markDragSource(child.path);
        // SOLO la tarjeta pulsada construye el source. Al cruzar el umbral de arrastre,
        // CUALQUIER DragDropSource vivo se apropia del payloadType (ver markDragSource):
        // con texturas y scripts en la misma carpeta, una tarjeta posterior convertiría
        // un arrastre de sprite en uno de script.
        if (child.path != m_dragSourcePath) return;
        FluentUI::DragDropSource src(payload);
        if (!src.IsActive()) return;
        src.SetPayload(child.path);
        // Preview: lo arrastrado sigue al cursor (la miniatura, o icono + nombre).
        if (th) {
            src.DragPreview([th]() {
                FluentUI::Image("drag_preview", th, FluentUI::Vec2(48.0f, 48.0f));
            });
        } else {
            const std::string preview = ellipsize(child.name, 14);
            src.DragPreview([preview]() {
                FluentUI::IconLabel(FluentUI::Icons::FileCode, 24.0f);
                FluentUI::Label(preview);
            });
        }
    }, 6.0f);
}

// Chrome de ventana propio (WinUI): dibuja la TitleBar arriba del todo y maneja su resultado
// (cerrar → quitRequest del motor; min/max los realiza el platform). Con withCommands=true el
// contenido lo compone buildTitleBarContent(); en la bienvenida (false) va el modo por defecto
// (icono + "PokeMotor"). Deja el cursor bajo la barra para lo que siga (el toolbar).
void EditorUI::drawTitleBar(bool withCommands) {
    auto* c = FluentUI::GetContext();
    if (!c) return;
    c->cursorPos = FluentUI::Vec2(0.0f, 0.0f);   // arriba a la izquierda, ancho completo
    FluentUI::TitleBarConfig cfg;
    cfg.height = 40.0f;
    FluentUI::TitleBarResult r = FluentUI::TitleBar(
        "editorTitleBar", "PokeMotor", FluentUI::Icons::Gamepad,
        withCommands ? std::function<void()>([this] { buildTitleBarContent(); })
                     : std::function<void()>(),
        cfg);
    // Cierre limpio del motor; con cambios sin guardar pregunta antes (en la bienvenida
    // no hay escena que proteger, así que sale directo).
    if (r.closePressed) {
        if (withCommands) requestGuarded(GuardedAction::Quit);
        else              m_requestQuit = true;
    }
    // Alto REAL de la barra (S(cfg.height), escalado por DPI) vía lastItemSize — hardcodear 40
    // solapaba el toolbar a dpiScale>1 (patrón del demo App.cpp).
    c->cursorPos = FluentUI::Vec2(0.0f, c->lastItemSize.y);
}

// Contenido componible de la TitleBar (modo content): icono de la app + el MENÚ clásico con el
// widget NATIVO (BeginMenu/MenuItem) → hover-para-cambiar, teclado y submenús. Como BeginMenu no
// publica su bbox a focusableWidgets, su zona NO se auto-excluye del arrastre: la marcamos a mano
// con TitleBarDragExclude (patrón del demo examples/App). Los accesos rápidos (Nuevo/Abrir/Guardar/
// Tiles) viven en la fila del toolbar. Los ítems sin acción real van deshabilitados.
void EditorUI::buildTitleBarContent() {
    auto* c = FluentUI::GetContext();
    FluentUI::IconLabel(FluentUI::Icons::Gamepad, 18.0f);

    const float menuX0 = c->cursorPos.x;   // inicio de la franja de menús (para excluir del arrastre)

    // Jugando, la escena de memoria es una partida en curso que se descartará al parar:
    // todo lo que la guarde o la sustituya se deshabilita hasta volver a edición.
    const bool editing = (m_runMode == RunMode::Edit);
    if (FluentUI::BeginMenu("Archivo")) {
        // Las entradas que descartan la escena pasan por requestGuarded: si hay cambios
        // sin guardar preguntan antes en vez de perderlos.
        if (FluentUI::MenuItem("Nuevo", editing))         requestGuarded(GuardedAction::NewScene);
        if (FluentUI::MenuItem("Abrir…", editing))        requestGuarded(GuardedAction::OpenScene);
        if (FluentUI::MenuItem("Guardar", editing))       saveScene();
        if (FluentUI::MenuItem("Guardar como…", editing)) openSaveDialog();
        FluentUI::MenuSeparator();
        if (FluentUI::MenuItem("Salir"))         requestGuarded(GuardedAction::Quit);
        FluentUI::EndMenu();
    }
    if (FluentUI::BeginMenu("Editar")) {
        FluentUI::MenuItem("Deshacer", false);
        FluentUI::MenuItem("Rehacer", false);
        FluentUI::EndMenu();
    }
    // Los paneles del HUD no se cierran: se PLIEGAN a su cabecera (siguen a la vista y a un
    // clic de volver), que es lo que mantiene el juego despejado sin esconder nada.
    if (FluentUI::BeginMenu("Ver")) {
        if (FluentUI::MenuItem(m_showHierarchy ? "Plegar escena"    : "Desplegar escena"))
            m_showHierarchy = !m_showHierarchy;
        if (FluentUI::MenuItem(m_showInspector ? "Plegar inspector" : "Desplegar inspector"))
            m_showInspector = !m_showInspector;
        if (FluentUI::MenuItem(m_showConsole   ? "Plegar consola"   : "Desplegar consola"))
            m_showConsole = !m_showConsole;
        FluentUI::MenuSeparator();
        // Los paneles se arrastran por su cabecera; esto los devuelve a todos a su sitio
        // (equivale al doble clic en la cabecera de cada uno).
        if (FluentUI::MenuItem("Restablecer disposición")) resetOverlayPositions();
        FluentUI::EndMenu();
    }
    if (FluentUI::BeginMenu("Proyecto")) {
        // Una entrada por plantilla (las carpetas de Templates/): el proyecto nace con ese
        // contenido, igual que desde la pantalla de bienvenida.
        for (const std::string& t : Project::instance().templateNames()) {
            if (FluentUI::MenuItem("Nuevo: " + templateLabel(t) + "…", editing)) {
                m_newProjectTemplate = t;
                requestGuarded(GuardedAction::NewProject);
            }
        }
        if (FluentUI::MenuItem("Abrir proyecto…", editing)) requestGuarded(GuardedAction::OpenProject);
        FluentUI::MenuSeparator();
        // Ajustes de render del proyecto. HD-2D: ON = sprites Smooth (+UI/texto) a resolución
        // completa compuestos sobre el lowRes; OFF = todo por el lowRes (look retro puro).
        if (m_renderer) {
            const bool hd = m_renderer->hd2D();
            if (FluentUI::MenuItem(hd ? "Sprites HD-2D: activado"
                                      : "Sprites HD-2D: desactivado"))
                m_renderer->setHD2D(!hd);
        }
        FluentUI::EndMenu();
    }
    // Altas manuales de contenido: son la vía para poblar un proyecto en blanco (el motor ya
    // no siembra nada por su cuenta). Solo en edición: jugando, la escena es una partida.
    if (FluentUI::BeginMenu("Entidad")) {
        if (FluentUI::MenuItem("Crear vacía", editing))    requestNewEntity(NewEntityKind::Empty);
        if (FluentUI::MenuItem("Crear sprite…", editing))  openNewSpriteDialog();
        if (FluentUI::MenuItem("Crear cámara", editing))   requestNewEntity(NewEntityKind::Camera);
        if (FluentUI::MenuItem("Crear mapa de tiles", editing)) requestNewEntity(NewEntityKind::TileMap);
        if (FluentUI::MenuItem("Crear jugador", editing))  requestNewEntity(NewEntityKind::Player);
        FluentUI::MenuSeparator();
        const bool hasSel = m_selection && m_selection->has();
        if (FluentUI::MenuItem("Eliminar", editing && hasSel)) deleteSelectedEntity();
        FluentUI::EndMenu();
    }
    if (FluentUI::BeginMenu("Ventana")) {
        // El editor de tiles escribe en el TileMapComponent de la escena viva: en partida
        // esos cambios se irían con el snapshot al parar.
        if (FluentUI::MenuItem("Editor de tiles", editing)) m_requestTileEditor = true;
        FluentUI::EndMenu();
    }
    if (FluentUI::BeginMenu("Ayuda")) {
        FluentUI::MenuItem("Acerca de PokeMotor", false);
        FluentUI::EndMenu();
    }

    const float menuX1 = c->cursorPos.x;

    // Estado de la escena a la derecha de la barra: con el layout overlay ya no hay barra de
    // estado abajo (ese borde es del juego), así que el modo de ejecución, los FPS y el
    // "sin guardar" viven aquí, en el único chrome fijo que queda.
    FluentUI::TitleBarSpacer();
    const char* runLabel = (m_runMode == RunMode::Play)   ? "JUGANDO"
                         : (m_runMode == RunMode::Paused) ? "PAUSA" : "EDICIÓN";
    char status[160];
    std::snprintf(status, sizeof(status), "%s · %.0f FPS%s", runLabel, m_fps,
                  m_sceneDirty ? " · cambios sin guardar" : "");
    FluentUI::Label(status, std::nullopt, FluentUI::TypographyStyle::Caption);

    // BeginMenu no marca su zona como interactiva, así que hay que excluir a mano la franja de
    // los menús del arrastre de la ventana (si no, clicar un menú arrastraría en vez de abrirlo).
    // La altura 200 se recorta contra el caption; sobra-cubrir es inofensivo (patrón del demo).
    FluentUI::TitleBarDragExclude(
        FluentUI::Rect(FluentUI::Vec2(menuX0, 0.0f), FluentUI::Vec2(menuX1 - menuX0, 200.0f)));
}

void EditorUI::shutdown() {
    if (!m_initialized) return;
    FluentUI::SetCurrentContext(static_cast<FluentUI::UIContext*>(m_uictx));
    // Persistir la visibilidad de los paneles del HUD (por proyecto). Solo con proyecto
    // abierto: sin raíz, resolveWrite no apunta a ninguna parte útil.
    if (Project::instance().isOpen()) {
        std::ofstream f(Project::instance().resolveWrite("editor_layout.ini"));
        if (f) {
            f << m_showHierarchy << ' ' << m_showInspector << ' ' << m_showConsole << ' '
              << m_bottomTab << ' ' << m_bottomMax;
            // Dónde dejó el usuario cada panel (0 0 = en su sitio por defecto).
            for (const char* id : { "ovl_hier", "ovl_insp", "ovl_console" }) {
                auto it = m_overlayState.find(id);
                const FluentUI::Vec2 o = (it != m_overlayState.end()) ? it->second.offset
                                                                     : FluentUI::Vec2(0.0f, 0.0f);
                const FluentUI::Vec2 sz = (it != m_overlayState.end()) ? it->second.sizeDelta
                                                                       : FluentUI::Vec2(0.0f, 0.0f);
                f << ' ' << o.x << ' ' << o.y << ' ' << sz.x << ' ' << sz.y;
            }
            // Disposición: una letra por panel (H/I/C); ',' separa grupos acoplados dentro de
            // un cluster y ';' separa clusters. El cluster empieza por 'h' o 'v' según reparta
            // en columnas o en filas. Ej: "hH,I;C" = escena e inspector lado a lado, consola aparte.
            f << ' ';
            for (size_t ci = 0; ci < m_clusters.size(); ++ci) {
                if (ci) f << ';';
                f << (m_clusters[ci].vertical ? 'v' : 'h');
                for (size_t mi = 0; mi < m_clusters[ci].members.size(); ++mi) {
                    if (mi) f << ',';
                    // '*' marca el grupo que da su sitio al cluster.
                    if (!m_clusters[ci].members[mi].tabs.empty() &&
                        m_clusters[ci].members[mi].tabs[0] == m_clusters[ci].anchor) f << '*';
                    for (PanelId p : m_clusters[ci].members[mi].tabs)
                        f << (p == PanelId::Hierarchy ? 'H' : p == PanelId::Inspector ? 'I' : 'C');
                }
            }
            f << '\n';
        }
    }
    if (m_device) vkDeviceWaitIdle(m_device);
    for (auto& kv : m_thumb)
        if (kv.second) FluentUI::DestroyExternalTexture(kv.second);
    m_thumb.clear();
    FluentUI::DestroyContext();
    // El platform (host, no posee SDL) se libera tras el contexto, que lo referenciaba.
    delete m_platform;
    m_platform = nullptr;
    m_initialized = false;
}

}  // namespace pk
