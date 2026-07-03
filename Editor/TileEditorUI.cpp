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
#include <optional>
#include <string>
#include <vector>

namespace pk {

namespace {
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
        m_map.loadAscii({
            "TTTTTTTTTTTT", "T..........T", "T..gggggg..T", "T..gGGGGg..T",
            "T..gGGGGg.wT", "T....P...wwT", "T..gggg..wwT", "T..gggg...wT",
            "T..........T", "TTTTTTTTTTTT",
        });
        std::snprintf(m_status, sizeof(m_status), "Sin mapa en la escena; demo %dx%d", m_map.width(), m_map.height());
    }
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
    // (que pone el inspector del juego) se preservan al no tocarlos.
    c->map = m_map;

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
        std::snprintf(m_status, sizeof(m_status), "Mapa recargado de la escena");
    } else {
        std::snprintf(m_status, sizeof(m_status), "No hay mapa en la escena");
    }
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

// Pantalla "Pintar mapa": toolbar de pinceles + guardar/recargar del MAPA, paleta y lienzo.
void TileEditor::buildMapScreen(int width, int height) {
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

    // Borde inferior real de la toolbar (EndToolbar dejó ahí el cursor): la paleta y
    // el lienzo se colocan justo debajo, sin solaparla.
    const float top = c ? c->cursorPos.y : 40.0f;

    drawPalette(top, width, height);
    drawCanvas(top, width, height);
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
    if (FluentUI::Button("- Borrar", FluentUI::Vec2(84.0f, 24.0f))) removeSelectedType();
    FluentUI::EndToolbar();

    const float top = c ? c->cursorPos.y : 40.0f;

    // --- Inspector del tipo seleccionado: nombre + flags (columna izquierda) ---
    // OJO con el espaciado vertical: TextInput dibuja su etiqueta ("Nombre") en una línea
    // PROPIA encima de la caja, así que ocupa ~52 px en total (etiqueta + caja). Las casillas
    // van bien por debajo para no solaparse con la caja de texto.
    const float ix = 12.0f, iy = top + 10.0f;
    if (c)
        c->renderer.DrawText(FluentUI::Vec2(ix, iy), "Tipo seleccionado",
                             FluentUI::Color::FromHex("#9aa0a6"), 12.0f);
    // editType() devuelve una referencia editable a los campos del tipo; TextInput/Checkbox
    // los modifican EN SITIO (el cambio queda en memoria; "Guardar tileset" lo persiste).
    TileTypeDef& d = TileSet::instance().editType(m_selType);
    FluentUI::TextInput("Nombre", &d.name, 210.0f, false, FluentUI::Vec2(ix, iy + 22.0f));
    FluentUI::Checkbox("Pisable",                     &d.walkable,  FluentUI::Vec2(ix, iy + 86.0f));
    FluentUI::Checkbox("Dispara encuentros (hierba)", &d.encounter, FluentUI::Vec2(ix, iy + 116.0f));

    const float listW = 300.0f;
    drawTypeList(iy + 152.0f - 6.0f, 10.0f, listW, height);   // drawTypeList dibuja desde top+6

    // Tamaño de tile (px): controla cómo se trocea la imagen. Presets en la cabecera del
    // atlas (a la derecha del título "Imagen: …"). El grid se rededuce solo al cambiarlo.
    const float hx = static_cast<float>(width) - 230.0f;
    if (c)
        c->renderer.DrawText(FluentUI::Vec2(hx, top + 7.0f), "Tile px:",
                             FluentUI::Color::FromHex("#9aa0a6"), 13.0f);
    if (FluentUI::Button("8",  FluentUI::Vec2(30.0f, 22.0f), FluentUI::Vec2(hx + 58.0f,  top + 3.0f)))
        TileSet::instance().setTileSize(8);
    if (FluentUI::Button("16", FluentUI::Vec2(34.0f, 22.0f), FluentUI::Vec2(hx + 92.0f,  top + 3.0f)))
        TileSet::instance().setTileSize(16);
    if (FluentUI::Button("32", FluentUI::Vec2(34.0f, 22.0f), FluentUI::Vec2(hx + 130.0f, top + 3.0f)))
        TileSet::instance().setTileSize(32);

    drawAtlasPanel(top, 10.0f + listW + 12.0f, width, height);
}

// Añade un tipo nuevo (con valores por defecto) y lo deja seleccionado para configurarlo.
void TileEditor::addNewType() {
    TileTypeDef nd;            // defaults: name "Tile", cell 0, walkable true, sin encounter
    nd.name = "Nuevo";
    m_selType = TileSet::instance().addType(nd);
    std::snprintf(m_status, sizeof(m_status),
                  "Tipo añadido (#%d): renómbralo y asígnale una celda.", m_selType);
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

void TileEditor::drawPalette(float top, int /*width*/, int /*height*/) {
    auto* c = FluentUI::GetContext();
    if (!c) return;
    const FluentUI::Color text = FluentUI::Color::FromHex("#e6e8ec");
    const FluentUI::Color acc  = FluentUI::Color::FromHex("#4a9eff");

    const float px = 10.0f, py = top + 6.0f, sw = 22.0f, gap = 6.0f;
    const float mx = c->input.MouseX(), my = c->input.MouseY();
    const bool  pressed = c->input.IsMousePressed(0);

    const TileSet& ts    = TileSet::instance();
    void*          atlas = atlasHandle();   // miniaturas reales del PNG (o nullptr → color)
    const int      cols  = ts.columns(), rows = ts.rows();
    for (int i = 0; i < ts.count(); ++i) {
        const FluentUI::Vec2 pos(px, py + i * (sw + gap));
        const TileProps& p = tileProps(static_cast<TileType>(i));
        if (atlas) {
            FluentUI::Vec2 uv0, uv1;
            cellUV(ts.at(i).cell, cols, rows, uv0, uv1);
            c->renderer.DrawImage(pos, FluentUI::Vec2(sw, sw), atlas, uv0, uv1);
        } else {
            c->renderer.DrawRectFilled(pos, FluentUI::Vec2(sw, sw),
                                       FluentUI::Color(p.color.x, p.color.y, p.color.z, 1.0f), 3.0f);
        }
        if (i == m_activeType)
            c->renderer.DrawRect(FluentUI::Vec2(pos.x - 2.0f, pos.y - 2.0f),
                                 FluentUI::Vec2(sw + 4.0f, sw + 4.0f), acc, 3.0f);
        c->renderer.DrawText(FluentUI::Vec2(pos.x + sw + 8.0f, pos.y + 4.0f),
                             ts.at(i).name.c_str(), text, 13.0f);

        const bool over = mx >= pos.x && mx <= pos.x + 120.0f && my >= pos.y && my <= pos.y + sw;
        if (over && pressed) m_activeType = i;
    }
}

void TileEditor::drawCanvas(float top, int width, int height) {
    auto* c = FluentUI::GetContext();
    if (!c || m_map.width() <= 0 || m_map.height() <= 0) return;

    const float x0 = 150.0f, y0 = top + 4.0f;
    const float availW = static_cast<float>(width)  - x0 - 12.0f;
    const float availH = static_cast<float>(height) - y0 - 32.0f;   // deja sitio a la statusbar
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
void TileEditor::drawTypeList(float top, float x, float listW, int height) {
    auto* c = FluentUI::GetContext();
    if (!c) return;
    const FluentUI::Color text = FluentUI::Color::FromHex("#e6e8ec");
    const FluentUI::Color sub  = FluentUI::Color::FromHex("#9aa0a6");
    const FluentUI::Color acc  = FluentUI::Color::FromHex("#4a9eff");

    const float py = top + 6.0f, sw = 28.0f, gap = 10.0f;
    const float mx = c->input.MouseX(), my = c->input.MouseY();
    const bool  pressed = c->input.IsMousePressed(0);

    const TileSet& ts    = TileSet::instance();
    void*          atlas = atlasHandle();
    const int      cols  = ts.columns(), rows = ts.rows();
    for (int i = 0; i < ts.count(); ++i) {
        const float ry = py + i * (sw + gap);
        if (ry > height - 36.0f) break;                  // no invadir la statusbar
        const FluentUI::Vec2 pos(x + 4.0f, ry);
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

        const bool over = mx >= x && mx <= x + listW && my >= ry && my <= ry + sw;
        if (over && pressed) m_selType = i;
    }
}

// Atlas del tileset recortado en su rejilla (cols×rows). Clic en una celda = asignar
// esa celda al tipo seleccionado (m_selType). Resalta la celda ya asignada. Es la
// columna derecha de la pantalla de tileset.
void TileEditor::drawAtlasPanel(float top, float x, int width, int height) {
    auto* c = FluentUI::GetContext();
    if (!c) return;
    void*          atlas = atlasHandle();
    const TileSet& ts    = TileSet::instance();
    const int      cols  = ts.columns(), rows = ts.rows();

    // Título: qué imagen es el atlas actual + su rejilla deducida.
    char title[128];
    std::snprintf(title, sizeof(title), "Imagen: %s   (%dx%d celdas)",
                  baseName(ts.texture()).c_str(), cols, rows);
    c->renderer.DrawText(FluentUI::Vec2(x, top + 4.0f), title,
                         FluentUI::Color::FromHex("#e6e8ec"), 13.0f);

    const float y0     = top + 26.0f;   // deja sitio al título
    const float availW = static_cast<float>(width)  - x - 12.0f;
    const float availH = static_cast<float>(height) - y0 - 32.0f;
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
