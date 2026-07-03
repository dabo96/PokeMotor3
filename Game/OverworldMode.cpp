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

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

namespace pk {

void OverworldMode::onEnter(GameContext& ctx) {
    loadTileset(ctx.assets);   // atlas con textura (o color plano si no hay tileset)

    // Animación del jugador desde datos; si la hoja no carga, sprite único.
    if (ctx.assets) {
        m_anim = loadAnimationSet(*ctx.assets, "Assets/Data/player_anim.json");
        if (m_anim.valid) {
            for (const auto& [name, clip] : m_anim.clips) m_animator.addClip(name, clip);
            m_animated = true;
        } else {
            m_playerTex = ctx.assets->loadTexture("Assets/Models/Sprites/001.png");
        }
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
    }

    LOG_INFO("Overworld 2D listo. WASD = mover, F5/F9 = guardar/cargar partida. El mapa es parte de la escena (menú Archivo).");
}

// Componente-mapa de la escena vigente (o nullptr si aún no hay / se invalidó).
TileMapComponent* OverworldMode::mapComp() {
    if (!m_scene || !m_scene->alive(m_mapEntity) || !m_scene->has<TileMapComponent>(m_mapEntity))
        return nullptr;
    return &m_scene->get<TileMapComponent>(m_mapEntity);
}

// Busca la entidad-mapa de la escena; si no existe (escena nueva/sin guardar), la crea
// con el mapa por defecto. Igual patrón que ensurePlayer con PlayerTag.
void OverworldMode::ensureMap(GameContext& ctx) {
    if (!ctx.scene) return;
    Entity found{};
    ctx.scene->view<TileMapComponent>().each([&](Entity e, TileMapComponent&) { found = e; });
    if (found.valid()) { m_mapEntity = found; return; }

    TileMapComponent tmc;
    seedDefaultMap(tmc);
    m_mapEntity = ctx.scene->createEntity();
    ctx.scene->add<NameComponent>(m_mapEntity, NameComponent{ "Map" });
    ctx.scene->add<TileMapComponent>(m_mapEntity, std::move(tmc));
}

CameraComponent* OverworldMode::camComp() {
    if (!m_scene || !m_scene->alive(m_cameraEntity) || !m_scene->has<CameraComponent>(m_cameraEntity))
        return nullptr;
    return &m_scene->get<CameraComponent>(m_cameraEntity);
}

// Centro de la cámara = Transform.position de su entidad (así se mueve/edita como cualquier
// otra). Fallback al jugador si aún no hay entidad-cámara (no debería tras ensureCamera).
Vec2 OverworldMode::cameraCenter() const {
    if (m_scene && m_scene->alive(m_cameraEntity) && m_scene->has<Transform>(m_cameraEntity))
        return m_scene->get<Transform>(m_cameraEntity).position;
    return m_lastPlayerPos + Vec2(0.5f, 0.5f);
}

float OverworldMode::cameraZoom() const {
    if (m_scene && m_scene->alive(m_cameraEntity) && m_scene->has<CameraComponent>(m_cameraEntity))
        return m_scene->get<CameraComponent>(m_cameraEntity).zoom;
    return kZoom;
}

void OverworldMode::setCameraCenter(Vec2 c) {
    if (m_scene && m_scene->alive(m_cameraEntity) && m_scene->has<Transform>(m_cameraEntity))
        m_scene->get<Transform>(m_cameraEntity).position = c;
}

// Busca la entidad-cámara de la escena; si no existe, la crea como singleton con el centro
// en el jugador, el zoom por defecto y el script de seguimiento (camera_follow.lua, que la
// manipula como en "play"; apagar su export 'follow' la deja libre). Mismo patrón que
// ensureMap/ensurePlayer.
void OverworldMode::ensureCamera(GameContext& ctx) {
    if (!ctx.scene) return;
    Entity found{};
    ctx.scene->view<CameraComponent>().each([&](Entity e, CameraComponent&) { found = e; });
    if (found.valid()) { m_cameraEntity = found; return; }

    m_cameraEntity = ctx.scene->createEntity();
    ctx.scene->add<NameComponent>(m_cameraEntity, NameComponent{ "Camera" });
    ctx.scene->add<Transform>(m_cameraEntity, Transform{ m_lastPlayerPos + Vec2(0.5f, 0.5f), 0.0f, Vec2(1.0f, 1.0f) });
    ctx.scene->add<CameraComponent>(m_cameraEntity, CameraComponent{ kZoom });
    ctx.scene->add<ScriptComponent>(m_cameraEntity, ScriptComponent{ "Assets/Scripts/camera_follow.lua" });
}

// Mapa por defecto al crear la entidad-mapa: por compatibilidad siembra una vez el viejo
// `overworld.json` si existe (tipos + overrides); si no, el mapa ASCII de demo. A partir
// de ahí la verdad vive en la escena (scene.json), no en overworld.json.
void OverworldMode::seedDefaultMap(TileMapComponent& tmc) {
    const std::string owPath = Project::instance().resolveRead("Assets/Data/overworld.json");
    if (tmc.map.loadJson(owPath)) {
        std::ifstream f(owPath);
        if (f) {
            try {
                nlohmann::json j;
                f >> j;
                if (j.contains("overrides") && j["overrides"].is_array()) {
                    for (const auto& o : j["overrides"]) {
                        const int x = o.value("x", -1), y = o.value("y", -1);
                        if (!tmc.map.inBounds(x, y)) continue;
                        TileXform xf;
                        xf.position = Vec2(static_cast<float>(x), static_cast<float>(y));
                        if (o.contains("pos") && o["pos"].size() == 2)
                            xf.position = Vec2(o["pos"][0].get<float>(), o["pos"][1].get<float>());
                        xf.rotationDeg = o.value("rot", 0.0f);
                        if (o.contains("scale") && o["scale"].size() == 2)
                            xf.scale = Vec2(o["scale"][0].get<float>(), o["scale"][1].get<float>());
                        if (o.contains("tint") && o["tint"].size() == 4)
                            xf.tint = Vec4(o["tint"][0].get<float>(), o["tint"][1].get<float>(),
                                           o["tint"][2].get<float>(), o["tint"][3].get<float>());
                        xf.layer = o.value("layer", 0);
                        tmc.overrides[y * tmc.map.width() + x] = xf;
                    }
                }
            } catch (...) {}
        }
        return;
    }
    // T=árbol/muro  .=camino  g=césped  G=hierba alta  w=agua  P=inicio.
    tmc.map.loadAscii({
        "TTTTTTTTTTTT", "T..........T", "T..gggggg..T", "T..gGGGGg..T",
        "T..gGGGGg.wT", "T....P...wwT", "T..gggg..wwT", "T..gggg...wT",
        "T..........T", "TTTTTTTTTTTT",
    });
}

// (Re)vincula el mapa y el jugador al entrar al overworld o al cambiar de escena.
void OverworldMode::bindScene(GameContext& ctx) {
    m_scene     = ctx.scene;
    m_lastScene = ctx.scene;
    ensureMap(ctx);
    if (const TileMapComponent* c = mapComp())
        m_grid = c->map.playerStart();
    m_lastPlayerPos = Vec2(static_cast<float>(m_grid.x), static_cast<float>(m_grid.y));
    ensureCamera(ctx);   // crea/encuentra la entidad-cámara (centro inicial = jugador)
    buildTileSprites();
    ensurePlayer(ctx);   // ajusta m_grid/m_lastPlayerPos si ya hay un jugador en la escena
    setCameraCenter(m_lastPlayerPos + Vec2(0.5f, 0.5f));   // enfoca la cámara en el jugador al entrar
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

void OverworldMode::ensurePlayer(GameContext& ctx) {
    if (!ctx.scene) return;

    // ¿Ya hay un jugador en la escena (p.ej. recién cargada)? Reutilízalo y coloca el
    // overworld en su posición. Si no, crea uno por defecto.
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
        return;
    }

    m_playerEntity = ctx.scene->createEntity();
    ctx.scene->add<PlayerTag>(m_playerEntity, PlayerTag{});
    Transform tr;
    tr.position = Vec2(static_cast<float>(m_grid.x), static_cast<float>(m_grid.y));  // arranca en playerStart
    ctx.scene->add<Transform>(m_playerEntity, tr);
    SpriteComponent sp;
    sp.tex = m_animated ? m_anim.sheet.texture : m_playerTex;
    ctx.scene->add<SpriteComponent>(m_playerEntity, sp);
    ctx.scene->add<NameComponent>(m_playerEntity, NameComponent{ "Player" });
    // Su control de movimiento vive en Lua (input + try_step con colisión de grid).
    ctx.scene->add<ScriptComponent>(m_playerEntity, ScriptComponent{ "Assets/Scripts/player_movement.lua" });
    // Estado de grid sembrado en la celda de inicio: el GridMover gobierna su Transform.
    GridMover gm; gm.cell = gm.from = m_grid; gm.seeded = true;
    ctx.scene->add<GridMover>(m_playerEntity, gm);
    m_lastPlayerPos = tr.position;
}

void OverworldMode::spawnSpriteEntity(GameContext& ctx, const std::string& path, Vec2 world) {
    if (!ctx.scene || !ctx.assets) return;
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
    // escena (menú Archivo → Guardar/Abrir), ya no se guarda aparte con F6/F7.
    if (ctx.input->wasKeyPressed(Key::F5)) save(ctx);
    if (ctx.input->wasKeyPressed(Key::F9)) load(ctx);
}

void OverworldMode::variableUpdate(GameContext& ctx, float dt) {
    // Materializa un drop de asset pendiente: crea la entidad en la celda soltada.
    if (m_hasDrop) {
        m_hasDrop = false;
        const Vec2 wpos = screenToWorld(m_dropScreen, ctx.screenW, ctx.screenH);
        spawnSpriteEntity(ctx, m_dropPath, Vec2(std::floor(wpos.x), std::floor(wpos.y)));
    }

    m_scene = ctx.scene;   // escena vigente (la usa mapComp / el callback de colisión)

    // Re-vincula mapa + jugador si cambió la escena (cargar/nueva): comparamos el PUNTERO
    // de la escena, no alive() — los ids/generación se reinician por escena y un Entity
    // viejo puede "coincidir" por azar con otra entidad de la escena nueva.
    if (ctx.scene && ctx.scene != m_lastScene) {
        bindScene(ctx);                      // re-resuelve la entidad-mapa + reconstruye sprites + jugador
    } else if (ctx.scene && !ctx.scene->alive(m_playerEntity)) {
        ensurePlayer(ctx);                   // el jugador desapareció en la misma escena
    }

    // Corre los scripts del mundo (incluido el movimiento del jugador vía try_step) como
    // parte de la simulación del overworld. Al estar aquí, un overlay que congele este
    // modo (MenuMode/DialogueMode) pausa también los scripts: el jugador deja de moverse.
    // Mientras una CORRUTINA de evento esté activa (p.ej. durante un wait() sin UI en la
    // pila), también congelamos el on_update del mundo: el jugador no se mueve a mitad de
    // un evento aunque ese instante no haya un DialogueMode encima.
    if (ctx.scripts && ctx.scene && !ctx.scripts->eventsActive())
        ctx.scripts->update(*ctx.scene, dt);

    // El MOVIMIENTO del jugador lo dicta su script (player_movement.lua), que ya corrió
    // este frame y dejó la celda lógica en su Transform (vía GridMover). Aquí solo lo
    // LEEMOS para presentar; el script no sabe de cámara/animación/encuentros.
    Vec2 playerPos = m_lastPlayerPos;
    if (ctx.scene && ctx.scene->alive(m_playerEntity) && ctx.scene->has<Transform>(m_playerEntity))
        playerPos = ctx.scene->get<Transform>(m_playerEntity).position;

    // Dirección + caminar inferidos del DELTA de posición respecto al frame anterior
    // (sin que el script reporte nada). Quieto → idle conservando la última dirección.
    const Vec2 delta  = playerPos - m_lastPlayerPos;
    const bool moving = glm::dot(delta, delta) > 1e-6f;
    if (moving) {
        if (std::fabs(delta.x) >= std::fabs(delta.y)) m_facing = (delta.x > 0.0f) ? 3 : 2;
        else                                          m_facing = (delta.y > 0.0f) ? 0 : 1;
    }

    // Encuentros: al ASENTARSE en una celda nueva (terminó el deslizamiento). No se
    // disparan a mitad de paso, para que el combate empiece con el jugador alineado.
    const IVec2 cell(static_cast<int>(std::lround(playerPos.x)),
                     static_cast<int>(std::lround(playerPos.y)));
    if (!moving && cell != m_grid) {
        m_grid = cell;
        onArrive(ctx);
    }

    m_lastPlayerPos = playerPos;

    // La cámara YA NO se mueve aquí: es una entidad del ECS (m_cameraEntity) y su
    // seguimiento del jugador lo gobierna un script (camera_follow.lua, vía player_pos()),
    // que ya corrió en ctx.scripts->update arriba. Así en editor se puede mover libre
    // (apagando su export 'follow') y en play la maneja el script.

    // Selección del editor: picking con el ratón + aplicar/refrescar el inspector.
    handlePicking(ctx);

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
        SpriteComponent& sp = ctx.scene->get<SpriteComponent>(m_playerEntity);
        sp.tint  = m_playerTint;
        sp.layer = m_playerLayer;
        if (m_animated) {
            sp.tex    = m_anim.sheet.texture;
            sp.uvRect = m_anim.sheet.uvForFrame(m_animator.frame());
        } else {
            sp.tex    = m_playerTex;
            sp.uvRect = Vec4(0.0f, 0.0f, 1.0f, 1.0f);
        }
    }

}

// Emisión al renderer (separada de la lógica): cámara + tiles + gizmo. El GameStack la
// llama aunque el modo esté pausado bajo un overlay (menú), así el overworld se sigue
// viendo "congelado" detrás. El jugador-entidad lo dibuja el render-feed del Engine.
void OverworldMode::render(GameContext& ctx) {
    if (!ctx.renderer) return;
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
}

void OverworldMode::handlePicking(GameContext& ctx) {
    if (!ctx.selection || !ctx.input || !ctx.scene) return;
    if (ctx.uiCapturesMouse) return;                 // el ratón está sobre la UI
    if (!ctx.input->wasMousePressed(1)) return;      // 1 = botón izquierdo

    const Vec2 wpt = screenToWorld(ctx.input->mousePosition(), ctx.screenW, ctx.screenH);

    // Entidad bajo el cursor: la de mayor capa cuyo bbox (Transform.position..+scale)
    // contiene el punto. El inspector edita sus componentes; ya no hay sync de tiles.
    Entity hit{};
    int    bestLayer = -1;
    ctx.scene->view<Transform, SpriteComponent>().each(
        [&](Entity e, Transform& tr, SpriteComponent& sp) {
            if (wpt.x >= tr.position.x && wpt.x <= tr.position.x + tr.scale.x &&
                wpt.y >= tr.position.y && wpt.y <= tr.position.y + tr.scale.y &&
                sp.layer >= bestLayer) {
                bestLayer = sp.layer;
                hit = e;
            }
        });

    if (hit.valid()) ctx.selection->entity = hit;
    else             ctx.selection->clear();
}

void OverworldMode::appendSelectionGizmo(GameContext& ctx, std::vector<Sprite>& sprites) {
    if (!ctx.selection || !ctx.scene || !ctx.selection->has()) return;
    const Entity e = ctx.selection->entity;
    if (!ctx.scene->alive(e) || !ctx.scene->has<Transform>(e)) return;

    const Transform& tr = ctx.scene->get<Transform>(e);
    const float x = tr.position.x, y = tr.position.y, w = tr.scale.x, h = tr.scale.y;

    const Vec4  acc(0.29f, 0.62f, 1.0f, 1.0f);   // acento #4a9eff
    const float t = 0.08f;                        // grosor del borde (mundo)
    auto bar = [&](float bx, float by, float bw, float bh) {
        Sprite s;
        s.position = Vec2(bx, by);
        s.size     = Vec2(bw, bh);
        s.color    = acc;
        s.layer    = 5;                           // por encima de tiles y jugador
        sprites.push_back(s);
    };
    bar(x, y, w, t);                 // borde superior
    bar(x, y + h - t, w, t);         // borde inferior
    bar(x, y, t, h);                 // borde izquierdo
    bar(x + w - t, y, t, h);         // borde derecho
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
            if (ctx.stack) ctx.stack->push(std::make_unique<BattleMode>(s.name, s.id), ctx);
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
