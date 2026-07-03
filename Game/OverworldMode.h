// Game/OverworldMode.h — modo de exploración 2D: tilemap + jugador en grilla,
// movimiento celda a celda con colisión, encuentros en hierba alta, cámara
// ortográfica que sigue y guardar/cargar. Emite Sprites al renderer (camino 2D
// clásico). Diseño: MotorGrafico_Plan2D.md (Paso 1) + MotorGrafico_2DyCamara.md.
#pragma once

#include "Core/ECS/Entity.h"
#include "Core/EventBus.h"
#include "Core/Handle.h"
#include "Core/Math.h"
#include "Game/EventRunner.h"
#include "Game/GameMode.h"
#include "Game/TileMap.h"
#include "Renderer/2D/Animation.h"
#include "Renderer/2D/Sprite.h"
#include "Renderer/Camera/OrthographicCamera.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pk {

class AssetManager;
struct TileMapComponent;
struct CameraComponent;

class OverworldMode : public GameMode {
public:
    void onEnter(GameContext& ctx) override;
    void onExit(GameContext& ctx) override;
    void handleInput(GameContext& ctx) override;
    void variableUpdate(GameContext& ctx, float dt) override;
    void render(GameContext& ctx) override;       // emite tiles + cámara + gizmo

private:
    void     buildTileSprites();             // tiles → sprites (estáticos)
    void     ensurePlayer(GameContext& ctx); // reutiliza el jugador de la escena o lo crea
    void     spawnSpriteEntity(GameContext& ctx, const std::string& path, Vec2 world);
    Vec2     screenToWorld(Vec2 screen, int screenW, int screenH) const;
    void     loadTileset(AssetManager* assets);  // carga/re-resuelve el atlas (o color plano)
    void     ensureMap(GameContext& ctx);    // busca/crea la entidad-mapa (TileMapComponent) de la escena
    void     ensureCamera(GameContext& ctx); // busca/crea la entidad-cámara (CameraComponent) de la escena
    void     bindScene(GameContext& ctx);    // (re)vincula mapa + cámara + jugador al entrar/cambiar de escena
    void     seedDefaultMap(TileMapComponent& tmc);  // siembra el mapa por defecto (compat overworld.json / ASCII)
    TileMapComponent* mapComp();             // componente-mapa de la escena vigente (o nullptr)
    CameraComponent*  camComp();             // componente-cámara de la escena vigente (o nullptr)
    Vec2     cameraCenter() const;           // centro = Transform de la entidad-cámara (o fallback)
    float    cameraZoom() const;             // zoom del CameraComponent (o kZoom por defecto)
    void     setCameraCenter(Vec2 c);        // teletransporta la cámara (al cargar/sembrar)
    void     handlePicking(GameContext& ctx);          // click en viewport → selecciona entidad
    void     appendSelectionGizmo(GameContext& ctx, std::vector<Sprite>& sprites);
    void     onArrive(GameContext& ctx);     // al llegar: pasos + encuentros
    void     save(GameContext& ctx);
    void     load(GameContext& ctx);
    uint32_t nextRand();                      // xorshift determinista

    Entity              m_mapEntity;       // entidad-mapa (TileMapComponent) de la escena
    Entity              m_cameraEntity;    // entidad-cámara (CameraComponent) de la escena
    Scene*              m_scene = nullptr; // escena vigente (para leer el mapa del componente)
    Entity              m_playerEntity;    // el jugador vive en el ECS (Transform+Sprite)
    const Scene*        m_lastScene = nullptr;   // detecta cambio de escena (cargar/nueva)
    TextureHandle       m_playerTex;       // fallback: sprite único sin hoja
    std::vector<Sprite> m_tileSprites;     // construidos una vez en onEnter

    // Tileset/atlas (T2). Si no carga, los tiles usan color plano.
    TextureHandle       m_tilesetTex;
    int                 m_atlasCols = 1;
    int                 m_atlasRows = 1;
    bool                m_useAtlas  = false;

    AnimationSet        m_anim;            // hoja + clips del jugador (de JSON)
    Animator            m_animator;        // reproductor de clips
    bool                m_animated = false;// true si la hoja cargó

    // Transform/material persistentes del jugador (editables desde el Inspector).
    float m_playerRotDeg = 0.0f;
    Vec2  m_playerScale{ 1.0f, 1.0f };
    Vec4  m_playerTint{ 1.0f, 1.0f, 1.0f, 1.0f };
    int   m_playerLayer = 1;

    // El MOVIMIENTO del jugador vive ahora en Lua (player_movement.lua + self:try_step,
    // estado en su GridMover). El overworld solo LEE su Transform para presentar:
    // m_grid = última celda donde se procesaron los encuentros; m_lastPlayerPos =
    // posición leída el frame previo (para inferir dirección/caminar por el delta).
    IVec2 m_grid{ 0, 0 };               // celda lógica actual del jugador (para encuentros)
    Vec2  m_lastPlayerPos{ 0.0f, 0.0f };// Transform del jugador en el frame anterior
    int   m_facing = 0;                 // 0 abajo, 1 arriba, 2 izq, 3 der (dir de animación)
    int   m_steps  = 0;

    // La cámara es una ENTIDAD del ECS (m_cameraEntity, CameraComponent + Transform). Esta
    // OrthographicCamera es solo un helper transitorio para armar la viewProjection en
    // render() a partir de la entidad; ya no posee el estado (centro/zoom).
    OrthographicCamera m_cam;
    uint32_t           m_rng = 0x1234567u;

    EventRunner        m_events;   // secuencias de diálogo (hablar con NPCs/carteles)

    // Recarga del mapa en caliente cuando el editor de tiles guarda (MapSavedEvent).
    Subscription       m_mapSavedSub;
    // Soltar un asset del navegador sobre el viewport → crear una entidad (diferido a
    // variableUpdate, que sí tiene el GameContext con la escena).
    Subscription       m_assetDropSub;
    std::string        m_dropPath;
    Vec2               m_dropScreen{ 0.0f, 0.0f };
    bool               m_hasDrop = false;

    static constexpr float kEncounterChance = 0.18f;  // prob. al pisar hierba alta
    static constexpr float kZoom            = 32.0f;  // zoom por defecto de la cámara (px/tile)
};

}  // namespace pk
