// Game/OverworldMode.cpp — implementación del overworld 2D.
#include "Game/OverworldMode.h"

#include "Assets/AssetManager.h"
#include "Core/EventBus.h"
#include "Core/Log.h"
#include "Core/Project.h"
#include "Game/BattleMode.h"
#include "Game/Database.h"
#include "Game/GameEvents.h"
#include "Game/GameStack.h"
#include "Game/MenuMode.h"
#include "Game/Selection.h"
#include "Game/TileSet.h"
#include "Core/Scripting/ScriptSystem.h"
#include "Input/ActionMap.h"
#include "Input/Input.h"
#include "Renderer/Renderer.h"
#include "Renderer/Vulkan/Texture.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/SpriteRenderSystem.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

namespace pk {

void OverworldMode::onEnter(GameContext& ctx) {
    loadTileset(ctx.assets);   // atlas con textura (o color plano si no hay tileset)

    // Apariencia del jugador, TODA desde datos (player_anim.json): con clips se anima;
    // sin clips se usa esa misma hoja como sprite único; si el JSON o su imagen faltan,
    // el jugador queda sin textura (la config manda, el motor no inventa una ruta).
    if (ctx.assets) {
        m_anim = loadAnimationSet(*ctx.assets, kPlayerConfig);
        if (m_anim.valid) {
            for (const auto& [name, clip] : m_anim.clips) m_animator.addClip(name, clip);
            m_animated = true;
        }
        m_playerTex     = m_anim.sheet.texture;
        m_playerTexPath = m_anim.texturePath;
        if (!m_playerTex.valid())
            LOG_WARN("Overworld: sin sprite de jugador (revisa '%s').", kPlayerConfig);
    }

    // Vincula el MAPA (entidad-mapa con TileMapComponent) y el JUGADOR a la escena. El
    // mapa es parte de la escena (scene.json); ya no hay overworld.json como fuente.
    bindScene(ctx);

    // Provee la colisión del mapa al scripting (is_walkable / self:try_step). Lee el
    // TileMap del componente de la escena VIGENTE en cada llamada (la escena puede
    // cambiar; m_scene/m_mapEntity se mantienen al día). Sin mapa → todo transitable.
    if (ctx.scripts)
        ctx.scripts->setWalkable([this](int x, int y) {
            const TileMapComponent* c = mapComp();
            return c ? c->map.walkable(x, y) : true;
        });

    // Recarga en caliente: el editor de tiles (2ª ventana) vuelca sus cambios al
    // componente y emite MapSavedEvent; aquí solo reconstruimos los sprites del mapa.
    if (ctx.bus) {
        AssetManager* assets = ctx.assets;   // estable durante la vida del modo
        m_mapSavedSub = ctx.bus->subscribe<MapSavedEvent>(
            [this, assets](const MapSavedEvent&) {
                loadTileset(assets);   // el editor pudo cambiar la IMAGEN o el tamaño de tile
                buildTileSprites();
                LOG_INFO("Overworld: mapa reconstruido tras editar en la ventana de tiles.");
            });
        // Soltar un asset sobre el viewport: anota el drop; variableUpdate lo materializa.
        m_assetDropSub = ctx.bus->subscribe<AssetDroppedEvent>(
            [this](const AssetDroppedEvent& e) {
                m_dropPath = e.path; m_dropScreen = e.screenPos; m_hasDrop = true;
            });
        // Ídem para un .lua: aquí no se crea nada, se adjunta a la entidad bajo el cursor.
        m_scriptDropSub = ctx.bus->subscribe<ScriptDroppedEvent>(
            [this](const ScriptDroppedEvent& e) {
                m_scriptDropPath = e.path; m_scriptDropScreen = e.screenPos; m_hasScriptDrop = true;
            });
        // Alta pedida desde el menú Entidad del editor. Igual que los drops: aquí solo se
        // anota; la entidad se crea en variableUpdate, que trae la escena del frame.
        m_newEntitySub = ctx.bus->subscribe<CreateEntityEvent>(
            [this](const CreateEntityEvent& e) {
                m_newEntityKind = e.kind; m_newEntityPath = e.path; m_hasNewEntity = true;
            });
    }

    LOG_INFO("Overworld 2D listo. Vista de edición: arrastrar con el botón central/derecho = mover, "
             "rueda = zoom, F = centrar en la selección. Play (o F5) para jugar: WASD = mover, "
             "F9/F10 = guardar/cargar partida. El mapa es parte de la escena (menú Archivo).");
}

// Componente-mapa de la escena vigente (o nullptr si aún no hay / se invalidó).
TileMapComponent* OverworldMode::mapComp() {
    if (!m_scene || !m_scene->alive(m_mapEntity) || !m_scene->has<TileMapComponent>(m_mapEntity))
        return nullptr;
    return &m_scene->get<TileMapComponent>(m_mapEntity);
}

// Busca la entidad-mapa de la escena. NO la crea: una escena sin mapa es válida (proyecto
// en blanco) y el mapa se da de alta a mano desde el menú Entidad.
void OverworldMode::findMap(GameContext& ctx) {
    m_mapEntity = Entity{};
    if (!ctx.scene) return;
    Entity found{};
    ctx.scene->view<TileMapComponent>().each([&](Entity e, TileMapComponent&) { found = e; });
    m_mapEntity = found;
}

// Alta de la entidad-mapa: una grilla VACÍA del tamaño por defecto (todo del primer tipo del
// tileset). El contenido lo pinta el usuario en el editor de tiles; el motor no trae mundo.
Entity OverworldMode::spawnTileMap(GameContext& ctx) {
    if (!ctx.scene) return Entity{};
    TileMapComponent tmc;
    tmc.map.assign(kNewMapW, kNewMapH,
                   std::vector<TileType>(static_cast<size_t>(kNewMapW) * kNewMapH, kTileDefault),
                   IVec2(kNewMapW / 2, kNewMapH / 2));
    m_mapEntity = ctx.scene->createEntity();
    ctx.scene->add<NameComponent>(m_mapEntity, NameComponent{ "Map" });
    ctx.scene->add<TileMapComponent>(m_mapEntity, std::move(tmc));
    buildTileSprites();          // el mapa recién creado debe verse ya en el viewport
    return m_mapEntity;
}

CameraComponent* OverworldMode::camComp() {
    if (!m_scene || !m_scene->alive(m_cameraEntity) || !m_scene->has<CameraComponent>(m_cameraEntity))
        return nullptr;
    return &m_scene->get<CameraComponent>(m_cameraEntity);
}

// La vista tiene DOS dueños según el estado: editando manda el encuadre del editor (pan y
// zoom con el ratón, ajeno a la escena); jugando manda la entidad-cámara, que es la del
// juego y a la que camera_follow.lua sigue al jugador. Fallback al jugador si la escena
// aún no tiene entidad-cámara.
Vec2 OverworldMode::cameraCenter() const {
    if (m_editing && m_editCamValid) return m_editCamCenter;
    if (m_scene && m_scene->alive(m_cameraEntity) && m_scene->has<Transform>(m_cameraEntity))
        return m_scene->get<Transform>(m_cameraEntity).position;
    return m_lastPlayerPos + Vec2(0.5f, 0.5f);
}

float OverworldMode::cameraZoom() const {
    if (m_editing && m_editCamValid) return m_editCamZoom;
    if (m_scene && m_scene->alive(m_cameraEntity) && m_scene->has<CameraComponent>(m_cameraEntity))
        return m_scene->get<CameraComponent>(m_cameraEntity).zoom;
    return kZoom;
}

void OverworldMode::setCameraCenter(Vec2 c) {
    if (m_scene && m_scene->alive(m_cameraEntity) && m_scene->has<Transform>(m_cameraEntity))
        m_scene->get<Transform>(m_cameraEntity).position = c;
}

// Navegación de la vista en EDICIÓN: arrastrar con el botón central (o el derecho) hace
// pan, la rueda hace zoom manteniendo bajo el cursor el punto del mundo que apuntabas, y F
// centra en lo seleccionado. Solo actúa sobre el encuadre del editor: la entidad-cámara del
// juego no se toca, así que jugar sigue encuadrando como diga camera_follow.lua.
void OverworldMode::updateEditorCamera(GameContext& ctx) {
    if (!ctx.input) return;
    const Vec2 mouse   = ctx.input->mousePosition();
    const bool onScene = ctx.pointInViewport(mouse.x, mouse.y) && !ctx.uiCapturesMouse;

    // Zoom con la rueda: multiplicativo (cada muesca es un porcentaje, no un salto fijo,
    // así se siente igual de fino de cerca que de lejos).
    const float wheel = ctx.input->mouseWheel().y;
    if (onScene && wheel != 0.0f) {
        const Vec2  before = screenToWorld(mouse, ctx.screenW, ctx.screenH);
        const float factor = std::pow(1.15f, wheel);
        m_editCamZoom = std::clamp(m_editCamZoom * factor, 4.0f, 256.0f);
        // Reencuadre para que el punto bajo el cursor no se mueva: el zoom se aplica
        // "hacia" donde apuntas, no hacia el centro de la pantalla.
        const Vec2 after = screenToWorld(mouse, ctx.screenW, ctx.screenH);
        m_editCamCenter  = m_editCamCenter + (before - after);
    }

    // Pan: botón central o derecho. Se sigue arrastrando aunque el cursor salga del
    // viewport (soltar fuera no debe dejar la vista "pegada" al ratón).
    if (onScene && (ctx.input->wasMousePressed(2) || ctx.input->wasMousePressed(3))) {
        m_panning      = true;
        m_panLastMouse = mouse;
    }
    if (m_panning && !ctx.input->isMouseDown(2) && !ctx.input->isMouseDown(3)) m_panning = false;
    if (m_panning) {
        // Diferencia entre los dos puntos del mundo bajo el cursor: exacto a cualquier zoom
        // (y sin repetir la conversión píxel→mundo a mano).
        const Vec2 a = screenToWorld(m_panLastMouse, ctx.screenW, ctx.screenH);
        const Vec2 b = screenToWorld(mouse, ctx.screenW, ctx.screenH);
        m_editCamCenter = m_editCamCenter + (a - b);
        m_panLastMouse  = mouse;
    }

    // F = encuadrar. Con algo seleccionado centra en ello; si no, en el jugador. Es la vía
    // explícita para reencontrar la escena tras alejarse (el encuadre nunca se recoloca solo).
    if (ctx.input->wasKeyPressed(Key::F)) {
        Vec2 target = m_lastPlayerPos + Vec2(0.5f, 0.5f);
        if (ctx.selection && ctx.selection->has() && ctx.scene &&
            ctx.scene->alive(ctx.selection->entity) && ctx.scene->has<Transform>(ctx.selection->entity)) {
            const Transform& tr = ctx.scene->get<Transform>(ctx.selection->entity);
            target = tr.position + tr.scale * 0.5f;   // centro del bbox, no su esquina
        }
        m_editCamCenter = target;
    }
}

// Busca la entidad-cámara de la escena. Tampoco la crea: sin cámara se juega con el encuadre
// del editor (se avisa una vez) y la cámara se añade desde el menú Entidad.
void OverworldMode::findCamera(GameContext& ctx) {
    m_cameraEntity = Entity{};
    if (!ctx.scene) return;
    Entity found{};
    ctx.scene->view<CameraComponent>().each([&](Entity e, CameraComponent&) { found = e; });
    m_cameraEntity = found;
}

// Alta de la entidad-cámara del JUEGO: Transform + CameraComponent + el script que la hace
// seguir al jugador (apagar su export 'follow' la deja libre).
Entity OverworldMode::spawnCamera(GameContext& ctx, Vec2 world) {
    if (!ctx.scene) return Entity{};
    m_cameraEntity = ctx.scene->createEntity();
    ctx.scene->add<NameComponent>(m_cameraEntity, NameComponent{ "Camera" });
    ctx.scene->add<Transform>(m_cameraEntity, Transform{ world, 0.0f, Vec2(1.0f, 1.0f) });
    ctx.scene->add<CameraComponent>(m_cameraEntity, CameraComponent{ kZoom });
    ctx.scene->add<ScriptComponent>(m_cameraEntity, ScriptComponent{ "Assets/Scripts/camera_follow.lua" });
    return m_cameraEntity;
}

// (Re)vincula el mapa y el jugador al entrar al overworld o al cambiar de escena.
void OverworldMode::bindScene(GameContext& ctx) {
    m_scene     = ctx.scene;
    m_lastScene = ctx.scene;
    findMap(ctx);
    m_grid = IVec2(0, 0);
    if (const TileMapComponent* c = mapComp()) m_grid = c->map.playerStart();
    m_lastPlayerPos = Vec2(static_cast<float>(m_grid.x), static_cast<float>(m_grid.y));
    findCamera(ctx);     // la entidad-cámara de la escena (puede no haberla)
    buildTileSprites();
    bindPlayer(ctx);     // ajusta m_grid/m_lastPlayerPos si hay un jugador en la escena
    setCameraCenter(m_lastPlayerPos + Vec2(0.5f, 0.5f));   // enfoca la cámara del JUEGO en el jugador
    // Primer encuadre de la vista de EDICIÓN: el mismo. A partir de ahí es tuyo — cargar
    // otra escena o parar el juego ya no te mueve la vista; para reencuadrar está F.
    if (!m_editCamValid) {
        m_editCamCenter = m_lastPlayerPos + Vec2(0.5f, 0.5f);
        m_editCamZoom   = camComp() ? camComp()->zoom : kZoom;
        m_editCamValid  = true;
    }
}

void OverworldMode::onExit(GameContext& ctx) {
    // El callback de colisión captura este modo (this); al salir lo retiramos para no
    // dejar al ScriptSystem con un puntero colgante a un OverworldMode destruido.
    if (ctx.scripts) ctx.scripts->setWalkable(nullptr);
}

void OverworldMode::loadTileset(AssetManager* assets) {
    m_useAtlas = false;
    if (!assets) return;

    // El registro data-driven (Assets/Data/tileset.json) trae textura y tamaño de tile.
    TileSet&          ts  = TileSet::instance();
    const std::string tex = ts.texture();

    if (tex.empty()) {   // proyecto sin tileset declarado: tiles con color plano
        LOG_INFO("Sin tileset en el proyecto; los tiles usan color plano.");
        return;
    }
    const TextureHandle h = assets->loadTexture(tex, true, true, FilterMode::Pixel);  // tiles = pixel-art (nearest, sin mips)
    if (h == assets->whiteTexture()) {        // no hay tileset → color plano
        LOG_INFO("Tileset '%s' no disponible; tiles con color plano.", tex.c_str());
        return;
    }
    m_tilesetTex = h;
    // columns/rows se deducen del tamaño real del PNG y el tamaño de tile.
    if (const Texture* t = assets->getTexture(h))
        ts.resolveGrid(static_cast<int>(t->width()), static_cast<int>(t->height()));
    m_atlasCols  = ts.columns();
    m_atlasRows  = ts.rows();
    m_useAtlas   = true;
    LOG_INFO("Tileset cargado: '%s' (%dx%d).", tex.c_str(), m_atlasCols, m_atlasRows);
}

void OverworldMode::buildTileSprites() {
    m_tileSprites.clear();
    const TileMapComponent* c = mapComp();
    if (!c) return;
    const TileMap& map = c->map;
    m_tileSprites.reserve(static_cast<size_t>(map.width()) * map.height());
    const SpriteSheet sheet{ m_tilesetTex, m_atlasCols, m_atlasRows };
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            const TileProps& props = tileProps(map.at(x, y));
            const auto it = c->overrides.find(y * map.width() + x);

            Sprite s;
            if (it != c->overrides.end()) {              // tile con transform editado
                s.position = it->second.position;
                s.size     = it->second.scale;
                s.rotation = it->second.rotationDeg * 0.01745329f;
                s.layer    = it->second.layer;
            } else {
                s.position = Vec2(static_cast<float>(x), static_cast<float>(y));
                s.size     = Vec2(1.0f, 1.0f);
            }
            const Vec4 tint = (it != c->overrides.end()) ? it->second.tint
                                                         : Vec4(1.0f, 1.0f, 1.0f, 1.0f);

            // Apariencia: tileset (textura + uvRect) o color plano (fallback).
            if (m_useAtlas) {
                s.texture = m_tilesetTex;
                s.uvRect  = sheet.uvForFrame(props.atlasCell);
                s.color   = tint;                 // textura tal cual (tinte blanco) o tintada
            } else {
                s.color   = props.color * tint;   // color del tipo de tile
            }
            m_tileSprites.push_back(s);
        }
    }
}

// Vincula el jugador que HAYA en la escena (y repara lo que no se serializa: su hoja, el
// script de movimiento y el estado de grid). Si no hay ninguno, no pasa nada: se juega sin
// jugador hasta que se dé de alta desde el menú Entidad.
void OverworldMode::bindPlayer(GameContext& ctx) {
    m_playerEntity = Entity{};
    if (!ctx.scene) return;
    Entity found{};
    ctx.scene->view<PlayerTag>().each([&found](Entity e, PlayerTag&) { found = e; });
    if (found.valid()) {
        m_playerEntity = found;
        if (ctx.scene->has<Transform>(found)) {
            const Vec2 p = ctx.scene->get<Transform>(found).position;
            m_grid = IVec2(static_cast<int>(std::round(p.x)),
                           static_cast<int>(std::round(p.y)));
            m_lastPlayerPos = Vec2(static_cast<float>(m_grid.x), static_cast<float>(m_grid.y));
        }
        // Apariencia del jugador EN EDICIÓN. Antes la ponía la presentación de cada frame
        // (que ahora solo corre jugando), así que una escena cuyo jugador no traiga la
        // textura resuelta se veía como un cuadro blanco con el juego parado. Se siembra
        // desde los datos (kPlayerConfig) sin pisar lo que el componente ya tenga.
        if (!ctx.scene->has<SpriteComponent>(found)) {
            if (m_playerTex.valid()) {
                SpriteComponent sp;
                sp.tex         = m_playerTex;
                sp.texturePath = m_playerTexPath;
                if (m_animated) sp.uvRect = m_anim.sheet.uvForFrame(0);
                ctx.scene->add<SpriteComponent>(found, sp);
            }
        } else {
            SpriteComponent& sp = ctx.scene->get<SpriteComponent>(found);
            // Sin ruta no hay nada que resolver al cargar (el SceneManager la salta): es el
            // caso de las escenas guardadas antes de que el jugador serializara su sprite.
            if (sp.texturePath.empty() && !m_playerTexPath.empty()) sp.texturePath = m_playerTexPath;
            if (!sp.tex.valid()          && m_playerTex.valid())    sp.tex         = m_playerTex;
            // Con hoja de varios frames, el rect completo mostraría la hoja entera: en
            // edición enseñamos el primer frame (el reposo de la convención del JSON).
            if (m_animated) sp.uvRect = m_anim.sheet.uvForFrame(0);
        }
        // El jugador se mueve desde Lua (Fase B): si la escena cargada no le trae un
        // script, le damos el de movimiento por casillas.
        if (!ctx.scene->has<ScriptComponent>(found))
            ctx.scene->add<ScriptComponent>(found, ScriptComponent{ "Assets/Scripts/player_movement.lua" });
        // Estado de grid sembrado en su celda actual. No se serializa, así que una
        // escena cargada nunca lo trae; lo (re)sembramos para que el GridMover gobierne
        // su Transform desde ya (y el offset visual no se realimente).
        GridMover gm; gm.cell = gm.from = m_grid; gm.seeded = true;
        if (ctx.scene->has<GridMover>(found)) ctx.scene->get<GridMover>(found) = gm;
        else                                  ctx.scene->add<GridMover>(found, gm);
    }
}

// Alta del JUGADOR: PlayerTag + Transform en `cell` + su hoja (kPlayerConfig) + el script de
// movimiento por casillas + el GridMover ya sembrado en esa celda.
Entity OverworldMode::spawnPlayer(GameContext& ctx, IVec2 cell) {
    if (!ctx.scene) return Entity{};
    m_playerEntity = ctx.scene->createEntity();
    ctx.scene->add<PlayerTag>(m_playerEntity, PlayerTag{});
    Transform tr;
    tr.position = Vec2(static_cast<float>(cell.x), static_cast<float>(cell.y));  // arranca en playerStart
    ctx.scene->add<Transform>(m_playerEntity, tr);
    SpriteComponent sp;
    sp.tex         = m_playerTex;       // animado o no, es la hoja de kPlayerConfig
    sp.texturePath = m_playerTexPath;   // sin ruta, la escena guardada perdería su sprite
    if (m_animated) sp.uvRect = m_anim.sheet.uvForFrame(0);   // primer frame como pose de editor
    // Proyecto sin arte de jugador (no hay player_anim.json): se crea igualmente, con la
    // textura blanca como placeholder visible. La imagen se asigna luego en el inspector.
    if (!sp.tex.valid() && ctx.assets) {
        sp.tex = ctx.assets->whiteTexture();
        LOG_WARN("El jugador se creó sin imagen: asígnale una en el inspector (Apariencia).");
    }
    ctx.scene->add<SpriteComponent>(m_playerEntity, sp);
    ctx.scene->add<NameComponent>(m_playerEntity, NameComponent{ "Player" });
    // Su control de movimiento vive en Lua (input + try_step con colisión de grid).
    ctx.scene->add<ScriptComponent>(m_playerEntity, ScriptComponent{ "Assets/Scripts/player_movement.lua" });
    // Estado de grid sembrado en la celda de inicio: el GridMover gobierna su Transform.
    GridMover gm; gm.cell = gm.from = cell; gm.seeded = true;
    ctx.scene->add<GridMover>(m_playerEntity, gm);
    m_grid          = cell;
    m_lastPlayerPos = tr.position;
    return m_playerEntity;
}

Entity OverworldMode::spawnSpriteEntity(GameContext& ctx, const std::string& path, Vec2 world) {
    if (!ctx.scene || !ctx.assets) return Entity{};
    const TextureHandle tex = ctx.assets->loadTexture(path);

    // Escala inicial: 1 tile de ALTO y el ancho según el ASPECT RATIO de la textura.
    // Así el sprite se ve a un tamaño consistente (no diminuto si la textura es de
    // pocos píxeles) y sin deformarse. Su tamaño real se afina luego en el inspector.
    Vec2 scale(1.0f, 1.0f);
    if (Texture* t = ctx.assets->getTexture(tex); t && t->valid() && t->height() > 0)
        scale = Vec2(static_cast<float>(t->width()) / static_cast<float>(t->height()), 1.0f);

    Entity e = ctx.scene->createEntity();
    ctx.scene->add<Transform>(e, Transform{ world, 0.0f, scale });
    SpriteComponent sp;
    sp.texturePath = path;
    sp.tex         = tex;
    sp.layer       = 1;
    ctx.scene->add<SpriteComponent>(e, sp);

    std::string name = path;                                  // nombre = archivo sin ruta/ext
    if (size_t s = name.find_last_of("/\\"); s != std::string::npos) name = name.substr(s + 1);
    if (size_t d = name.find_last_of('.');   d != std::string::npos) name = name.substr(0, d);
    ctx.scene->add<NameComponent>(e, NameComponent{ name });
    LOG_INFO("Objeto '%s' creado en (%.0f, %.0f).", name.c_str(), world.x, world.y);
    return e;
}

// Celda del mundo en el centro de la vista actual: es donde el menú Entidad deja lo que
// crea, para que aparezca siempre a la vista (mires donde mires y con cualquier zoom).
Vec2 OverworldMode::viewCenterCell() const {
    const Vec2 c = cameraCenter();
    return Vec2(std::floor(c.x), std::floor(c.y));
}

// Materializa una petición del menú Entidad. Mapa, cámara y jugador son SINGLETONS de la
// escena (los ensure*/camComp usan el primero que encuentran): si ya hay uno no se duplica,
// se selecciona el existente y se avisa.
void OverworldMode::createEntityOfKind(GameContext& ctx, NewEntityKind kind, const std::string& path) {
    if (!ctx.scene) return;
    const Vec2 at = viewCenterCell();
    Entity     created{};
    bool       existed = false;

    switch (kind) {
        case NewEntityKind::Empty: {
            created = ctx.scene->createEntity();
            ctx.scene->add<NameComponent>(created, NameComponent{ "Entidad" });
            ctx.scene->add<Transform>(created, Transform{ at, 0.0f, Vec2(1.0f, 1.0f) });
            LOG_INFO("Entidad vacía creada en (%.0f, %.0f).", at.x, at.y);
            break;
        }
        case NewEntityKind::Sprite: {
            if (path.empty()) { LOG_WARN("Crear sprite: no se eligió ninguna imagen."); return; }
            created = spawnSpriteEntity(ctx, path, at);
            break;
        }
        case NewEntityKind::Camera: {
            ctx.scene->view<CameraComponent>().each([&](Entity e, CameraComponent&) { created = e; });
            if (created.valid()) { existed = true; m_cameraEntity = created; break; }
            created = spawnCamera(ctx, at + Vec2(0.5f, 0.5f));   // centro de la celda
            LOG_INFO("Cámara creada en (%.1f, %.1f).", at.x + 0.5f, at.y + 0.5f);
            break;
        }
        case NewEntityKind::TileMap: {
            ctx.scene->view<TileMapComponent>().each([&](Entity e, TileMapComponent&) { created = e; });
            if (created.valid()) { existed = true; m_mapEntity = created; break; }
            created = spawnTileMap(ctx);
            LOG_INFO("Mapa de tiles creado (menú Ventana → Editor de tiles para dibujarlo).");
            break;
        }
        case NewEntityKind::Player: {
            ctx.scene->view<PlayerTag>().each([&](Entity e, PlayerTag&) { created = e; });
            if (created.valid()) { existed = true; m_playerEntity = created; break; }
            created = spawnPlayer(ctx, IVec2(static_cast<int>(at.x), static_cast<int>(at.y)));
            LOG_INFO("Jugador creado en (%.0f, %.0f).", at.x, at.y);
            break;
        }
    }

    if (!created.valid()) return;
    if (existed)
        LOG_WARN("La escena ya tenía esa entidad (es única): se ha seleccionado la existente.");
    if (ctx.selection) ctx.selection->entity = created;   // el inspector la muestra ya
    if (!existed && ctx.bus) ctx.bus->emit(SceneEditedEvent{});   // marca "cambios sin guardar"
}

Vec2 OverworldMode::screenToWorld(Vec2 screen, int screenW, int screenH) const {
    const float u = screenW > 0 ? screen.x / static_cast<float>(screenW) : 0.0f;
    const float v = screenH > 0 ? screen.y / static_cast<float>(screenH) : 0.0f;
    const float zoom  = cameraZoom();
    const float halfW = 240.0f / zoom;        // 240×135 = mitad del target lowRes (480×270)
    const float halfH = 135.0f / zoom;
    const Vec2  center = cameraCenter();
    return Vec2(center.x + (2.0f * u - 1.0f) * halfW,
                center.y + (2.0f * v - 1.0f) * halfH);
}

// Inversa exacta de screenToWorld: dónde cae un punto del mundo en la pantalla. La usa el
// editor para anclar el inspector flotante a la entidad seleccionada.
Vec2 OverworldMode::worldToScreen(Vec2 world, int screenW, int screenH) const {
    const float zoom  = cameraZoom();
    const float halfW = 240.0f / zoom;        // 240×135 = mitad del target lowRes (480×270)
    const float halfH = 135.0f / zoom;
    const Vec2  center = cameraCenter();
    const float u = halfW > 0.0f ? ((world.x - center.x) / halfW + 1.0f) * 0.5f : 0.0f;
    const float v = halfH > 0.0f ? ((world.y - center.y) / halfH + 1.0f) * 0.5f : 0.0f;
    return Vec2(u * static_cast<float>(screenW), v * static_cast<float>(screenH));
}

// Celda de enfrente según la dirección de mirada (0 abajo, 1 arriba, 2 izq, 3 der).
static IVec2 facingDelta(int facing) {
    switch (facing) {
        case 1:  return IVec2(0, -1);   // arriba
        case 2:  return IVec2(-1, 0);   // izquierda
        case 3:  return IVec2(1, 0);    // derecha
        default: return IVec2(0, 1);    // abajo
    }
}

void OverworldMode::handleInput(GameContext& ctx) {
    if (!ctx.input) return;

    // Avanza la secuencia de diálogos: si el diálogo en curso se cerró y quedan más,
    // empuja el siguiente. Mientras la secuencia siga activa, no procesamos el resto.
    m_events.pump(ctx);
    if (m_events.active()) return;

    // Abrir el menú del juego (acción Menu = Tab): empuja un MenuMode encima. Se hace en
    // handleInput (no en update) para no modificar la pila mientras el GameStack la itera.
    if (ctx.actions && ctx.actions->wasTriggered(*ctx.input, Action::Menu)) {
        if (ctx.stack) ctx.stack->push(std::make_unique<MenuMode>(), ctx);
        return;
    }

    // Interactuar (Confirm) con un NPC/cartel de FRENTE. Dos caminos, en este orden:
    //  1) Si la entidad de enfrente tiene un ScriptComponent con on_interact, se corre como
    //     CORRUTINA de evento (puede encadenar show_text/show_choice/wait y ramificar).
    //  2) Si no, fallback al texto estático del DialogueComponent (vía EventRunner).
    if (ctx.actions && ctx.scene && ctx.actions->wasTriggered(*ctx.input, Action::Confirm)) {
        const IVec2 front = m_grid + facingDelta(m_facing);

        // 1) Evento por script: primera entidad de enfrente con on_interact.
        if (ctx.scripts) {
            Entity hit{};
            ctx.scene->view<Transform, ScriptComponent>().each(
                [&](Entity e, Transform& tr, ScriptComponent&) {
                    if (hit.valid()) return;
                    const IVec2 c(static_cast<int>(std::lround(tr.position.x)),
                                  static_cast<int>(std::lround(tr.position.y)));
                    if (c == front && ctx.scripts->hasFunction(e, "on_interact")) hit = e;
                });
            if (hit.valid()) { ctx.scripts->runEvent(*ctx.scene, hit, "on_interact"); return; }
        }

        // 2) Fallback: texto estático del DialogueComponent.
        std::string text;
        bool found = false;
        ctx.scene->view<Transform, DialogueComponent>().each(
            [&](Entity, Transform& tr, DialogueComponent& dc) {
                if (found) return;
                const IVec2 c(static_cast<int>(std::lround(tr.position.x)),
                              static_cast<int>(std::lround(tr.position.y)));
                if (c == front) { text = dc.text; found = true; }
            });
        if (found) { m_events.showTexts({ text }, ctx); return; }
    }

    // Guardar/cargar PARTIDA del jugador (posición + pasos). El MAPA es parte de la
    // escena (menú Archivo → Guardar/Abrir), ya no se guarda aparte con F6/F7. Movidas a
    // F9/F10: F5 y F6 son ahora Play/Stop y Pausa del editor.
    if (ctx.input->wasKeyPressed(Key::F9))  save(ctx);
    if (ctx.input->wasKeyPressed(Key::F10)) load(ctx);
}

void OverworldMode::variableUpdate(GameContext& ctx, float dt) {
    // Estado de ejecución del frame. Va lo PRIMERO porque decide qué cámara manda, y de la
    // cámara dependen todas las conversiones pantalla↔mundo que vienen a continuación
    // (drops, picking, gizmo).
    m_editing = (ctx.mode != RunMode::Play);

    // Materializa un drop de asset pendiente: crea la entidad en la celda soltada.
    if (m_hasDrop) {
        m_hasDrop = false;
        const Vec2 wpos = screenToWorld(m_dropScreen, ctx.screenW, ctx.screenH);
        spawnSpriteEntity(ctx, m_dropPath, Vec2(std::floor(wpos.x), std::floor(wpos.y)));
    }

    // Ídem para un .lua soltado: se adjunta a la entidad que haya bajo el punto (no crea
    // ninguna). Reasignar solo cambia la ruta; el ScriptSystem re-instancia por su cuenta.
    if (m_hasScriptDrop) {
        m_hasScriptDrop = false;
        const Vec2   wpos = screenToWorld(m_scriptDropScreen, ctx.screenW, ctx.screenH);
        const Entity hit  = entityAt(ctx, wpos);
        if (!hit.valid()) {
            LOG_WARN("No hay ninguna entidad bajo el cursor: el script '%s' no se adjuntó.",
                     m_scriptDropPath.c_str());
        } else if (ctx.scene) {
            if (ctx.scene->has<ScriptComponent>(hit))
                ctx.scene->get<ScriptComponent>(hit).path = m_scriptDropPath;
            else
                ctx.scene->add<ScriptComponent>(hit, ScriptComponent{ m_scriptDropPath });
            if (ctx.selection) ctx.selection->entity = hit;   // el inspector lo muestra ya
            LOG_INFO("Script '%s' adjuntado a la entidad %u.", m_scriptDropPath.c_str(), hit.id);
        }
    }

    // Alta pedida desde el menú Entidad del editor. Va aquí, con los drops, por la misma
    // razón: el evento llega mientras el editor dibuja y la escena solo se toca en el update.
    if (m_hasNewEntity) {
        m_hasNewEntity = false;
        createEntityOfKind(ctx, m_newEntityKind, m_newEntityPath);
        m_newEntityPath.clear();
    }

    m_scene = ctx.scene;   // escena vigente (la usa mapComp / el callback de colisión)

    // Re-vincula mapa + jugador si cambió la escena (cargar/nueva): comparamos el PUNTERO
    // de la escena, no alive() — los ids/generación se reinician por escena y un Entity
    // viejo puede "coincidir" por azar con otra entidad de la escena nueva.
    if (ctx.scene && ctx.scene != m_lastScene) {
        bindScene(ctx);                      // re-resuelve la entidad-mapa + reconstruye sprites + jugador
    } else if (ctx.scene && !ctx.scene->alive(m_playerEntity)) {
        bindPlayer(ctx);                     // el jugador cambió/desapareció en la misma escena
    }

    // La entidad-mapa se pudo BORRAR desde el editor: sus sprites son una copia construida
    // aparte, así que sin mapa hay que vaciarlos o el mundo seguiría dibujándose sin dueño.
    if (!mapComp() && !m_tileSprites.empty()) buildTileSprites();

    // A partir de aquí empieza la SIMULACIÓN. En edición (o en pausa) no corre nada de
    // esto: ni scripts, ni pasos, ni encuentros, ni la presentación que reescribe el
    // Transform del jugador — de lo contrario sería imposible colocar nada en el mundo,
    // porque su script le reasigna la posición cada frame. Lo que sigue vivo en edición es
    // el picking (más abajo), los drops y el render de lo que haya en la escena.
    const bool playing = !m_editing;

    // Jugar sin entidad-cámara es válido (se usa el encuadre del editor), pero conviene
    // saberlo: en el juego final nadie seguiría al jugador. Se avisa una vez por partida.
    if (playing && !camComp() && !m_warnedNoCamera) {
        LOG_WARN("La escena no tiene cámara: se juega con el encuadre del editor. "
                 "Añade una entidad con CameraComponent para que la vista siga al jugador.");
        m_warnedNoCamera = true;
    }
    if (!playing) m_warnedNoCamera = false;

    // Corre los scripts del mundo (incluido el movimiento del jugador vía try_step) como
    // parte de la simulación del overworld. Al estar aquí, un overlay que congele este
    // modo (MenuMode/DialogueMode) pausa también los scripts: el jugador deja de moverse.
    // Mientras una CORRUTINA de evento esté activa (p.ej. durante un wait() sin UI en la
    // pila), también congelamos el on_update del mundo: el jugador no se mueve a mitad de
    // un evento aunque ese instante no haya un DialogueMode encima.
    if (playing && ctx.scripts && ctx.scene && !ctx.scripts->eventsActive())
        ctx.scripts->update(*ctx.scene, dt);
    else if (!playing && ctx.scripts && ctx.scene)
        // Editando: los .lua se cargan igual (hot-reload) para que el inspector siga
        // mostrando sus `exports` y sus errores, pero NADA se ejecuta ni se mueve.
        ctx.scripts->refresh(*ctx.scene);

    // El MOVIMIENTO del jugador lo dicta su script (player_movement.lua), que ya corrió
    // este frame y dejó la celda lógica en su Transform (vía GridMover). Aquí solo lo
    // LEEMOS para presentar; el script no sabe de cámara/animación/encuentros.
    Vec2 playerPos = m_lastPlayerPos;
    if (ctx.scene && ctx.scene->alive(m_playerEntity) && ctx.scene->has<Transform>(m_playerEntity))
        playerPos = ctx.scene->get<Transform>(m_playerEntity).position;

    // Dirección + caminar inferidos del DELTA de posición respecto al frame anterior
    // (sin que el script reporte nada). Quieto → idle conservando la última dirección.
    const Vec2 delta  = playerPos - m_lastPlayerPos;
    const bool moving = playing && glm::dot(delta, delta) > 1e-6f;
    if (moving) {
        if (std::fabs(delta.x) >= std::fabs(delta.y)) m_facing = (delta.x > 0.0f) ? 3 : 2;
        else                                          m_facing = (delta.y > 0.0f) ? 0 : 1;
    }

    // Encuentros: al ASENTARSE en una celda nueva (terminó el deslizamiento). No se
    // disparan a mitad de paso, para que el combate empiece con el jugador alineado.
    const IVec2 cell(static_cast<int>(std::lround(playerPos.x)),
                     static_cast<int>(std::lround(playerPos.y)));
    if (playing) {
        if (!moving && cell != m_grid) {
            m_grid = cell;
            onArrive(ctx);
        }
    } else {
        // En edición seguimos al jugador allá donde lo dejes (arrastrarlo, cargar otra
        // escena) SIN interpretarlo como pasos: al pulsar Play no debe saltar un encuentro
        // por el "movimiento" que en realidad hiciste tú editando.
        m_grid = cell;
    }

    m_lastPlayerPos = playerPos;

    // La cámara YA NO se mueve aquí: es una entidad del ECS (m_cameraEntity) y su
    // seguimiento del jugador lo gobierna un script (camera_follow.lua, vía player_pos()),
    // que ya corrió en ctx.scripts->update arriba. Así en editor se puede mover libre
    // (apagando su export 'follow') y en play la maneja el script.

    // Navegación de la vista (pan/zoom/F) y selección por picking: ambas son del editor y
    // solo tienen sentido con el juego parado.
    if (!playing) updateEditorCamera(ctx);
    // El gizmo se queda con el clic cuando se agarra uno de sus ejes: si no, arrastrarlo
    // seleccionaría lo que hubiera debajo a mitad de movimiento.
    if (!handleGizmoDrag(ctx)) handlePicking(ctx);

    // Todo lo que sigue es PRESENTACIÓN del jugador y solo tiene sentido jugando: en
    // edición el Transform y el sprite son los que tú dejes (es lo que permitirá moverlo
    // con el gizmo), no los que imponga el modo.
    if (!playing) return;

    // Clip de animación según estado (idle/walk × dirección) y avance por tiempo.
    const char* dir = (m_facing == 1) ? "up"
                    : (m_facing == 2) ? "left"
                    : (m_facing == 3) ? "right" : "down";
    m_animator.play(std::string(moving ? "walk_" : "idle_") + dir);
    m_animator.update(dt);

    // Saltito (bob) al caminar: del progreso del paso (gm.t) del GridMover del jugador,
    // no de la posición leída, así no se realimenta con el offset visual.
    float bob = 0.0f;
    if (ctx.scene && ctx.scene->alive(m_playerEntity) && ctx.scene->has<GridMover>(m_playerEntity)) {
        const GridMover& gm = ctx.scene->get<GridMover>(m_playerEntity);
        if (gm.moving) bob = std::sin(gm.t * 6.2831853f) * 0.08f;
    }

    // El jugador (entidad del ECS): el script gobierna su POSICIÓN (Transform); aquí
    // solo le aplicamos la PRESENTACIÓN encima (offset visual del sprite alto, saltito,
    // tinte, capa, frame de animación). Corremos DESPUÉS del script —que reescribe la
    // celda lógica cada frame— así que el offset no se realimenta.
    if (ctx.scene && ctx.scene->alive(m_playerEntity)) {
        Transform& tr = ctx.scene->get<Transform>(m_playerEntity);
        tr.position = Vec2(playerPos.x, playerPos.y - 0.25f - bob);   // sube el sprite (alto 1.25) + bob
        tr.rotation = m_playerRotDeg * 0.01745329f;
        tr.scale    = Vec2(1.0f * m_playerScale.x, 1.25f * m_playerScale.y);
        if (ctx.scene->has<SpriteComponent>(m_playerEntity)) {
            SpriteComponent& sp = ctx.scene->get<SpriteComponent>(m_playerEntity);
            sp.tint  = m_playerTint;
            sp.layer = m_playerLayer;
            if (m_animated) {
                // Con clips manda la hoja: el frame define textura y UV cada frame.
                sp.tex    = m_anim.sheet.texture;
                sp.uvRect = m_anim.sheet.uvForFrame(m_animator.frame());
            } else {
                // Sin animación NO pisamos la textura: la que traiga el componente (de la
                // escena o asignada en el editor) es la buena; solo normalizamos la UV.
                sp.uvRect = Vec4(0.0f, 0.0f, 1.0f, 1.0f);
            }
        }
    }

}

// Emisión al renderer (separada de la lógica): cámara + tiles + gizmo. El GameStack la
// llama aunque el modo esté pausado bajo un overlay (menú), así el overworld se sigue
// viendo "congelado" detrás. El jugador-entidad lo dibuja el render-feed del Engine.
void OverworldMode::render(GameContext& ctx) {
    if (!ctx.renderer) return;
    m_editing = (ctx.mode != RunMode::Play);   // el GameStack puede llamar render sin update
    m_cam.setViewport(480.0f, 270.0f);
    m_cam.setZoom(cameraZoom());
    m_cam.setCenter(cameraCenter());
    std::vector<Sprite> sprites = m_tileSprites;
    appendSelectionGizmo(ctx, sprites);   // recuadro de selección (gizmo)
    ctx.renderer->setSprites(sprites);

    // Render-feed del ECS: entidades (jugador, objetos) sobre los tiles. Va AQUÍ y no en
    // el Engine para que en un modo que tape el overworld (combate) no se dibujen.
    if (ctx.scene) {
        std::vector<Sprite> entities;
        collectEntitySprites(*ctx.scene, entities);
        ctx.renderer->appendSprites(entities);
    }
    ctx.renderer->set2DCamera(m_cam.viewProjection());

    // Dónde se ve la entidad seleccionada: el editor ancla ahí su inspector flotante. Se
    // publica al renderizar porque es aquí donde la cámara de este frame ya está resuelta.
    if (ctx.selection) {
        ctx.selection->screenValid = false;
        if (ctx.scene && ctx.selection->has() && ctx.scene->alive(ctx.selection->entity) &&
            ctx.scene->has<Transform>(ctx.selection->entity)) {
            // El CENTRO, no la esquina: es donde se dibuja el gizmo, y es lo que el editor
            // usa para apartar el inspector. Con la esquina, el hueco se quedaba corto por
            // media entidad y el panel acababa encima de las flechas.
            const Transform& t2 = ctx.scene->get<Transform>(ctx.selection->entity);
            const Vec2 w(t2.position.x + t2.scale.x * 0.5f, t2.position.y + t2.scale.y * 0.5f);
            const Vec2 s = worldToScreen(w, ctx.screenW, ctx.screenH);
            ctx.selection->screenX = s.x;
            ctx.selection->screenY = s.y;
            // Media entidad + lo que el gizmo se extiende (ejes 46 px + punta 11, o el anillo
            // de 40 + su tirador): es la distancia mínima a la que algo puede ponerse al lado
            // sin taparlo.
            // OJO con las escalas: cameraZoom() son píxeles del TARGET interno (480×270) por
            // unidad, mientras que screenX/Y ya vienen en píxeles de ventana. El radio se
            // convierte a esos mismos píxeles o el editor lo interpreta 5 veces más pequeño.
            const float zoomPx  = cameraZoom();
            const float perUnit = (ctx.screenW > 0) ? static_cast<float>(ctx.screenW) * zoomPx / 480.0f
                                                    : zoomPx;
            const float halfU   = std::max(t2.scale.x, t2.scale.y) * 0.5f;   // media entidad
            const float gizmoU  = 57.0f / std::max(1.0f, zoomPx);            // ejes 46 + punta 11
            ctx.selection->screenRadius = (halfU + gizmoU) * perUnit;
            ctx.selection->screenValid = true;
        }
    }
}

// Geometría del gizmo, EN UNIDADES DE MUNDO y con las mismas medidas que el dibujo
// (appendSelectionGizmo): ejes de 46 px y tiradores de 11 px, convertidos con el zoom.
OverworldMode::GizmoPart OverworldMode::gizmoPartAt(GameContext& ctx, Vec2 world, Vec2 c) const {
    const float px   = 1.0f / std::max(1.0f, cameraZoom());
    const float len  = 46.0f * px;
    const float head = 11.0f * px;
    const float tol  = 9.0f  * px;    // margen de agarre, generoso a cualquier zoom

    const float dx = world.x - c.x, dy = world.y - c.y;

    if (ctx.gizmo == GizmoTool::Rotate) {
        const float r = 40.0f * px;
        const float d = std::sqrt(dx * dx + dy * dy);
        return (std::fabs(d - r) <= tol) ? GizmoPart::Ring : GizmoPart::None;
    }
    // Cuadro central (mueve/escala en los dos ejes a la vez).
    if (std::fabs(dx - head * 0.7f) <= head * 0.7f && std::fabs(dy + head * 0.7f) <= head * 0.7f)
        return GizmoPart::Both;
    if (std::fabs(dx) <= head * 0.6f && std::fabs(dy) <= head * 0.6f) return GizmoPart::Both;
    // Ejes: dentro del largo y cerca de la línea.
    if (dx >= -tol && dx <= len + head && std::fabs(dy) <= tol) return GizmoPart::AxisX;
    if (dy <= tol && dy >= -(len + head) && std::fabs(dx) <= tol) return GizmoPart::AxisY;
    return GizmoPart::None;
}

// Arrastre del gizmo. El resultado se calcula siempre contra el estado INICIAL (posición del
// ratón y Transform al pulsar): así el objeto sigue al cursor sin deriva por acumulación.
bool OverworldMode::handleGizmoDrag(GameContext& ctx) {
    if (!m_editing || !ctx.input || !ctx.scene || !ctx.selection) return false;
    if (ctx.gizmo == GizmoTool::Select) { m_gizmoDrag = GizmoPart::None; return false; }
    if (!ctx.selection->has() || !ctx.scene->alive(ctx.selection->entity) ||
        !ctx.scene->has<Transform>(ctx.selection->entity)) {
        m_gizmoDrag = GizmoPart::None;
        return false;
    }
    Transform& tr = ctx.scene->get<Transform>(ctx.selection->entity);
    const Vec2 center(tr.position.x + tr.scale.x * 0.5f, tr.position.y + tr.scale.y * 0.5f);
    const Vec2 mouse = ctx.input->mousePosition();
    const Vec2 world = screenToWorld(mouse, ctx.screenW, ctx.screenH);

    // Soltar: fin del arrastre (y aviso al editor, que es quien marca "sin guardar").
    if (m_gizmoDrag != GizmoPart::None && !ctx.input->isMouseDown(1)) {
        m_gizmoDrag = GizmoPart::None;
        if (ctx.bus) ctx.bus->emit(SceneEditedEvent{});
        return true;   // este clic era del gizmo, no del picking
    }

    // Empezar: solo si el cursor está sobre una parte del gizmo y dentro del viewport.
    if (m_gizmoDrag == GizmoPart::None) {
        if (!ctx.input->wasMousePressed(1)) return false;
        if (ctx.uiCapturesMouse || !ctx.pointInViewport(mouse.x, mouse.y)) return false;
        const GizmoPart part = gizmoPartAt(ctx, world, center);
        if (part == GizmoPart::None) return false;
        m_gizmoDrag       = part;
        m_gizmoGrabWorld  = world;
        m_gizmoStartPos   = tr.position;
        m_gizmoStartScale = tr.scale;
        m_gizmoStartRot   = tr.rotation;
        m_gizmoStartAngle = std::atan2(world.y - center.y, world.x - center.x);
        return true;
    }

    const float dx = world.x - m_gizmoGrabWorld.x;
    const float dy = world.y - m_gizmoGrabWorld.y;
    switch (ctx.gizmo) {
    case GizmoTool::Move:
        if (m_gizmoDrag == GizmoPart::AxisX || m_gizmoDrag == GizmoPart::Both)
            tr.position.x = m_gizmoStartPos.x + dx;
        if (m_gizmoDrag == GizmoPart::AxisY || m_gizmoDrag == GizmoPart::Both)
            tr.position.y = m_gizmoStartPos.y + dy;
        break;
    case GizmoTool::Rotate: {
        const float ang = std::atan2(world.y - center.y, world.x - center.x);
        tr.rotation = m_gizmoStartRot + (ang - m_gizmoStartAngle);
        break;
    }
    case GizmoTool::Scale: {
        // El factor sale de cuánto se aleja el cursor del centro respecto a donde se agarró.
        const float grabX = m_gizmoGrabWorld.x - center.x, grabY = m_gizmoGrabWorld.y - center.y;
        const float curX  = world.x - center.x,            curY  = world.y - center.y;
        auto ratio = [](float cur, float grab) {
            if (std::fabs(grab) < 0.0001f) return 1.0f;
            return std::max(0.05f, cur / grab);
        };
        if (m_gizmoDrag == GizmoPart::AxisX) tr.scale.x = m_gizmoStartScale.x * ratio(curX, grabX);
        else if (m_gizmoDrag == GizmoPart::AxisY) tr.scale.y = m_gizmoStartScale.y * ratio(curY, grabY);
        else {   // uniforme, por la distancia al centro
            const float g = std::sqrt(grabX * grabX + grabY * grabY);
            const float cu = std::sqrt(curX * curX + curY * curY);
            const float f = (g < 0.0001f) ? 1.0f : std::max(0.05f, cu / g);
            tr.scale.x = m_gizmoStartScale.x * f;
            tr.scale.y = m_gizmoStartScale.y * f;
        }
        break;
    }
    default: break;
    }
    return true;
}

void OverworldMode::handlePicking(GameContext& ctx) {
    if (!ctx.selection || !ctx.input || !ctx.scene) return;
    if (ctx.uiCapturesMouse) return;                 // el ratón está sobre la UI
    if (!ctx.input->wasMousePressed(1)) return;      // 1 = botón izquierdo
    // Y además el clic tiene que haber caído DENTRO del viewport. Sin esto, un clic rápido
    // sobre la toolbar o un panel (antes de que la UI publique su hover) se interpretaba
    // como "click en el vacío de la escena" y BORRABA la selección.
    const Vec2 mouse = ctx.input->mousePosition();
    if (!ctx.pointInViewport(mouse.x, mouse.y)) return;

    const Entity hit = entityAt(ctx, screenToWorld(ctx.input->mousePosition(),
                                                   ctx.screenW, ctx.screenH));
    if (hit.valid()) ctx.selection->entity = hit;
    else             ctx.selection->clear();
}

// Entidad bajo un punto del MUNDO: la de mayor capa cuyo bbox (Transform.position..+scale)
// lo contiene. La comparten el picking del click y el drop de un script en el viewport.
Entity OverworldMode::entityAt(GameContext& ctx, Vec2 world) const {
    if (!ctx.scene) return Entity{};
    Entity hit{};
    int    bestLayer = -1;
    ctx.scene->view<Transform, SpriteComponent>().each(
        [&](Entity e, Transform& tr, SpriteComponent& sp) {
            if (world.x >= tr.position.x && world.x <= tr.position.x + tr.scale.x &&
                world.y >= tr.position.y && world.y <= tr.position.y + tr.scale.y &&
                sp.layer >= bestLayer) {
                bestLayer = sp.layer;
                hit = e;
            }
        });
    return hit;
}

void OverworldMode::appendSelectionGizmo(GameContext& ctx, std::vector<Sprite>& sprites) {
    if (!m_editing) return;   // jugando, el viewport es el JUEGO: nada de marcas del editor
    if (!ctx.selection || !ctx.scene || !ctx.selection->has()) return;
    const Entity e = ctx.selection->entity;
    if (!ctx.scene->alive(e) || !ctx.scene->has<Transform>(e)) return;

    const Transform& tr = ctx.scene->get<Transform>(e);
    const float x = tr.position.x, y = tr.position.y, w = tr.scale.x, h = tr.scale.y;
    const Vec2  c(x + w * 0.5f, y + h * 0.5f);   // centro de la entidad

    const Vec4 accent(0.29f, 0.62f, 1.0f, 1.0f);   // #4a9eff
    const Vec4 axisX (1.00f, 0.42f, 0.44f, 1.0f);  // rojo  = X
    const Vec4 axisY (0.42f, 0.80f, 0.37f, 1.0f);  // verde = Y
    const Vec4 axisXY(0.29f, 0.62f, 1.0f, 0.85f);  // centro = ambos ejes

    // El gizmo se mide en PÍXELES DE PANTALLA: con el zoom del editor, uno medido en
    // unidades de mundo se volvería gigante o invisible según el encuadre.
    const float zoom = std::max(1.0f, cameraZoom());
    const float px   = 1.0f / zoom;

    auto quad = [&](Vec2 mid, Vec2 size, float rot, const Vec4& col) {
        Sprite s;
        s.position = Vec2(mid.x - size.x * 0.5f, mid.y - size.y * 0.5f);
        s.size     = size;
        s.rotation = rot;
        s.color    = col;
        s.layer    = 5;                            // por encima de tiles y entidades
        sprites.push_back(s);
    };
    // Segmento A→B de grosor `thick` (px de pantalla): es la primitiva de todo el gizmo.
    auto seg = [&](Vec2 a, Vec2 b, float thickPx, const Vec4& col) {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0001f) return;
        quad(Vec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), Vec2(len, thickPx * px),
             std::atan2(dy, dx), col);
    };

    // --- Marca de selección: ESQUINAS, no un marco cerrado ---------------------------
    // Un rectángulo completo se lee como un collider; las cuatro escuadras dicen
    // "esto está seleccionado" sin fingir que son los límites físicos del objeto.
    {
        const float armX = std::min(w * 0.32f, 14.0f * px);
        const float armY = std::min(h * 0.32f, 14.0f * px);
        const float t    = 1.5f;
        const Vec4  col(accent.x, accent.y, accent.z, 0.9f);
        const float x0 = x, x1 = x + w, y0 = y, y1 = y + h;
        seg(Vec2(x0, y0), Vec2(x0 + armX, y0), t, col);  seg(Vec2(x0, y0), Vec2(x0, y0 + armY), t, col);
        seg(Vec2(x1, y0), Vec2(x1 - armX, y0), t, col);  seg(Vec2(x1, y0), Vec2(x1, y0 + armY), t, col);
        seg(Vec2(x0, y1), Vec2(x0 + armX, y1), t, col);  seg(Vec2(x0, y1), Vec2(x0, y1 - armY), t, col);
        seg(Vec2(x1, y1), Vec2(x1 - armX, y1), t, col);  seg(Vec2(x1, y1), Vec2(x1, y1 - armY), t, col);
    }

    // --- Manipulador, según la herramienta del dock ----------------------------------
    const float len  = 46.0f * px;   // largo de cada eje
    const float head = 11.0f * px;   // punta de flecha / tirador
    switch (ctx.gizmo) {
    case GizmoTool::Move: {
        const Vec2 ex(c.x + len, c.y), ey(c.x, c.y - len);   // +X derecha, +Y arriba (Y crece hacia abajo)
        seg(c, ex, 2.0f, axisX);
        seg(c, ey, 2.0f, axisY);
        // Puntas: dos trazos en diagonal desde el extremo (flecha abierta, se lee a cualquier zoom).
        seg(ex, Vec2(ex.x - head, ex.y - head * 0.55f), 2.0f, axisX);
        seg(ex, Vec2(ex.x - head, ex.y + head * 0.55f), 2.0f, axisX);
        seg(ey, Vec2(ey.x - head * 0.55f, ey.y + head), 2.0f, axisY);
        seg(ey, Vec2(ey.x + head * 0.55f, ey.y + head), 2.0f, axisY);
        // Cuadro central: arrastre libre en los dos ejes.
        quad(Vec2(c.x + head * 0.7f, c.y - head * 0.7f), Vec2(head, head), 0.0f, axisXY);
        break;
    }
    case GizmoTool::Rotate: {
        // Anillo aproximado con segmentos: el radio va en píxeles, así que se ve igual
        // de grande con cualquier zoom.
        const float r = 40.0f * px;
        const int   n = 28;
        Vec2 prev(c.x + r, c.y);
        for (int i = 1; i <= n; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
            const Vec2 cur(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
            seg(prev, cur, 2.0f, accent);
            prev = cur;
        }
        // Tirador arriba: por dónde se agarra para girar.
        seg(Vec2(c.x, c.y - r), Vec2(c.x, c.y - r - head), 2.0f, accent);
        quad(Vec2(c.x, c.y - r - head), Vec2(head * 0.8f, head * 0.8f), 0.7853982f, accent);
        break;
    }
    case GizmoTool::Scale: {
        const Vec2 ex(c.x + len, c.y), ey(c.x, c.y - len);
        seg(c, ex, 2.0f, axisX);
        seg(c, ey, 2.0f, axisY);
        // Tiradores cuadrados en los extremos (la convención de escalar).
        quad(ex, Vec2(head * 0.8f, head * 0.8f), 0.0f, axisX);
        quad(ey, Vec2(head * 0.8f, head * 0.8f), 0.0f, axisY);
        quad(c,  Vec2(head * 0.7f, head * 0.7f), 0.0f, axisXY);   // escalar uniforme
        break;
    }
    case GizmoTool::Select:
    default:
        break;   // solo las escuadras de selección
    }

    // Si lo seleccionado es la CÁMARA DEL JUEGO, además se dibuja su encuadre: qué se verá
    // al pulsar Play. Sin esto, moverla en edición sería a ciegas (la vista de edición ya no
    // la sigue). El rect sale del mismo modelo que usa el render: 480×270 unidades de
    // pantalla divididas por el zoom (px/tile), centrado en su Transform.
    if (ctx.scene->has<CameraComponent>(e)) {
        const float camZoom = std::max(1.0f, ctx.scene->get<CameraComponent>(e).zoom);
        const float halfW = 240.0f / camZoom, halfH = 135.0f / camZoom;
        const float cx = tr.position.x, cy = tr.position.y;
        const Vec4 dim(accent.x, accent.y, accent.z, 0.55f);   // más tenue que la selección
        seg(Vec2(cx - halfW, cy - halfH), Vec2(cx + halfW, cy - halfH), 1.5f, dim);   // superior
        seg(Vec2(cx - halfW, cy + halfH), Vec2(cx + halfW, cy + halfH), 1.5f, dim);   // inferior
        seg(Vec2(cx - halfW, cy - halfH), Vec2(cx - halfW, cy + halfH), 1.5f, dim);   // izquierdo
        seg(Vec2(cx + halfW, cy - halfH), Vec2(cx + halfW, cy + halfH), 1.5f, dim);   // derecho
    }
}


void OverworldMode::onArrive(GameContext& ctx) {
    ++m_steps;
    if (ctx.bus) ctx.bus->queue(PlayerMovedEvent{ m_grid.x, m_grid.y });

    const TileMapComponent* c = mapComp();
    if (c && c->map.isEncounter(m_grid.x, m_grid.y)) {
        const float roll = (nextRand() % 1000) / 1000.0f;
        if (roll < kEncounterChance && ctx.db && !ctx.db->empty()) {
            const Species& s = ctx.db->pick(nextRand());
            LOG_INFO("¡Apareció un %s salvaje! (celda %d,%d)", s.name.c_str(), m_grid.x, m_grid.y);
            if (ctx.bus) ctx.bus->queue(EncounterEvent{ s.id, s.name });
            // Empuja el combate: el overworld se pausa hasta que el modo se saque.
            if (ctx.stack) ctx.stack->push(std::make_unique<BattleMode>(s.name, s.id, s.sprite), ctx);
        }
    }
}

void OverworldMode::save(GameContext& ctx) {
    (void)ctx;
    nlohmann::json j;
    j["playerX"] = m_grid.x;
    j["playerY"] = m_grid.y;
    j["steps"]   = m_steps;
    std::ofstream f("save.json");
    if (!f) { LOG_WARN("No se pudo escribir save.json"); return; }
    f << j.dump(2);
    LOG_INFO("Partida guardada (%d,%d, %d pasos).", m_grid.x, m_grid.y, m_steps);
}

void OverworldMode::load(GameContext& ctx) {
    std::ifstream f("save.json");
    if (!f) { LOG_WARN("No hay save.json que cargar."); return; }
    try {
        nlohmann::json j;
        f >> j;
        const int x = j.value("playerX", m_grid.x);
        const int y = j.value("playerY", m_grid.y);
        m_steps = j.value("steps", m_steps);
        const TileMapComponent* c = mapComp();
        if (c && c->map.walkable(x, y)) m_grid = { x, y };

        // La posición del jugador vive en su Transform/GridMover (la gobierna el
        // script): teletransportamos ambos a la celda guardada y cancelamos cualquier
        // paso en curso, si no el GridMover seguiría deslizándose al destino viejo.
        const Vec2 cellPos(static_cast<float>(m_grid.x), static_cast<float>(m_grid.y));
        if (ctx.scene && ctx.scene->alive(m_playerEntity)) {
            if (ctx.scene->has<Transform>(m_playerEntity))
                ctx.scene->get<Transform>(m_playerEntity).position = cellPos;
            if (ctx.scene->has<GridMover>(m_playerEntity)) {
                GridMover& gm = ctx.scene->get<GridMover>(m_playerEntity);
                gm.cell = gm.from = m_grid; gm.moving = false; gm.t = 0.0f; gm.seeded = true;
            }
        }
        m_lastPlayerPos = cellPos;
        setCameraCenter(cellPos + Vec2(0.5f, 0.5f));   // re-enfoca la cámara en la celda cargada
        LOG_INFO("Partida cargada (%d,%d, %d pasos).", m_grid.x, m_grid.y, m_steps);
    } catch (...) {
        LOG_WARN("save.json corrupto; ignorado.");
    }
}

uint32_t OverworldMode::nextRand() {
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return m_rng;
}

}  // namespace pk
