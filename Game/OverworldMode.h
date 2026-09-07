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
#include "Game/GameEvents.h"
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
    void     bindPlayer(GameContext& ctx);   // vincula el jugador que haya en la escena (no lo crea)
    Entity   spawnSpriteEntity(GameContext& ctx, const std::string& path, Vec2 world);
    // Altas EXPLÍCITAS de entidad (menú Entidad del editor). Son las mismas que usan los
    // ensure*, extraídas para poder invocarlas a mano: el motor no autocrea contenido.
    Entity   spawnTileMap(GameContext& ctx);
    Entity   spawnCamera(GameContext& ctx, Vec2 world);
    Entity   spawnPlayer(GameContext& ctx, IVec2 cell);
    void     createEntityOfKind(GameContext& ctx, NewEntityKind kind, const std::string& path);
    Vec2     viewCenterCell() const;         // celda del centro de la vista (dónde se crea)
    Vec2     screenToWorld(Vec2 screen, int screenW, int screenH) const;
    Vec2     worldToScreen(Vec2 world, int screenW, int screenH) const;   // inversa (ancla del inspector)
    void     loadTileset(AssetManager* assets);  // carga/re-resuelve el atlas (o color plano)
    void     findMap(GameContext& ctx);      // busca la entidad-mapa (TileMapComponent) de la escena
    void     findCamera(GameContext& ctx);   // busca la entidad-cámara (CameraComponent) de la escena
    void     bindScene(GameContext& ctx);    // (re)vincula mapa + cámara + jugador al entrar/cambiar de escena
    TileMapComponent* mapComp();             // componente-mapa de la escena vigente (o nullptr)
    CameraComponent*  camComp();             // componente-cámara de la escena vigente (o nullptr)
    Vec2     cameraCenter() const;           // centro de la vista: la de EDICIÓN o la del juego
    float    cameraZoom() const;             // ídem para el zoom
    void     setCameraCenter(Vec2 c);        // teletransporta la cámara del juego (al cargar/sembrar)
    void     updateEditorCamera(GameContext& ctx);   // pan/zoom/encuadre con el ratón (solo en edición)
    void     handlePicking(GameContext& ctx);          // click en viewport → selecciona entidad
    // Manipulación con el gizmo: arrastrar sus ejes/anillo mueve, gira o escala la entidad
    // seleccionada. Devuelve true si el gizmo se quedó con el ratón (entonces no hay picking).
    bool     handleGizmoDrag(GameContext& ctx);
    // Parte del gizmo bajo el cursor (o la que se está arrastrando).
    enum class GizmoPart { None, AxisX, AxisY, Both, Ring };
    GizmoPart gizmoPartAt(GameContext& ctx, Vec2 world, Vec2 center) const;
    Entity   entityAt(GameContext& ctx, Vec2 world) const;  // entidad bajo un punto del mundo (capa mayor)
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
    TextureHandle       m_playerTex;       // hoja del jugador (de kPlayerConfig)
    std::string         m_playerTexPath;   // su ruta, para serializarla en el SpriteComponent
    std::vector<Sprite> m_tileSprites;     // construidos una vez en onEnter

    // Tileset/atlas (T2). Si no carga, los tiles usan color plano.
    TextureHandle       m_tilesetTex;
    int                 m_atlasCols = 1;
    int                 m_atlasRows = 1;
    bool                m_useAtlas  = false;

    // --- Cámara de EDICIÓN ---
    // Encuadre propio del editor, independiente de la entidad-cámara (que es la del JUEGO y
    // la gobierna camera_follow.lua). Se conserva mientras juegas, así que al pulsar Stop
    // vuelves exactamente a donde estabas mirando. Tecla F = centrar en la selección.
    Vec2                m_editCamCenter{ 0.0f, 0.0f };
    float               m_editCamZoom  = kZoom;
    bool                m_editCamValid = false;   // aún sin encuadrar: se sitúa al vincular la escena
    bool                m_editing      = true;    // ctx.mode != Play (lo refrescan variableUpdate/render)
    bool                m_panning      = false;   // arrastrando la vista con el botón central/derecho
    Vec2                m_panLastMouse{ 0.0f, 0.0f };
    bool                m_warnedNoCamera = false; // el aviso de "sin cámara" se da una vez por partida

    AnimationSet        m_anim;            // hoja + clips del jugador (de JSON)
    Animator            m_animator;        // reproductor de clips
    bool                m_animated = false;// true si la hoja cargó

    // Transform/material persistentes del jugador (editables desde el Inspector).
    float m_playerRotDeg = 0.0f;

    // Arrastre del gizmo en curso (editor): qué parte se agarró y el estado del Transform al
    // empezar, para que el resultado dependa del desplazamiento TOTAL y no se acumule error.
    GizmoPart m_gizmoDrag = GizmoPart::None;
    Vec2      m_gizmoGrabWorld;      // punto del mundo donde se pulsó
    Vec2      m_gizmoStartPos;       // Transform al empezar
    Vec2      m_gizmoStartScale;
    float     m_gizmoStartRot   = 0.0f;
    float     m_gizmoStartAngle = 0.0f;   // ángulo cursor→centro al empezar (rotar)
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
    // Soltar un .lua sobre el viewport → adjuntarlo a la entidad que haya bajo el cursor
    // (mismo picking que el click). También diferido a variableUpdate.
    Subscription       m_scriptDropSub;
    std::string        m_scriptDropPath;
    Vec2               m_scriptDropScreen{ 0.0f, 0.0f };
    bool               m_hasScriptDrop = false;
    // Alta pedida desde el menú Entidad del editor (CreateEntityEvent). Como los drops, se
    // anota aquí y se materializa en variableUpdate, que tiene la escena del frame.
    Subscription       m_newEntitySub;
    NewEntityKind      m_newEntityKind = NewEntityKind::Empty;
    std::string        m_newEntityPath;
    bool               m_hasNewEntity  = false;

    // Tamaño del mapa que crea el menú Entidad: una grilla vacía razonable para empezar
    // (el editor de tiles la redimensiona). El motor no trae ningún mundo de ejemplo.
    static constexpr int   kNewMapW = 20;
    static constexpr int   kNewMapH = 15;
    static constexpr float kEncounterChance = 0.18f;  // prob. al pisar hierba alta
    static constexpr float kZoom            = 32.0f;  // zoom por defecto de la cámara (px/tile)
    // Configuración del jugador (hoja + clips). Único literal de ruta que queda aquí: el
    // ARTE ya no se nombra en C++, se declara en este JSON.
    static constexpr const char* kPlayerConfig = "Assets/Data/player_anim.json";
};

}  // namespace pk
