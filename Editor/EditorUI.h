// Editor/EditorUI.h — UI del motor (FluentUI en modo Vulkan compartido).
// Diseño: MotorGrafico_UIEditor.md. Orquesta los paneles y se dibuja como último
// pass sobre la swapchain. Todo bajo #ifdef ENGINE_EDITOR (fuera de release).
#pragma once

#include "Editor/AssetBrowser.h"
#include "Core/ECS/Entity.h"   // entidad destino de la asignación de script (diálogo diferido)
#include "Core/EventBus.h"     // Subscription (escucha de las ediciones del viewport)
#include "Game/GameEvents.h"   // NewEntityKind (peticiones del menú Entidad)
#include "Game/RunMode.h"      // Play/Pausa/Stop de la toolbar
#include "Math/Rect.h"         // FluentUI::Rect (rect del viewport para el hit-test de drop)

#include <vulkan/vulkan.h>

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace FluentUI { class PlatformBackend; }   // puerto de plataforma (TitleBar: hit-test + min/max/cerrar)

namespace pk {

class VulkanContext;
class Renderer;
class AssetManager;
class EventBus;
class SceneManager;
class ScriptSystem;
struct Selection;

class EditorUI {
public:
    bool init(VulkanContext& ctx, SDL_Window* window, Renderer* renderer, AssetManager* assets);
    void shutdown();

    void setSelection(Selection* s) { m_selection = s; }   // entidad activa del editor
    void setEventBus(EventBus* bus);   // emite AssetDropped… y escucha SceneEdited (gizmo)
    void setSceneManager(SceneManager* sm) { m_sceneMgr = sm; }  // jerarquía/inspector sobre la escena
    void setScriptSystem(ScriptSystem* ss) { m_scriptSys = ss; }  // exports del script en el inspector

    void beginInputFrame();                  // limpia flancos (1x por frame, ANTES de los eventos)
    void processEvent(const SDL_Event& e);   // alimenta el input de FluentUI
    // NewFrame + construye los paneles. `fbExtent` DEBE ser el extent del attachment
    // sobre el que se grabará render() este mismo frame (Renderer::uiExtent()): el
    // pass del editor usa loadOp=LOAD, así que todo píxel que la UI no cubra deja ver
    // la escena. Maquetar con un tamaño distinto al del attachment es exactamente lo
    // que produce las franjas de juego en los bordes. Extent {0,0} → fallback al
    // tamaño en PÍXELES de la ventana (nunca al lógico de SDL_GetWindowSize).
    void beginFrame(float dt, VkExtent2D fbExtent);
    void render(VkCommandBuffer cmd, VkImageView swapchainView, VkExtent2D extent);  // graba draws
    bool wantsInput() const;                 // FluentUI quiere ratón/teclado (sobre cualquier panel/barra)
    // Rect del pane "Viewport" en píxeles (el hueco central donde se ve la escena). El
    // picking del modo lo usa para ignorar los clics que caen sobre la UI: es geométrico y
    // por tanto inmune al retraso con el que la UI publica su hover. Lo calcula el layout
    // al dibujar, así que es el del frame anterior (los paneles solo se mueven al arrastrar
    // un splitter, y ahí el ratón está sobre el divisor, no sobre la escena).
    void viewportRect(float& x, float& y, float& w, float& h) const {
        x = m_viewportRect.pos.x;  y = m_viewportRect.pos.y;
        w = m_viewportRect.size.x; h = m_viewportRect.size.y;
    }

    // Paneles del HUD que flotan SOBRE el viewport (jerarquía, inspector, consola, dock…).
    // Con el layout overlay el viewport ocupa casi toda la ventana, así que el picking del
    // modo necesita restar además estos rects (mismo criterio geométrico y mismo frame de
    // lag que viewportRect). Los publica el layout al dibujar.
    int  uiRectCount() const { return static_cast<int>(m_uiRectsPrev.size()); }
    void uiRectAt(int i, float& x, float& y, float& w, float& h) const {
        const FluentUI::Rect& r = m_uiRectsPrev[static_cast<size_t>(i)];
        x = r.pos.x;  y = r.pos.y;  w = r.size.x;  h = r.size.y;
    }

    // Contexto FluentUI de este editor (void* = FluentUI::UIContext*). La 2ª ventana
    // (ToolWindow) lo usa como "shareFrom" para crear SU contexto sobre el MISMO device +
    // resource-pool del editor principal (CreateStandaloneContext ownSwapchain).
    void* uiContext() const { return m_uictx; }
    bool consumeTileEditorRequest();         // true una vez si el usuario pidió abrir el editor de tiles
    bool consumeQuitRequest();               // true una vez si el usuario pidió salir (menú Archivo)

    // Play/Pausa/Stop. El editor solo PIDE la transición (los botones); el estado real lo
    // decide el Engine —que es quien para la simulación— y lo devuelve con setRunMode.
    bool    consumeRunModeRequest(RunMode& out);
    void    setRunMode(RunMode m) { m_runMode = m; }
    RunMode runMode() const { return m_runMode; }

    // "Cambios sin guardar". El Engine lo lee al empezar a jugar y lo devuelve al parar:
    // lo que la partida toque no debe dejar el proyecto marcado como sucio.
    // Herramienta de manipulación elegida en el dock (0 Seleccionar, 1 Mover, 2 Rotar,
    // 3 Escalar). El modo la lee para dibujar el gizmo que corresponda.
    int  gizmoTool() const { return m_gizmoTool; }

    bool sceneDirty() const { return m_sceneDirty; }
    void setSceneDirty(bool d) { m_sceneDirty = d; }

    // Pantalla de bienvenida (selector de proyecto). El Engine la activa al arrancar y la
    // apaga al montar el juego.
    void setWelcomeMode(bool w) { m_welcome = w; }
    // Si el usuario eligió crear/abrir un proyecto, devuelve true una vez y rellena la ruta
    // del .pkproj y si es nuevo (Save dialog) o existente (Open/reciente). El Engine lo aplica.
    bool consumeProjectChoice(std::string& outPath, bool& outIsNew, std::string& outTemplate);

private:
    void  buildPanels(float dt, int w, int h);
    void  buildWelcomeScreen(int w, int h);   // selector de proyecto (Nuevo/Abrir/recientes)
    static std::string templateLabel(const std::string& name);   // "Demo" → "Mundo de ejemplo"
    void* thumbnailFor(VkImageView view);   // registra/cachea la textura para Image()
    void  drawFolderTree(const AssetNode& node);          // columna izq: solo carpetas
    void  drawAssetCards(const AssetNode& folder, float gridW);  // columna der: tarjetas
    void  markDragSource(const std::string& path);        // tarjeta pulsada = origen del arrastre
    void  paintChromeBackground(float fw, float fh);   // fondo opaco de la UI salvo el viewport
    void  drawTitleBar(bool withCommands);   // chrome de ventana propio (menú + min/max/cerrar)
    void  buildTitleBarContent();            // contenido componible del TitleBar (icono + menú)
    // Contenido de cada panel, dibujado DENTRO de su pane del Splitter (calcula pos/size del
    // pane con cursorPos + layoutStack.back().availableSpace).
    void  buildHierarchyPanel();
    void  buildInspectorPanel();
    void  buildConsoleTabs();

    // --- HUD flotante (layout overlay: el viewport ES la ventana) ---------------------
    // Superficie flotante translúcida con cabecera propia. Pinta sombra + fondo, recorta
    // el contenido a su rect, publica el rect para la oclusión del picking y deja el cursor
    // dentro. `open` (opcional) lo hace plegable desde el chevron de la cabecera: con el
    // panel plegado se dibuja SOLO la cabecera y devuelve false.
    // Contrato: llamar a endOverlay() SOLO si devolvió true.
    // Descriptor de una superficie del HUD. Es un struct y no una lista de parámetros
    // posicionales porque ya iban nueve: así la llamada dice qué es cada cosa.
    struct OverlayDesc {
        const char*    id    = nullptr;
        FluentUI::Vec2 pos;
        FluentUI::Vec2 size;
        const char*    title = nullptr;
        uint32_t       icon  = 0;
        bool*          open  = nullptr;     // plegable desde el chevron de la cabecera
        const char*    note  = nullptr;     // aviso a la derecha (lo único visible si está plegado)
        bool           noteAlert = false;   // ese aviso en rojo (hay errores)
        bool*          maximized = nullptr; // añade el botón maximizar/restaurar
        float          bgAlpha = 0.93f;     // opacidad del fondo (el juego se ve por detrás)
        bool           anchorRight  = false; // su posición se calcula desde el borde DERECHO
        bool           anchorBottom = false; // ídem desde el borde INFERIOR
        bool           resizable    = true;  // permite estirarlo por bordes y esquinas
        bool           draggable    = true;  // permite moverlo arrastrando su cabecera
        // Panel cuyo desplazamiento se mueve al arrastrar ESTE (el ancla de su cluster):
        // así arrastrar la cabecera de cualquier miembro mueve el bloque entero. Nulo = él mismo.
        const char*    moveId       = nullptr;
        bool           inCluster    = false; // acoplado con otros: el doble clic lo SACA
    };
    bool  beginOverlay(const OverlayDesc& d);
    void  endOverlay();
    // Estado de arrastre/posición de un panel del HUD. `offset` es lo que el usuario lo ha
    // separado de su sitio por defecto (el que calcula el layout): así el panel sigue
    // reaccionando a un cambio de tamaño de ventana y a la vez respeta dónde lo dejaste.
    struct OverlayState {
        FluentUI::Vec2 offset;                 // desplazamiento manual respecto al ancla
        FluentUI::Vec2 sizeDelta;              // ampliación manual respecto al tamaño del layout
        FluentUI::Vec2 grab;                   // punto agarrado dentro del panel (al arrastrar)
        bool           dragging  = false;
        float          lastClick = -10.0f;     // para detectar el doble clic que lo restablece
        int            edge      = 0;          // borde en redimensión: 1=izq 2=der 4=arriba 8=abajo
        FluentUI::Vec2 dragStart;              // ratón al empezar a redimensionar
        FluentUI::Vec2 startOffset, startSize; // offset/sizeDelta al empezar (deltas absolutos)
    };
    std::unordered_map<std::string, OverlayState> m_overlayState;
    bool  overlayMoved(const char* id) const;  // ¿el usuario lo ha movido de su sitio?
    void  resetOverlayPositions();             // devuelve todos los paneles a su sitio y tamaño
    // Tamaño efectivo de un panel = el que pide el layout + lo que el usuario haya estirado.
    // El layout lo consulta ANTES de colocarlo, porque los paneles anclados a un borde
    // (inspector a la derecha, consola abajo) calculan su posición a partir del tamaño.
    FluentUI::Vec2 overlaySize(const char* id, FluentUI::Vec2 base) const;
    void  pushUiRect(const FluentUI::Rect& r);   // rect que tapa la escena este frame

    // --- Acoplado de paneles ----------------------------------------------------------
    // Los tres paneles del HUD pueden compartir una misma superficie flotante: al soltar uno
    // sobre la cabecera de otro se agrupan y pasan a convivir como PESTAÑAS. Un grupo se
    // dibuja con la geometría de su panel PRINCIPAL (tabs[0]), que es también su identidad
    // (el id del overlay), así que fusionar o separar no pierde posición ni tamaño.
    enum class PanelId { Hierarchy, Inspector, Console };
    struct PanelGroup {
        std::vector<PanelId> tabs;      // ≥1; tabs[0] manda en geometría e identidad
        int                  active = 0;
    };
    // Un CLUSTER es un rectángulo que uno o varios grupos se reparten: acoplar por un lado
    // mete el grupo en el cluster del destino (columnas o filas, con divisor arrastrable);
    // acoplar sobre la cabecera lo mete como pestaña DENTRO de un grupo. Un cluster de un
    // solo miembro es exactamente el panel flotante de siempre.
    struct DockCluster {
        std::vector<PanelGroup> members;   // ≥1, en orden de izquierda→derecha o arriba→abajo
        std::vector<float>      ratios;    // fracción del rect por miembro (suman 1)
        bool                    vertical = false;   // true = apilados; false = lado a lado
        // Panel que da al cluster su sitio y su tamaño. Es el DESTINO del acople, no el
        // primer miembro: al soltar algo a la izquierda de la consola, el bloque debe
        // quedarse donde estaba la consola, no saltar al rincón del panel recién llegado.
        PanelId                 anchor = PanelId::Hierarchy;
    };
    // Panel del cluster que fija su geometría (con respaldo si el ancla ya no está dentro).
    PanelId clusterAnchor(const DockCluster& cl) const;
    std::vector<DockCluster> m_clusters{
        { { { { PanelId::Hierarchy }, 0 } }, { 1.0f }, false, PanelId::Hierarchy },
        { { { { PanelId::Inspector }, 0 } }, { 1.0f }, false, PanelId::Inspector },
        { { { { PanelId::Console   }, 0 } }, { 1.0f }, false, PanelId::Console   } };
    // Cabeceras dibujadas el frame anterior (id de overlay → rect): son el blanco del drop.
    std::unordered_map<std::string, FluentUI::Rect> m_headerRects, m_headerRectsPrev;
    // Divisor de cluster que se está arrastrando (reparte espacio entre dos miembros).
    struct SplitDrag { int cluster = -1; int index = -1; float lastPos = 0.0f; };
    SplitDrag      m_splitDrag;
    // Desacople PEDIDO durante el dibujado (doble clic en una cabecera o en una pestaña). Se
    // resuelve al terminar el bucle: tocar m_clusters mientras se recorre invalida las
    // referencias en uso — y el panel acababa en cualquier sitio.
    int            m_undockCluster = -1, m_undockMember = -1, m_undockTab = -1;
    std::string    m_dropSourceId;      // panel que se acaba de soltar (vacío = ninguno)
    FluentUI::Vec2 m_dropPos;           // dónde se soltó
    // Pista de acople mientras se arrastra: qué panel es el destino y por dónde entraría.
    enum class DockSide { None, Tab, Left, Right, Top, Bottom };
    std::string    m_dockHintId;
    DockSide       m_dockHintSide = DockSide::None;
    FluentUI::Rect m_dockHintRect;      // la silueta que se pinta como previsualización
    // Rects COMPLETOS de cada panel (no solo su cabecera): son las zonas de drop del acople
    // lateral. Como el hit-test ocurre DENTRO del dibujado, hace falta el mapa del frame
    // ANTERIOR: el de este frame aún se está llenando (mismo patrón que m_uiRects).
    std::unordered_map<std::string, FluentUI::Rect> m_panelRects, m_panelRectsPrev;

    static const char* panelOverlayId(PanelId p);
    const char*  panelTitle(PanelId p) const;
    uint32_t     panelIcon(PanelId p) const;
    void         buildPanelContent(PanelId p);
    // Localiza el grupo de un panel: devuelve cluster y miembro (-1 si no está).
    void         locateGroup(const std::string& overlayId, int& cluster, int& member) const;
    // Qué acople resultaría de soltar `p` (cursor) viniendo de `srcId`: devuelve el lado y
    // rellena destino + silueta. Se usa para la PISTA mientras se arrastra y para resolver el
    // drop: calcularlo en los dos sitios evita depender de un hint del frame anterior.
    DockSide     dockZoneAt(const std::string& srcId, FluentUI::Vec2 p,
                            std::string& outTarget, FluentUI::Rect& outRect) const;
    void         dockAsTab(const std::string& movedId, const std::string& targetId);
    void         dockBeside(const std::string& movedId, const std::string& targetId, DockSide side);
    void         undockGroup(int cluster, int member);        // saca un grupo a su propio cluster
    void         undockTab(int cluster, int member, int tab); // saca una pestaña a su grupo
    // Fila de pestañas de un grupo con más de una. Devuelve true si cambió la activa.
    bool         buildGroupTabs(PanelGroup& g, float width, int cluster, int member);
    // ¿Ese punto cae sobre un panel del HUD? (rects del frame anterior, como viewportRect).
    bool  pointOverHud(float x, float y) const {
        for (const FluentUI::Rect& r : m_uiRectsPrev)
            if (r.Contains(FluentUI::Vec2(x, y))) return true;
        return false;
    }
    // Play/Pausa/Stop + herramientas, flotante abajo al centro. `bottom` es el rect que ya
    // ocupa el panel inferior: el dock se sube por encima de él si se solaparían.
    // `bottom` es el rect que ya ocupa el panel inferior. `preferTop` lo manda arriba del
    // todo: con el navegador de assets grande, un dock pegado al panel se lee como un
    // apéndice suyo, y arriba queda claro que es del editor.
    void  buildTransportDock(float fw, float fh, float top,
                             const FluentUI::Rect& bottom, bool preferTop);
    void  loadHudState();                           // visibilidad de los paneles (1 vez por proyecto)

    // --- Paleta de comandos (Ctrl+K) --------------------------------------------------
    // Con el HUD plegable y sin toolbar, la paleta es la puerta rápida a TODO: comandos del
    // menú, entidades de la escena y assets del proyecto. Un ítem = una etiqueta + lo que
    // hace al ejecutarse.
    struct PaletteItem {
        std::string           label;   // lo que se busca y se muestra
        std::string           hint;    // columna derecha (categoría, ruta, atajo)
        uint32_t              icon = 0;
        std::function<void()> run;
    };
    void  collectPaletteItems(const std::string& query, std::vector<PaletteItem>& out);
    void  buildCommandPalette(float fw, float top);
    void  handleShortcuts();   // Ctrl+K, Ctrl+1/2/3 (los atajos del propio editor)

    // Menú Archivo: la op se pide desde el menú (su diálogo puede invocar el callback
    // en OTRO hilo) y se ejecuta en beginFrame —hilo principal, antes de dibujar— para
    // no cambiar la escena activa a mitad de frame.
    enum class SceneOp { None, New, Open, Save, SaveAs, NewProject, OpenProject };
    void applySceneOp(SceneOp op, const std::string& path);

    // Acciones que DESCARTAN la escena en memoria. Si hay cambios sin guardar se piden
    // por confirmación (ContentDialog) en vez de ejecutarse directamente.
    enum class GuardedAction { None, NewScene, OpenScene, NewProject, OpenProject, Quit };
    void requestGuarded(GuardedAction a);   // ejecuta ya, o abre el diálogo si está sucia
    void runGuarded(GuardedAction a);       // ejecuta la acción sin preguntar
    void buildDirtyDialog();                // diálogo "cambios sin guardar" (cada frame)
    void openSaveDialog();   // diálogo "Guardar como" → encola SaveAs con la ruta elegida
    // Diálogo "Asignar/Cambiar script" del inspector: elige un .lua y lo deja PENDIENTE
    // para aplicarlo a `target` en beginFrame (el callback puede correr en otro hilo).
    void openScriptDialog(Entity target);
    // Diálogo "Nuevo script": elige dónde crearlo, se escribe la PLANTILLA y (si `target`
    // es una entidad viva) se le asigna. Mismo camino diferido que openScriptDialog.
    void openNewScriptDialog(Entity target);
    // Adjunta `path` (relativa al proyecto) a la entidad: añade o reasigna su
    // ScriptComponent. El ScriptSystem instancia/re-instancia solo al frame siguiente.
    void assignScript(Entity e, const std::string& path);
    // Diálogo "Textura del sprite" (filtro de imágenes). Mismo camino diferido que el de
    // script: la elección se aplica en beginFrame, en el hilo principal.
    void openTextureDialog(Entity target);
    // Pone `path` como textura de la entidad: añade o reasigna su SpriteComponent
    // (resuelve el handle con el AssetManager respetando su FilterMode).
    void assignSprite(Entity e, const std::string& path);
    // Acciones compartidas por el menú (TitleBar) y los comandos rápidos (toolbar): encolan la
    // op o abren el diálogo nativo correspondiente.
    void newScene();
    void saveScene();
    void openSceneDialog();
    void newProjectDialog();
    void openProjectDialog();
    // Menú Entidad: el alta la ejecuta el MODO (sabe dónde mira la vista y qué lleva cada
    // tipo); aquí solo se publica la petición por el bus. "Crear sprite…" pasa antes por el
    // diálogo de imagen, cuyo callback puede venir de otro hilo (m_pendingNewSpritePath).
    void requestNewEntity(NewEntityKind kind, const std::string& path = std::string{});
    void openNewSpriteDialog();
    void deleteSelectedEntity();        // Supr / menú Entidad → Eliminar
    void refreshAfterProjectChange();   // tras Nuevo/Abrir proyecto: recarga tileset/assets, resetea scripts/selección

    SDL_Window*   m_window      = nullptr;
    Renderer*     m_renderer    = nullptr;
    AssetManager* m_assets      = nullptr;
    Selection*    m_selection   = nullptr;   // selección compartida (la posee el Engine)
    EventBus*     m_eventBus    = nullptr;   // para publicar el drop de assets al viewport
    Subscription  m_sceneEditedSub;          // escucha de las ediciones hechas con el gizmo
    SceneManager* m_sceneMgr    = nullptr;   // escena activa (jerarquía/inspector)
    ScriptSystem* m_scriptSys   = nullptr;   // exports del script seleccionado
    void*         m_uictx       = nullptr;   // FluentUI::UIContext* de este editor
    FluentUI::PlatformBackend* m_platform = nullptr;   // SDLPlatform (no posee SDL); TitleBar/cursores
    VkDevice      m_device      = VK_NULL_HANDLE;
    bool          m_initialized = false;
    bool          m_requestTileEditor = false;
    bool          m_requestQuit       = false;
    RunMode       m_runMode        = RunMode::Edit;   // estado que muestran los botones (lo fija el Engine)
    bool          m_runModeRequested = false;         // hay una transición pedida sin consumir
    RunMode       m_runModeRequest = RunMode::Edit;
    float         m_fps         = 0.0f;
    float         m_dockContentW = 0.0f;   // ancho medido del contenido del dock (frame previo)

    // Bienvenida: estado + elección pendiente (el callback del diálogo puede venir de otro
    // hilo; se protege con m_pendingMutex, igual que las ops de escena).
    bool          m_welcome            = false;
    bool          m_pendingProject     = false;
    bool          m_pendingProjectIsNew = false;
    std::string   m_pendingProjectPath;
    // Plantilla con la que nacerá el proyecto ("Empty" = en blanco). En la bienvenida la
    // elige el ComboBox; en el editor, la entrada de menú por la que se pidió.
    std::string   m_newProjectTemplate = "Empty";
    std::vector<std::string> m_templateNames;    // cacheadas al dibujar la bienvenida
    int           m_templateSel = 0;

    // Navegador de assets.
    std::unordered_map<VkImageView, void*> m_thumb;   // handles de miniatura (FluentUI)
    std::vector<std::string>               m_pendingLoads;  // rutas a cargar (hilo principal)
    std::mutex                             m_pendingMutex;
    SceneOp                                m_pendingSceneOp = SceneOp::None;   // acción del menú Archivo
    std::string                            m_pendingScenePath;                 // ruta del diálogo (Open/SaveAs)
    // Script pedido desde un diálogo: Assign = adjuntar uno existente; Create = crearlo
    // con la plantilla y (si hay entidad destino) adjuntarlo también.
    enum class ScriptOp { None, Assign, Create };
    ScriptOp                               m_pendingScriptOp = ScriptOp::None;
    Entity                                 m_pendingScriptEntity{};            // entidad destino (inválida = solo crear)
    std::string                            m_pendingScriptPath;                // .lua elegido en el diálogo
    // Imagen elegida en "Crear sprite…": se importa y se convierte en CreateEntityEvent en
    // beginFrame (el diálogo responde desde otro hilo).
    std::string                            m_pendingNewSpritePath;
    // Textura pedida desde el diálogo de "Apariencia" (misma mecánica que el script:
    // el callback puede venir de otro hilo; se aplica en beginFrame).
    Entity                                 m_pendingTextureEntity{};
    std::string                            m_pendingTexturePath;               // imagen elegida (vacía = nada pendiente)

    // Cambios sin guardar. Se marca al editar por el inspector o al soltar un asset en el
    // viewport (las dos vías de edición que pasan por el editor) y se limpia al guardar,
    // cargar o crear escena. Las acciones que descartan la escena pasan por el diálogo.
    bool          m_sceneDirty     = false;
    bool          m_dirtyDlgOpen   = false;                 // ContentDialog visible
    GuardedAction m_dirtyPending   = GuardedAction::None;   // acción a ejecutar tras decidir
    GuardedAction m_afterSaveAction = GuardedAction::None;  // acción diferida hasta que el SaveAs termine
    int                                    m_bottomTab = 0; // 0 = Consola, 1 = Assets
    AssetBrowser                           m_assetBrowser;        // escaneo de Assets/ en disco
    bool                                   m_assetsScanned = false;
    std::unordered_map<std::string, bool>  m_folderOpen;          // expansión por ruta de carpeta
    std::string                            m_selectedFolder = "Assets";  // carpeta mostrada en el grid
    std::string                            m_selectedAsset;              // tarjeta marcada (selección única)
    std::string                            m_assetFolderShown;           // carpeta del último dibujado (detecta el cambio)
    std::string                            m_dragSourcePath;             // tarjeta que originó el drag
    bool                                   m_dragEnded = false;          // soltó: limpiar el origen al frame siguiente
    std::string                            m_assetFilter;                // texto del buscador de assets
    // Un asset soltado FUERA del viewport: buildPanels lo detecta al principio del frame
    // (antes de que las tarjetas cancelen el drag) y la jerarquía —que se dibuja después—
    // lo consume si el punto cae sobre una de sus filas. Un .lua se adjunta como script;
    // una imagen pasa a ser la textura del sprite de esa entidad.
    std::string                            m_hierDropPath;
    FluentUI::Vec2                         m_hierDropPos;
    bool                                   m_hierDropIsScript = false;

    // Layout OVERLAY ("el juego primero"): no hay panes; el viewport ocupa la ventana entera
    // bajo la barra de título y los paneles flotan encima como HUD plegable.
    bool m_showHierarchy = true;   // panel flotante de escena (izquierda)
    bool m_showInspector = true;   // panel flotante de la selección (derecha)
    bool m_showConsole   = false;  // consola desplegada (plegada = solo su cabecera)
    bool m_hudLoaded     = false;  // el estado del HUD se lee una vez, con proyecto abierto
    int  m_gizmoTool     = 1;      // Mover por defecto (lo que se usa el 90 % del tiempo)
    bool m_bottomMax     = false;  // panel inferior a pantalla casi completa (assets a gusto)
    // Alpha del panel del estilo antes de entrar en un overlay (beginOverlay lo baja para que
    // los contenedores de dentro hereden su translucidez; endOverlay lo restaura).
    float m_overlayPrevPanelAlpha = 1.0f;
    // Paleta de comandos: abierta, texto buscado, fila resaltada y petición de foco (el campo
    // se enfoca solo el primer frame tras abrirla, para poder escribir sin clicar).
    bool        m_paletteOpen  = false;
    bool        m_paletteFocus = false;
    int         m_paletteSel   = 0;
    std::string m_paletteQuery;
    FluentUI::Rect m_viewportRect; // rect de la escena (modelo overlay); para el drop 1-frame-lag
    // Rects del HUD: los que se van acumulando ESTE frame y los del anterior, que son los que
    // se publican (el rect solo se conoce al dibujar, igual que m_viewportRect).
    std::vector<FluentUI::Rect> m_uiRects;
    std::vector<FluentUI::Rect> m_uiRectsPrev;
    // Alto REAL que ocupó el árbol de la jerarquía al dibujarlo (filas + espaciados + DPI,
    // ya aplicados). Es lo que permite dimensionar el panel al contenido sin adivinar:
    // estimar el alto de fila dejaba la última entidad fuera del recorte.
    float m_hierContentH = 0.0f;

    // Extent con el que se construyó la UI de ESTE frame (lo fija beginFrame). render()
    // lo contrasta con el extent real del attachment: si divergen, la UI no cubre la
    // imagen entera y se filtra la escena por los bordes. En vez de manifestarse como
    // un artefacto visual difícil de atribuir, el invariante se comprueba y se avisa.
    VkExtent2D m_buildExtent{ 0, 0 };
    bool       m_extentMismatchWarned = false;
};

}  // namespace pk
