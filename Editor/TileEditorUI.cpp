// Editor/TileEditorUI.cpp — editor de tiles (T4) en la ventana de herramientas.
#include "Editor/TileEditorUI.h"

#include "Assets/AssetManager.h"
#include "Core/EventBus.h"
#include "Core/Log.h"
#include "Core/Project.h"
#include "Game/GameEvents.h"
#include "Game/SceneManager.h"
#include "Game/TileSet.h"
#include "Renderer/Vulkan/Texture.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"

#include "FluentGUI.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pk {

namespace {
// Alto reservado abajo para la barra de estado (la dibuja build() al final del frame).
constexpr float kStatusBandH = 28.0f;

// Nombre de archivo (sin ruta) de una ruta cualquiera.
std::string baseName(const std::string& p) {
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

// Normaliza la ruta elegida en el diálogo: si el archivo está bajo una carpeta "Assets/"
// del proyecto, la deja RELATIVA desde ahí (portable, resoluble desde el dir de trabajo);
// si está fuera, conserva la ruta absoluta tal cual (resoluble en esta máquina).
std::string toAssetRelative(std::string p) {
    for (char& ch : p) if (ch == '\\') ch = '/';
    const size_t pos = p.rfind("/Assets/");
    if (pos != std::string::npos) return p.substr(pos + 1);          // "…/Assets/x" -> "Assets/x"
    if (p.compare(0, 7, "Assets/") == 0) return p;                   // ya es relativa
    return p;                                                        // fuera del proyecto: absoluta
}

// UV de la celda 'cell' en un atlas de cols×rows (recorte del tileset PNG).
void cellUV(int cell, int cols, int rows, FluentUI::Vec2& uv0, FluentUI::Vec2& uv1) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    const int cx = cell % cols;
    const int cy = (cell / cols) % rows;
    uv0 = FluentUI::Vec2(static_cast<float>(cx)     / cols, static_cast<float>(cy)     / rows);
    uv1 = FluentUI::Vec2(static_cast<float>(cx + 1) / cols, static_cast<float>(cy + 1) / rows);
}
}  // namespace

// Atlas del tileset envuelto para FluentUI (o nullptr si no hay tileset → color plano).
// Carga perezosa por el AssetManager (caché compartida con el overworld) y registro
// cacheado; se re-registra si la textura cambia. Debe llamarse con el contexto FluentUI
// de la ventana de tiles activo (lo está durante build()).
void* TileEditor::atlasHandle() {
    if (!m_assets) return nullptr;
    const TileSet& ts = TileSet::instance();
    const TextureHandle h = m_assets->loadTexture(ts.texture(), true, true, FilterMode::Pixel);
    if (h == m_assets->whiteTexture()) return nullptr;   // sin tileset.png → fallback color
    Texture* t = m_assets->getTexture(h);
    if (!t) return nullptr;
    // columns/rows se deducen del tamaño real del PNG y el tamaño de tile del JSON.
    TileSet::instance().resolveGrid(static_cast<int>(t->width()), static_cast<int>(t->height()));
    void* view = reinterpret_cast<void*>(t->view());
    if (view != m_atlasView) {            // 1ª vez (o cambió la textura) → registrar para FluentUI
        m_atlasView = view;
        m_atlasUi   = FluentUI::RegisterExternalTexture(view, nullptr, 0);
    }
    return m_atlasUi;
}

// Componente-mapa de la escena activa (o nullptr si aún no existe). El editor trabaja
// sobre una COPIA (m_map) y vuelca/recarga contra este componente.
TileMapComponent* TileEditor::mapComp() {
    if (!m_scenes) return nullptr;
    Scene& s = m_scenes->current();
    Entity found{};
    s.view<TileMapComponent>().each([&](Entity e, TileMapComponent&) { found = e; });
    if (!found.valid()) return nullptr;
    return &s.get<TileMapComponent>(found);
}

void TileEditor::ensureLoaded() {
    // Recarga la copia de trabajo si es la primera vez O si cambió la escena/proyecto
    // (comparamos el PUNTERO de la escena, igual que el overworld): si no, m_map quedaría
    // obsoleta y "Guardar" volcaría datos viejos sobre la escena nueva.
    const Scene* cur = m_scenes ? &m_scenes->current() : nullptr;
    if (m_loaded && cur == m_lastScene) return;
    m_loaded    = true;
    m_lastScene = cur;
    if (const TileMapComponent* c = mapComp()) {
        m_map = c->map;   // copia de trabajo desde el componente de la escena
        std::snprintf(m_status, sizeof(m_status), "Mapa %dx%d cargado", m_map.width(), m_map.height());
    } else {
        // Sin entidad-mapa la copia de trabajo queda VACÍA: pintar aquí un mapa de demo
        // haría creer que hay mundo donde no lo hay (y "Guardar" no tendría dónde volcarlo).
        m_map.assign(0, 0, {}, IVec2(0, 0));
        std::snprintf(m_status, sizeof(m_status),
                      "Sin mapa en la escena: créalo con Entidad → Crear mapa de tiles");
    }
    syncSizeBuffers();   // los campos del control "Redimensionar" reflejan el mapa cargado
}

// Vuelca el tamaño actual del mapa a los buffers de texto del control de redimensionar.
void TileEditor::syncSizeBuffers() {
    m_mapW = static_cast<double>(m_map.width());
    m_mapH = static_cast<double>(m_map.height());
}

void TileEditor::floodFill(int x, int y, TileType from, TileType to) {
    if (from == to || !m_map.inBounds(x, y)) return;
    std::vector<IVec2> stack{ { x, y } };
    while (!stack.empty()) {
        const IVec2 p = stack.back();
        stack.pop_back();
        if (!m_map.inBounds(p.x, p.y) || m_map.at(p.x, p.y) != from) continue;
        m_map.set(p.x, p.y, to);
        stack.push_back({ p.x + 1, p.y });
        stack.push_back({ p.x - 1, p.y });
        stack.push_back({ p.x, p.y + 1 });
        stack.push_back({ p.x, p.y - 1 });
    }
}

// "Guardar" (pantalla Pintar mapa) = UN solo botón que persiste TODO a disco: vuelca la
// copia de trabajo al componente, guarda la ESCENA (con el mapa) y el TILESET (los tipos).
// Antes esto solo escribía el componente en memoria, y faltaban dos guardados más —causa de
// que "guardé pero no quedó nada". Ahora un clic deja todo en disco bajo el proyecto activo.
void TileEditor::save() {
    TileMapComponent* c = mapComp();
    if (!c) {
        std::snprintf(m_status, sizeof(m_status), "No hay mapa en la escena");
        return;
    }
    // (1) Vuelca SOLO los tipos del mapa al componente; los overrides visuales por celda
    // (que pone el inspector del juego) se preservan al no tocarlos. PERO si el mapa se
    // redimensionó, su ancho cambió y los overrides (clave = y*width+x) apuntarían a celdas
    // equivocadas: los remapeamos al ancho nuevo y descartamos los que quedaron fuera.
    const int oldW = c->map.width(), oldH = c->map.height();
    c->map = m_map;
    const int newW = m_map.width(), newH = m_map.height();
    if (oldW > 0 && (oldW != newW || oldH != newH) && !c->overrides.empty()) {
        std::unordered_map<int, TileXform> remapped;
        for (const auto& kv : c->overrides) {
            const int x = kv.first % oldW, y = kv.first / oldW;
            if (x < newW && y < newH) remapped[y * newW + x] = kv.second;
        }
        c->overrides = std::move(remapped);
    }

    // (2) Persiste la ESCENA a disco: a su ruta actual, o a una por defecto bajo el proyecto
    // si aún no se ha guardado (SceneManager recuerda la ruta para próximas veces).
    bool sceneOk = false;
    if (m_scenes) {
        std::string scenePath = m_scenes->currentPath();
        if (scenePath.empty()) scenePath = Project::instance().resolveWrite("Assets/Data/scene.json");
        sceneOk = m_scenes->save(scenePath);
    }

    // (3) Persiste el TILESET (tipos) a disco.
    const bool tsOk = TileSet::instance().save();

    // (4) Avisa al overworld para reconstruir los sprites con lo guardado.
    if (m_bus) m_bus->emit(MapSavedEvent{});

    std::snprintf(m_status, sizeof(m_status), "Guardado a disco: mapa+escena%s, tileset%s",
                  sceneOk ? "" : " (ERROR)", tsOk ? "" : " (ERROR)");
}

void TileEditor::reload() {
    if (const TileMapComponent* c = mapComp()) {
        m_map = c->map;
        syncSizeBuffers();
        std::snprintf(m_status, sizeof(m_status), "Mapa recargado de la escena");
    } else {
        std::snprintf(m_status, sizeof(m_status), "No hay mapa en la escena");
    }
}

// Redimensiona la copia de trabajo del mapa según los buffers de texto (ancho×alto). Conserva
// lo pintado en la esquina 0,0; celdas nuevas = tipo por defecto. Se persiste al pulsar
// "Guardar" (que también remapea los overrides visuales si cambió el ancho).
void TileEditor::resizeMap() {
    // El NumberBox ya clampa a [1,512]; la comprobación se mantiene por si el valor llega
    // de otra vía (recarga, escena con un mapa raro).
    const int w = static_cast<int>(m_mapW);
    const int h = static_cast<int>(m_mapH);
    if (w < 1 || h < 1 || w > 512 || h > 512) {
        std::snprintf(m_status, sizeof(m_status), "Tamaño inválido (1..512). Sin cambios.");
        syncSizeBuffers();   // restaura los campos al tamaño real
        return;
    }
    if (w == m_map.width() && h == m_map.height()) {
        std::snprintf(m_status, sizeof(m_status), "El mapa ya es %dx%d.", w, h);
        return;
    }
    m_map.resize(w, h);
    syncSizeBuffers();
    std::snprintf(m_status, sizeof(m_status),
                  "Mapa redimensionado a %dx%d. Pulsa Guardar para persistir.", w, h);
}

void TileEditor::build(int width, int height) {
    ensureLoaded();
    applyPendingImage();   // imagen elegida en el diálogo (callback de otro hilo): aplícala aquí

    // La toolbar se dibuja en el cursor actual; lo fijamos arriba del todo para que
    // no quede un hueco ni la tape el contenido.
    auto* c = FluentUI::GetContext();
    if (c) c->cursorPos = FluentUI::Vec2(0.0f, 0.0f);

    // --- Toggle de modo: pintar el mapa vs configurar el tileset ---
    int screen = static_cast<int>(m_screen);
    FluentUI::BeginToolbar();
    FluentUI::SegmentedControl("screen",
        std::vector<std::string>{ "Pintar mapa", "Configurar tileset" }, &screen);
    FluentUI::EndToolbar();
    m_screen = static_cast<Screen>(screen);

    if (m_screen == Screen::Map) buildMapScreen(width, height);
    else                         buildTilesetScreen(width, height);

    FluentUI::BeginStatusBar(m_status);
    FluentUI::EndStatusBar();
}

// Pantalla "Pintar mapa": toolbar de pinceles + guardar/recargar del MAPA, y un Splitter
// paleta | lienzo (el divisor se arrastra; ya no hay un x0=150 fijo para el lienzo).
void TileEditor::buildMapScreen(int width, int height) {
    using FluentUI::Vec2;
    auto* c = FluentUI::GetContext();

    // --- Toolbar: herramienta activa + guardar/recargar ---
    int tool = static_cast<int>(m_tool);
    FluentUI::BeginToolbar();
    FluentUI::SegmentedControl("tool",
        std::vector<std::string>{ "Pintar", "Rect", "Balde", "Borrar" }, &tool);
    FluentUI::SameLine(12.0f);
    // Altura fija = 24 px para alinear con el SegmentedControl (sin esto los botones
    // usan la altura por defecto, más alta, y la barra queda desalineada).
    if (FluentUI::Button("Guardar",  FluentUI::Vec2(84.0f, 24.0f)))  save();
    FluentUI::SameLine(6.0f);
    if (FluentUI::Button("Recargar", FluentUI::Vec2(92.0f, 24.0f))) reload();
    FluentUI::EndToolbar();
    m_tool = static_cast<Tool>(tool);

    // Borde inferior real de la toolbar (EndToolbar dejó ahí el cursor): el área de
    // trabajo empieza justo debajo y llega hasta encima de la barra de estado.
    const float top  = c ? c->cursorPos.y : 40.0f;
    const float midH = static_cast<float>(height) - top - kStatusBandH;
    if (!c || midH <= 0.0f) return;

    c->cursorPos = Vec2(0.0f, top);
    if (FluentUI::BeginSplitter("tiles_mapSplit", true, &m_ratioPalette,
                                Vec2(static_cast<float>(width), midH))) {
        // --- Pane izquierdo: tamaño del mapa (en flujo) + paleta del pincel ---
        const Vec2 paneOrigin = c->cursorPos;
        const Vec2 paneSize   = c->layoutStack.empty() ? Vec2(0.0f, 0.0f)
                                                       : c->layoutStack.back().availableSpace;
        // Los campos reflejan el tamaño actual (syncSizeBuffers); "Redimensionar" aplica a
        // la copia de trabajo (conserva lo pintado) y "Guardar" lo persiste.
        if (FluentUI::BeginExpander("mapSize", "Mapa", FluentUI::Icons::LayoutGrid, &m_mapSizeOpen)) {
            FluentUI::NumberBox("Ancho", &m_mapW, 1.0, 512.0, 1.0, "%.0f");
            FluentUI::NumberBox("Alto",  &m_mapH, 1.0, 512.0, 1.0, "%.0f");
            if (FluentUI::Button("Redimensionar")) resizeMap();
            FluentUI::EndExpander();
        }
        drawPalette(remainingPane(paneOrigin, paneSize));

        FluentUI::SplitterPanel();

        // --- Pane derecho: el lienzo ocupa el pane entero ---
        const Vec2 canvasOrigin = c->cursorPos;
        const Vec2 canvasSize   = c->layoutStack.empty() ? Vec2(0.0f, 0.0f)
                                                         : c->layoutStack.back().availableSpace;
        drawCanvas(FluentUI::Rect(canvasOrigin, canvasSize));

        FluentUI::EndSplitter();
    }
}

// Rect libre del pane actual: desde donde quedó el cursor tras los widgets en flujo hasta
// el borde inferior del pane.
FluentUI::Rect TileEditor::remainingPane(FluentUI::Vec2 paneOrigin, FluentUI::Vec2 paneSize) const {
    auto* c = FluentUI::GetContext();
    const FluentUI::Vec2 pos = c ? c->cursorPos : paneOrigin;
    const float used = pos.y - paneOrigin.y;
    return FluentUI::Rect(pos, FluentUI::Vec2(paneSize.x, std::max(0.0f, paneSize.y - used)));
}

// Pantalla "Configurar tileset": acciones + inspector del tipo + lista (izq) + atlas (der).
void TileEditor::buildTilesetScreen(int width, int height) {
    auto* c = FluentUI::GetContext();

    // El tipo seleccionado puede quedar fuera de rango si se borraron tipos.
    const int n = TileSet::instance().count();
    if (m_selType >= n) m_selType = n - 1;
    if (m_selType < 0)  m_selType = 0;

    // --- Toolbar: guardar tileset + añadir/borrar tipo ---
    FluentUI::BeginToolbar();
    if (FluentUI::Button("Guardar tileset", FluentUI::Vec2(140.0f, 24.0f))) saveTileset();
    FluentUI::SameLine(8.0f);
    if (FluentUI::Button("Cambiar imagen…", FluentUI::Vec2(150.0f, 24.0f))) changeImageDialog();
    FluentUI::SameLine(8.0f);
    if (FluentUI::Button("+ Tipo", FluentUI::Vec2(80.0f, 24.0f))) addNewType();
    FluentUI::SameLine(6.0f);
    if (FluentUI::Button("+ Grupo", FluentUI::Vec2(84.0f, 24.0f))) addNewGroup();
    FluentUI::SameLine(6.0f);
    if (FluentUI::Button("- Borrar", FluentUI::Vec2(84.0f, 24.0f))) removeSelectedType();
    FluentUI::EndToolbar();

    const float top  = c ? c->cursorPos.y : 40.0f;
    const float midH = static_cast<float>(height) - top - kStatusBandH;
    if (!c || midH <= 0.0f) return;

    using FluentUI::Vec2;
    c->cursorPos = Vec2(0.0f, top);
    if (FluentUI::BeginSplitter("tiles_setSplit", true, &m_ratioTypes,
                                Vec2(static_cast<float>(width), midH))) {
        // --- Pane izquierdo: inspector del tipo (en flujo) + lista de tipos ---
        const Vec2 paneOrigin = c->cursorPos;
        const Vec2 paneSize   = c->layoutStack.empty() ? Vec2(0.0f, 0.0f)
                                                       : c->layoutStack.back().availableSpace;
        // Proyecto sin tileset todavía (no hay tileset.json): ni inspector ni lista, solo el
        // aviso. Sin este corte, editType() daría de alta un tipo "Tile" fantasma nada más
        // abrir la ventana — el motor ya no crea contenido por su cuenta.
        if (n == 0) {
            FluentUI::Label("Este proyecto aún no tiene tipos de tile.",
                            std::nullopt, FluentUI::TypographyStyle::Subtitle);
            FluentUI::Label("Pulsa «+ Tipo» para crear el primero y asígnale una celda de la imagen.",
                            std::nullopt, FluentUI::TypographyStyle::Caption);
        } else {
        // editType() devuelve una referencia editable a los campos del tipo; los widgets los
        // modifican EN SITIO (el cambio queda en memoria; "Guardar tileset" lo persiste).
        TileTypeDef& d = TileSet::instance().editType(m_selType);
        if (FluentUI::BeginExpander("typeInsp", "Tipo seleccionado",
                                    FluentUI::Icons::Settings, &m_typeInspOpen)) {
            FluentUI::TextInput("Nombre", &d.name);
            // Grupo (vacío = tile suelto). Teclear el mismo nombre en varios tipos los agrupa;
            // el grupo solo organiza la lista y permite fijar "pisable" de golpe.
            FluentUI::TextInput("Grupo", &d.group);
            FluentUI::ToggleSwitch("Pisable", &d.walkable, "Sí", "No");
            FluentUI::ToggleSwitch("Dispara encuentros (hierba)", &d.encounter, "Sí", "No");
            FluentUI::EndExpander();
        }
        drawTypeList(remainingPane(paneOrigin, paneSize));
        }

        FluentUI::SplitterPanel();

        // --- Pane derecho: cabecera del atlas (imagen + tamaño de tile) + rejilla ---
        const Vec2 atlasOrigin = c->cursorPos;
        const Vec2 atlasSize   = c->layoutStack.empty() ? Vec2(0.0f, 0.0f)
                                                        : c->layoutStack.back().availableSpace;
        const TileSet& ts = TileSet::instance();
        char title[128];
        std::snprintf(title, sizeof(title), "Imagen: %s   (%dx%d celdas)",
                      baseName(ts.texture()).c_str(), ts.columns(), ts.rows());
        FluentUI::Label(title, std::nullopt, FluentUI::TypographyStyle::Caption);
        // Tamaño de tile (px): controla cómo se trocea la imagen; el grid se re-deduce solo.
        // Es una elección entre opciones excluyentes → SegmentedControl, no tres botones.
        const int px = ts.tileWidth();
        int sel = (px == 8) ? 0 : (px == 32) ? 2 : 1;
        if (FluentUI::SegmentedControl("tilepx",
                std::vector<std::string>{ "8 px", "16 px", "32 px" }, &sel))
            TileSet::instance().setTileSize(sel == 0 ? 8 : sel == 2 ? 32 : 16);
        drawAtlasPanel(remainingPane(atlasOrigin, atlasSize));

        FluentUI::EndSplitter();
    }
}

// Añade un tipo nuevo (con valores por defecto) y lo deja seleccionado para configurarlo.
void TileEditor::addNewType() {
    TileTypeDef nd;            // defaults: name "Tile", cell 0, walkable true, sin encounter
    nd.name = "Nuevo";
    m_selType = TileSet::instance().addType(nd);
    std::snprintf(m_status, sizeof(m_status),
                  "Tipo añadido (#%d): renómbralo y asígnale una celda.", m_selType);
}

// Añade un tipo nuevo YA dentro de un grupo nuevo (nombre único "Grupo N"). Para sumar más
// tiles al grupo: crea/selecciona otros tipos y escribe el mismo nombre en el campo "Grupo".
void TileEditor::addNewGroup() {
    const std::vector<std::string> existing = TileSet::instance().groupOrder();
    std::string gname;
    for (int n = 1; ; ++n) {                        // primer "Grupo N" que no exista
        gname = "Grupo " + std::to_string(n);
        if (std::find(existing.begin(), existing.end(), gname) == existing.end()) break;
    }
    TileTypeDef nd;
    nd.name  = "Nuevo";
    nd.group = gname;
    m_selType = TileSet::instance().addType(nd);
    std::snprintf(m_status, sizeof(m_status),
                  "Grupo '%s' creado. Añade más tiles poniéndoles este mismo grupo.", gname.c_str());
}

// Borra el tipo seleccionado. El mapa guarda ÍNDICES, así que hay que remapear la copia de
// trabajo: las celdas del tipo borrado pasan al tipo 0 y los índices mayores bajan en 1.
// (El mapa de la escena se actualiza al pulsar "Guardar" en la pantalla de mapa.)
void TileEditor::removeSelectedType() {
    const int i = m_selType;
    if (TileSet::instance().count() <= 1) {
        std::snprintf(m_status, sizeof(m_status), "Debe quedar al menos un tipo.");
        return;
    }
    for (int y = 0; y < m_map.height(); ++y) {
        for (int x = 0; x < m_map.width(); ++x) {
            const TileType t = m_map.at(x, y);
            if (t == i)      m_map.set(x, y, kTileDefault);
            else if (t > i)  m_map.set(x, y, static_cast<TileType>(t - 1));
        }
    }
    TileSet::instance().removeType(i);
    const int n = TileSet::instance().count();
    if (m_selType >= n) m_selType = n - 1;
    std::snprintf(m_status, sizeof(m_status),
                  "Tipo borrado. Guarda el mapa (pantalla Pintar) y el tileset.");
}

// Abre el diálogo nativo para elegir la imagen del tileset (cualquier nombre/ubicación).
// El diálogo es NO bloqueante y su callback puede venir de otro hilo: sólo deja la ruta
// en m_pendingImage; applyPendingImage() la materializa en el hilo principal (build()).
void TileEditor::changeImageDialog() {
    FluentUI::ShowOpenFileDialog(
        m_window,
        std::vector<FluentUI::FileFilter>{ { "Imágenes", "png;jpg;bmp" }, { "Todos", "*" } },
        Project::instance().resolveWrite("Assets/Textures"), false,
        [this](const std::vector<std::string>& paths, int) {
            if (paths.empty()) return;                       // el usuario canceló
            std::lock_guard<std::mutex> lk(m_imgMutex);
            m_pendingImage    = paths[0];
            m_hasPendingImage = true;
        });
}

// Aplica la imagen elegida: apunta el TileSet a esa ruta y fuerza recarga del atlas. El
// cambio queda en memoria; "Guardar tileset" lo persiste en tileset.json (con la ruta
// normalizada). Llamado al inicio de build() (hilo principal, contexto FluentUI activo).
void TileEditor::applyPendingImage() {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(m_imgMutex);
        if (!m_hasPendingImage) return;
        path = m_pendingImage;
        m_hasPendingImage = false;
    }
    // Importa la imagen al proyecto (copia a Assets/Textures) y usa su ruta relativa; si ya
    // estaba dentro o falla la copia, cae a la normalización por ruta. Así el tileset es parte
    // del proyecto y persiste/portable.
    const std::string rel = Project::instance().importAsset(path, "Assets/Textures");
    path = rel.empty() ? toAssetRelative(path) : rel;
    TileSet::instance().setTexture(path);
    resetAtlasCache();   // el atlas cacheado apunta a la textura anterior: re-registrar
    std::snprintf(m_status, sizeof(m_status), "Imagen del tileset: %s", baseName(path).c_str());
}

// Escribe Assets/Data/tileset.json y avisa al overworld para que reconstruya los sprites
// del mapa con las celdas nuevas (el TileSet es un singleton, ya vive en memoria).
void TileEditor::saveTileset() {
    if (TileSet::instance().save()) {
        std::snprintf(m_status, sizeof(m_status), "tileset.json guardado (%d tipos)",
                      TileSet::instance().count());
        if (m_bus) m_bus->emit(MapSavedEvent{});
    } else {
        std::snprintf(m_status, sizeof(m_status), "No se pudo guardar tileset.json");
    }
}

// Paleta del pincel (columna izquierda de "Pintar mapa"): clusterizada por grupos igual que
// la lista de "Configurar tileset" (cabecera de grupo + miembros indentados; los sueltos al
// final). El clic fija el tipo a pintar (m_activeType). Es solo una VISTA: no reordena m_types.
void TileEditor::drawPalette(const FluentUI::Rect& r) {
    auto* c = FluentUI::GetContext();
    if (!c || r.size.x <= 0.0f || r.size.y <= 0.0f) return;
    const FluentUI::Color text = FluentUI::Color::FromHex("#e6e8ec");
    const FluentUI::Color acc  = FluentUI::Color::FromHex("#4a9eff");
    const FluentUI::Color head = FluentUI::Color::FromHex("#c8b06a");   // cabecera de grupo

    const float sw = 22.0f, gap = 6.0f, rowH = sw + gap, headH = 18.0f, spacer = 3.0f;
    const float px = r.pos.x + 10.0f;                             // margen izquierdo del pane
    const float mx = c->input.MouseX(), my = c->input.MouseY();
    const bool  pressed = c->input.IsMousePressed(0);
    const float vpTop = r.pos.y + 6.0f;                           // viewport de la paleta
    const float vpBot = r.pos.y + r.size.y;
    const float vpH   = std::max(0.0f, vpBot - vpTop);
    const float vpR   = r.pos.x + r.size.x - 10.0f;               // borde derecho del pane

    const TileSet& ts    = TileSet::instance();
    void*          atlas = atlasHandle();   // miniaturas reales del PNG (o nullptr → color)
    const int      cols  = ts.columns(), rows = ts.rows();
    const std::vector<std::string> groups = ts.groupOrder();

    // --- Altura total del contenido (para clamp del scroll): mismo criterio que drawTypeList,
    //     respetando grupos plegados (compartidos con la lista de config).
    auto membersOf = [&](const std::string& g) {
        int n = 0; for (int i = 0; i < ts.count(); ++i) if (ts.at(i).group == g) ++n; return n;
    };
    int  looseCount = 0;
    for (int i = 0; i < ts.count(); ++i) if (ts.at(i).group.empty()) ++looseCount;
    const bool anyLoose = looseCount > 0;
    float contentH = 0.0f;
    for (const std::string& g : groups)
        contentH += headH + (m_collapsedGroups.count(g) ? 0.0f : membersOf(g) * rowH) + spacer;
    if (anyLoose && !groups.empty()) contentH += headH;
    contentH += looseCount * rowH;

    const bool  overVp    = mx >= r.pos.x && mx <= vpR && my >= vpTop && my <= vpBot;
    const float maxScroll = std::max(0.0f, contentH - vpH);
    if (overVp) { const float w = c->input.MouseWheelY(); if (w != 0.0f) m_paletteScroll -= w * 48.0f; }
    m_paletteScroll = std::clamp(m_paletteScroll, 0.0f, maxScroll);

    c->renderer.PushClipRect(FluentUI::Vec2(r.pos.x, vpTop),
                             FluentUI::Vec2(vpR - r.pos.x, vpH));
    float y = vpTop - m_paletteScroll;

    // Dibuja una miniatura seleccionable (tile) en 'y' y avanza el cursor. 'indent' cuelga a
    // los miembros de su cabecera de grupo. Se saltan las offscreen.
    auto drawTile = [&](int i, float indent) {
        const float ry = y;
        y += rowH;
        if (ry + sw < vpTop || ry > vpBot) return;
        const FluentUI::Vec2 pos(px + indent, ry);
        const TileTypeDef& d = ts.at(i);
        if (atlas) {
            FluentUI::Vec2 uv0, uv1;
            cellUV(d.cell, cols, rows, uv0, uv1);
            c->renderer.DrawImage(pos, FluentUI::Vec2(sw, sw), atlas, uv0, uv1);
        } else {
            c->renderer.DrawRectFilled(pos, FluentUI::Vec2(sw, sw),
                                       FluentUI::Color(d.color.x, d.color.y, d.color.z, 1.0f), 3.0f);
        }
        if (i == m_activeType)
            c->renderer.DrawRect(FluentUI::Vec2(pos.x - 2.0f, pos.y - 2.0f),
                                 FluentUI::Vec2(sw + 4.0f, sw + 4.0f), acc, 3.0f);
        c->renderer.DrawText(FluentUI::Vec2(pos.x + sw + 8.0f, pos.y + 4.0f), d.name.c_str(), text, 13.0f);
        const bool over = overVp && mx >= px && mx <= vpR && my >= ry && my <= ry + sw;
        if (over && pressed) m_activeType = i;
    };

    // 1) Grupos: cabecera clicable (pliega/despliega, estado compartido con la config) +
    //    miembros indentados (si no está plegado).
    for (const std::string& g : groups) {
        const float hy = y;
        y += headH;
        const bool collapsed = m_collapsedGroups.count(g) > 0;
        if (!(hy + 16.0f < vpTop || hy > vpBot)) {
            const std::string label = std::string(collapsed ? "> " : "v ") + g;
            c->renderer.DrawText(FluentUI::Vec2(px, hy + 1.0f), label.c_str(), head, 12.0f);
            const bool overHed = overVp && mx >= px && mx <= vpR && my >= hy && my <= hy + 16.0f;
            if (overHed && pressed) {
                if (collapsed) m_collapsedGroups.erase(g);
                else           m_collapsedGroups.insert(g);
            }
        }
        if (!collapsed)
            for (int i = 0; i < ts.count(); ++i)
                if (ts.at(i).group == g) drawTile(i, 10.0f);
        y += spacer;
    }

    // 2) Tipos sueltos: cabecera "Sueltos" solo si además hay grupos.
    if (anyLoose && !groups.empty()) {
        const float hy = y;
        y += headH;
        if (!(hy + 16.0f < vpTop || hy > vpBot))
            c->renderer.DrawText(FluentUI::Vec2(px, hy + 1.0f), "Sueltos", head, 12.0f);
    }
    for (int i = 0; i < ts.count(); ++i)
        if (ts.at(i).group.empty()) drawTile(i, 0.0f);

    c->renderer.PopClipRect();

    if (maxScroll > 0.0f) {
        const float barX = vpR - 4.0f;
        c->renderer.DrawRectFilled(FluentUI::Vec2(barX, vpTop), FluentUI::Vec2(3.0f, vpH),
                                   FluentUI::Color(1.0f, 1.0f, 1.0f, 0.06f), 1.5f);
        const float thumbH = std::max(24.0f, vpH * vpH / contentH);
        const float thumbY = vpTop + (m_paletteScroll / maxScroll) * (vpH - thumbH);
        c->renderer.DrawRectFilled(FluentUI::Vec2(barX, thumbY), FluentUI::Vec2(3.0f, thumbH),
                                   FluentUI::Color(1.0f, 1.0f, 1.0f, 0.22f), 1.5f);
    }
}

void TileEditor::drawCanvas(const FluentUI::Rect& r) {
    auto* c = FluentUI::GetContext();
    if (!c || m_map.width() <= 0 || m_map.height() <= 0) return;

    const float x0 = r.pos.x + 8.0f, y0 = r.pos.y + 4.0f;
    const float availW = r.size.x - 16.0f;
    const float availH = r.size.y - 8.0f;
    if (availW <= 0.0f || availH <= 0.0f) return;
    float ts = std::min(availW / m_map.width(), availH / m_map.height());
    if (ts < 4.0f) ts = 4.0f;

    const float gridW = m_map.width()  * ts;
    const float gridH = m_map.height() * ts;

    // Fondo del lienzo.
    c->renderer.DrawRectFilled(FluentUI::Vec2(x0, y0), FluentUI::Vec2(gridW, gridH),
                               FluentUI::Color::FromHex("#14161a"));
    // Tiles (con un hueco de 1px que deja ver el fondo como rejilla). Con tileset cargado
    // se pinta el recorte real del atlas; si no, color plano por tipo.
    void*          atlas = atlasHandle();
    const TileSet& tset  = TileSet::instance();
    const int      cols  = tset.columns(), rows = tset.rows();
    for (int y = 0; y < m_map.height(); ++y) {
        for (int x = 0; x < m_map.width(); ++x) {
            const TileType t = m_map.at(x, y);
            const FluentUI::Vec2 cpos(x0 + x * ts + 1.0f, y0 + y * ts + 1.0f);
            const FluentUI::Vec2 csize(ts - 2.0f, ts - 2.0f);
            if (atlas) {
                FluentUI::Vec2 uv0, uv1;
                cellUV(tset.at(t).cell, cols, rows, uv0, uv1);
                c->renderer.DrawImage(cpos, csize, atlas, uv0, uv1);
            } else {
                const TileProps& p = tileProps(t);
                c->renderer.DrawRectFilled(cpos, csize,
                                           FluentUI::Color(p.color.x, p.color.y, p.color.z, 1.0f));
            }
        }
    }
    // Marcador del inicio del jugador.
    const IVec2 st = m_map.playerStart();
    c->renderer.DrawRect(FluentUI::Vec2(x0 + st.x * ts, y0 + st.y * ts),
                         FluentUI::Vec2(ts, ts), FluentUI::Color::FromHex("#4a9eff"), 0.0f);

    // --- Entrada / pintar ---
    const float mx = c->input.MouseX(), my = c->input.MouseY();
    const bool inCanvas = mx >= x0 && my >= y0 && mx < x0 + gridW && my < y0 + gridH;
    const int  cellX = static_cast<int>((mx - x0) / ts);
    const int  cellY = static_cast<int>((my - y0) / ts);
    const bool down    = c->input.IsMouseDown(0);
    const bool pressed = c->input.IsMousePressed(0);

    if (inCanvas && m_map.inBounds(cellX, cellY)) {
        switch (m_tool) {
            case Tool::Paint: if (down)    m_map.set(cellX, cellY, static_cast<TileType>(m_activeType)); break;
            case Tool::Erase: if (down)    m_map.set(cellX, cellY, kTileDefault); break;
            case Tool::Fill:  if (pressed) floodFill(cellX, cellY, m_map.at(cellX, cellY),
                                                     static_cast<TileType>(m_activeType)); break;
            case Tool::Rect:
                if (pressed) { m_rectActive = true; m_rectStart = { cellX, cellY }; }
                if (m_rectActive) m_rectEnd = { cellX, cellY };
                break;
        }
        // Resalta la celda bajo el cursor.
        c->renderer.DrawRect(FluentUI::Vec2(x0 + cellX * ts, y0 + cellY * ts),
                             FluentUI::Vec2(ts, ts), FluentUI::Color(1.0f, 1.0f, 1.0f, 0.4f), 0.0f);
    }

    // Pincel rectángulo: previsualización + commit al soltar.
    if (m_tool == Tool::Rect && m_rectActive) {
        const int rx0 = std::min(m_rectStart.x, m_rectEnd.x), rx1 = std::max(m_rectStart.x, m_rectEnd.x);
        const int ry0 = std::min(m_rectStart.y, m_rectEnd.y), ry1 = std::max(m_rectStart.y, m_rectEnd.y);
        c->renderer.DrawRect(FluentUI::Vec2(x0 + rx0 * ts, y0 + ry0 * ts),
                             FluentUI::Vec2((rx1 - rx0 + 1) * ts, (ry1 - ry0 + 1) * ts),
                             FluentUI::Color::FromHex("#4a9eff"), 0.0f);
        if (!down) {   // soltó → aplica
            for (int yy = ry0; yy <= ry1; ++yy)
                for (int xx = rx0; xx <= rx1; ++xx)
                    m_map.set(xx, yy, static_cast<TileType>(m_activeType));
            m_rectActive = false;
        }
    }
}

// Lista vertical de tipos (miniatura real del atlas + nombre + celda/flags). Clic
// selecciona el tipo a configurar (m_selType). Es la columna izquierda de la pantalla
// de tileset; el atlas de la derecha asigna la celda al tipo aquí seleccionado.
void TileEditor::drawTypeList(const FluentUI::Rect& r) {
    auto* c = FluentUI::GetContext();
    if (!c || r.size.x <= 0.0f || r.size.y <= 0.0f) return;
    const float top   = r.pos.y;
    const float x     = r.pos.x + 10.0f;
    const float listW = std::max(0.0f, r.size.x - 26.0f);
    const FluentUI::Color text = FluentUI::Color::FromHex("#e6e8ec");
    const FluentUI::Color sub  = FluentUI::Color::FromHex("#9aa0a6");
    const FluentUI::Color acc  = FluentUI::Color::FromHex("#4a9eff");
    const FluentUI::Color head  = FluentUI::Color::FromHex("#c8b06a");  // cabecera de grupo
    const FluentUI::Color walk  = FluentUI::Color::FromHex("#7fc77f");  // chip "pisable"
    const FluentUI::Color block = FluentUI::Color::FromHex("#e06a6a");  // chip "no pisable"

    const float sw = 28.0f, gap = 10.0f, rowH = sw + gap, headH = 22.0f, spacer = 4.0f;
    const float mx = c->input.MouseX(), my = c->input.MouseY();
    const bool  pressed = c->input.IsMousePressed(0);
    const float vpTop = top + 6.0f;                                    // viewport de la lista
    const float vpBot = r.pos.y + r.size.y;                       // borde inferior del pane
    const float vpH   = std::max(0.0f, vpBot - vpTop);
    const float vpR   = x + listW + 6.0f;                              // borde derecho del viewport

    const TileSet& ts    = TileSet::instance();
    void*          atlas = atlasHandle();
    const int      cols  = ts.columns(), rows = ts.rows();
    const std::vector<std::string> groups = ts.groupOrder();

    // --- Altura total del contenido (para clamp del scroll): cabecera + miembros (si no está
    //     plegado) + respiro por grupo, más los sueltos. Es barato de recomputar cada frame.
    auto membersOf = [&](const std::string& g) {
        int n = 0; for (int i = 0; i < ts.count(); ++i) if (ts.at(i).group == g) ++n; return n;
    };
    int  looseCount = 0;
    for (int i = 0; i < ts.count(); ++i) if (ts.at(i).group.empty()) ++looseCount;
    const bool anyLoose = looseCount > 0;
    float contentH = 0.0f;
    for (const std::string& g : groups)
        contentH += headH + (m_collapsedGroups.count(g) ? 0.0f : membersOf(g) * rowH) + spacer;
    if (anyLoose && !groups.empty()) contentH += headH;
    contentH += looseCount * rowH;

    // --- Scroll con la rueda cuando el cursor está sobre el viewport de la lista. ---
    const bool  overVp    = mx >= r.pos.x && mx <= vpR && my >= vpTop && my <= vpBot;
    const float maxScroll = std::max(0.0f, contentH - vpH);
    if (overVp) { const float w = c->input.MouseWheelY(); if (w != 0.0f) m_typeScroll -= w * 48.0f; }
    m_typeScroll = std::clamp(m_typeScroll, 0.0f, maxScroll);

    c->renderer.PushClipRect(FluentUI::Vec2(r.pos.x, vpTop),
                             FluentUI::Vec2(vpR - r.pos.x, vpH));
    float y = vpTop - m_typeScroll;   // y en PANTALLA del inicio del contenido (desplazado)

    // Dibuja una fila de tipo (miniatura + nombre + celda/flags) en 'y' y avanza el cursor.
    // 'indent' cuelga a los miembros de un grupo bajo su cabecera. Se saltan las offscreen.
    auto drawTypeRow = [&](int i, float indent) {
        const float ry = y;
        y += rowH;
        if (ry + sw < vpTop || ry > vpBot) return;    // fuera del viewport: no dibujar
        const FluentUI::Vec2 pos(x + 4.0f + indent, ry);
        const TileTypeDef& d = ts.at(i);
        if (atlas) {
            FluentUI::Vec2 uv0, uv1;
            cellUV(d.cell, cols, rows, uv0, uv1);
            c->renderer.DrawImage(pos, FluentUI::Vec2(sw, sw), atlas, uv0, uv1);
        } else {
            c->renderer.DrawRectFilled(pos, FluentUI::Vec2(sw, sw),
                                       FluentUI::Color(d.color.x, d.color.y, d.color.z, 1.0f), 3.0f);
        }
        if (i == m_selType)
            c->renderer.DrawRect(FluentUI::Vec2(pos.x - 3.0f, pos.y - 3.0f),
                                 FluentUI::Vec2(sw + 6.0f, sw + 6.0f), acc, 3.0f);
        c->renderer.DrawText(FluentUI::Vec2(pos.x + sw + 10.0f, pos.y + 1.0f), d.name.c_str(), text, 14.0f);
        char info[64];
        std::snprintf(info, sizeof(info), "celda %d%s%s", d.cell,
                      d.walkable ? "" : "  -  no pisable",
                      d.encounter ? "  -  hierba" : "");
        c->renderer.DrawText(FluentUI::Vec2(pos.x + sw + 10.0f, pos.y + 17.0f), info, sub, 11.0f);

        const bool over = overVp && mx >= x && mx <= x + listW && my >= ry && my <= ry + sw;
        if (over && pressed) m_selType = i;
    };

    // 1) Grupos: cabecera clicable (pliega/despliega como dropdown) + chip de colisión + los
    //    miembros indentados debajo (si no está plegado).
    for (const std::string& g : groups) {
        const float hy = y;
        y += headH;
        const bool collapsed = m_collapsedGroups.count(g) > 0;
        if (!(hy + 20.0f < vpTop || hy > vpBot)) {    // cabecera visible
            bool allWalk = true;                      // estado del chip = ¿todos pisables?
            for (int i = 0; i < ts.count(); ++i)
                if (ts.at(i).group == g && !ts.at(i).walkable) { allWalk = false; break; }

            // Marcador de plegado (ASCII, seguro en el atlas de la fuente) + nombre del grupo.
            const std::string label = std::string(collapsed ? "> " : "v ") + g;
            c->renderer.DrawText(FluentUI::Vec2(x, hy + 2.0f), label.c_str(), head, 13.0f);

            const char*           tag   = allWalk ? "pisable" : "no pisable";
            const FluentUI::Color tcol  = allWalk ? walk : block;
            const float           chipW = allWalk ? 58.0f : 76.0f;
            const float           chipX = x + listW - chipW - 2.0f;
            const FluentUI::Vec2  chipPos(chipX, hy);
            c->renderer.DrawRectFilled(chipPos, FluentUI::Vec2(chipW, 18.0f),
                                       FluentUI::Color(tcol.r, tcol.g, tcol.b, 0.12f), 4.0f);
            c->renderer.DrawRect(chipPos, FluentUI::Vec2(chipW, 18.0f), tcol, 4.0f);
            c->renderer.DrawText(FluentUI::Vec2(chipX + 8.0f, hy + 3.0f), tag, tcol, 11.0f);

            // Clics: el chip fija "pisable" de todo el grupo; el resto de la cabecera pliega.
            const bool overTag = overVp && mx >= chipX && mx <= chipX + chipW && my >= hy && my <= hy + 18.0f;
            const bool overHed = overVp && mx >= x && mx < chipX - 6.0f && my >= hy && my <= hy + 18.0f;
            if (pressed && overTag)      TileSet::instance().setGroupWalkable(g, !allWalk);
            else if (pressed && overHed) {
                if (collapsed) m_collapsedGroups.erase(g);
                else           m_collapsedGroups.insert(g);
            }
        }
        if (!collapsed)
            for (int i = 0; i < ts.count(); ++i)
                if (ts.at(i).group == g) drawTypeRow(i, 14.0f);
        y += spacer;
    }

    // 2) Tipos sueltos: cabecera "Sueltos" solo si además hay grupos (si no, la lista es igual
    //    que antes). Los sueltos no se pliegan.
    if (anyLoose && !groups.empty()) {
        const float hy = y;
        y += headH;
        if (!(hy + 20.0f < vpTop || hy > vpBot))
            c->renderer.DrawText(FluentUI::Vec2(x, hy + 2.0f), "Sueltos", head, 13.0f);
    }
    for (int i = 0; i < ts.count(); ++i)
        if (ts.at(i).group.empty()) drawTypeRow(i, 0.0f);

    c->renderer.PopClipRect();

    // Barra de scroll (indicador, sin arrastre): solo si el contenido no cabe.
    if (maxScroll > 0.0f) {
        const float barX = vpR - 4.0f;
        c->renderer.DrawRectFilled(FluentUI::Vec2(barX, vpTop), FluentUI::Vec2(3.0f, vpH),
                                   FluentUI::Color(1.0f, 1.0f, 1.0f, 0.06f), 1.5f);
        const float thumbH = std::max(24.0f, vpH * vpH / contentH);
        const float thumbY = vpTop + (m_typeScroll / maxScroll) * (vpH - thumbH);
        c->renderer.DrawRectFilled(FluentUI::Vec2(barX, thumbY), FluentUI::Vec2(3.0f, thumbH),
                                   FluentUI::Color(1.0f, 1.0f, 1.0f, 0.22f), 1.5f);
    }
}

// Atlas del tileset recortado en su rejilla (cols×rows). Clic en una celda = asignar
// esa celda al tipo seleccionado (m_selType). Resalta la celda ya asignada. Es la
// columna derecha de la pantalla de tileset.
void TileEditor::drawAtlasPanel(const FluentUI::Rect& r) {
    auto* c = FluentUI::GetContext();
    if (!c) return;
    void*          atlas = atlasHandle();
    const TileSet& ts    = TileSet::instance();
    const int      cols  = ts.columns(), rows = ts.rows();

    // El título ("Imagen: … (NxM celdas)") y el selector de tamaño de tile los dibuja
    // buildTilesetScreen como widgets en flujo; aquí solo va la rejilla del atlas.
    const float x      = r.pos.x + 8.0f;
    const float y0     = r.pos.y + 4.0f;
    const float availW = r.size.x - 16.0f;
    const float availH = r.size.y - 8.0f;
    if (availW <= 0.0f || availH <= 0.0f || cols < 1 || rows < 1) return;

    if (!atlas) {
        c->renderer.DrawText(FluentUI::Vec2(x, y0),
                             "Sin imagen de tileset: usa \"Cambiar imagen…\" para elegir una.",
                             FluentUI::Color::FromHex("#9aa0a6"), 14.0f);
        return;
    }

    float cell = std::min(availW / cols, availH / rows);
    if (cell > 48.0f) cell = 48.0f;
    if (cell < 6.0f)  cell = 6.0f;

    const float gridW = cols * cell, gridH = rows * cell;
    c->renderer.DrawRectFilled(FluentUI::Vec2(x, y0), FluentUI::Vec2(gridW, gridH),
                               FluentUI::Color::FromHex("#14161a"));

    const int             assigned = ts.at(m_selType).cell;
    const FluentUI::Color line     = FluentUI::Color::FromHex("#2a2e35");
    const FluentUI::Color acc      = FluentUI::Color::FromHex("#4a9eff");

    for (int cy = 0; cy < rows; ++cy) {
        for (int cx = 0; cx < cols; ++cx) {
            const int            idx = cy * cols + cx;
            const FluentUI::Vec2 pos(x + cx * cell, y0 + cy * cell);
            FluentUI::Vec2 uv0, uv1;
            cellUV(idx, cols, rows, uv0, uv1);
            c->renderer.DrawImage(pos, FluentUI::Vec2(cell, cell), atlas, uv0, uv1);
            c->renderer.DrawRect(pos, FluentUI::Vec2(cell, cell), line, 0.0f);
            if (idx == assigned)
                c->renderer.DrawRect(FluentUI::Vec2(pos.x + 1.0f, pos.y + 1.0f),
                                     FluentUI::Vec2(cell - 2.0f, cell - 2.0f), acc, 0.0f);
        }
    }

    // Hover + clic = asignar la celda al tipo seleccionado.
    const float mx = c->input.MouseX(), my = c->input.MouseY();
    const bool  pressed = c->input.IsMousePressed(0);
    const bool  inGrid  = mx >= x && my >= y0 && mx < x + gridW && my < y0 + gridH;
    if (inGrid) {
        const int cx = static_cast<int>((mx - x)  / cell);
        const int cy = static_cast<int>((my - y0) / cell);
        if (cx >= 0 && cx < cols && cy >= 0 && cy < rows) {
            c->renderer.DrawRect(FluentUI::Vec2(x + cx * cell, y0 + cy * cell),
                                 FluentUI::Vec2(cell, cell), FluentUI::Color(1.0f, 1.0f, 1.0f, 0.5f), 0.0f);
            if (pressed) {
                const int idx = cy * cols + cx;
                TileSet::instance().editType(m_selType).cell = idx;
                std::snprintf(m_status, sizeof(m_status), "'%s' usa la celda %d (sin guardar)",
                              TileSet::instance().at(m_selType).name.c_str(), idx);
            }
        }
    }
}

}  // namespace pk
