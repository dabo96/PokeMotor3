// Scene/Components.h — componentes (structs planos) de la escena 2D + DrawItem.
// Cualquier struct plano puede ser un componente; el ECS no los conoce de forma
// especial (ver Core/ECS). Aquí viven los básicos que usan el render-feed, el
// SceneManager (serialización) y el editor.
#pragma once

#include "Core/Handle.h"
#include "Core/Math.h"
#include "Game/TileMap.h"
#include "Renderer/Vulkan/ImageVk.h"   // FilterMode

#include <string>
#include <unordered_map>

namespace pk {

// Posición/orientación/escala en el plano 2D (unidades de mundo = tiles).
struct Transform {
    Vec2  position{ 0.0f, 0.0f };
    float rotation = 0.0f;          // radianes, alrededor del centro
    Vec2  scale{ 1.0f, 1.0f };
};

// Sprite de una entidad. La RUTA se serializa; el handle se resuelve al cargar
// (AssetManager, con caché). tint.a = opacidad. layer = orden de pintado.
struct SpriteComponent {
    std::string   texturePath;
    TextureHandle tex;
    Vec4          uvRect{ 0.0f, 0.0f, 1.0f, 1.0f };   // sub-rect del atlas/hoja (frame)
    Vec4          tint{ 1.0f, 1.0f, 1.0f, 1.0f };
    int           layer = 0;
    // Filtrado del asset: Smooth (defecto) = arte HD (lineal+mips); Pixel = pixel-art
    // (nearest). Lo aplica AssetManager al cargar la textura; para que surta efecto, el
    // resolve del handle debe pasarlo a loadTexture (ver SceneManager::init, prohibido aquí
    // — documentado como paso manual). El sampler real lo elige el run por Texture::filter().
    FilterMode    filter = FilterMode::Smooth;
};

// Nombre legible para la jerarquía ("bulbasaur" en vez de "entidad 7").
struct NameComponent {
    std::string value;
};

// Marca la entidad del jugador (sin datos). El overworld la busca al entrar a una
// escena (cargada o nueva) en vez de crear un jugador duplicado.
struct PlayerTag {};

// Script Lua adjunto a la entidad. Solo se guarda la RUTA del .lua (serializable);
// el estado de ejecución (entorno, on_update…) lo gestiona el ScriptSystem, así
// este componente no arrastra sol2 a todo el que incluya Components.h.
struct ScriptComponent {
    std::string path;   // ruta del .lua (p.ej. "Assets/Scripts/npc_wander.lua")
};

// NPC / cartel: texto que se muestra al interactuar (Confirm) mirándolo de frente.
// Se serializa (es contenido de la escena). El OverworldMode busca la entidad con este
// componente en la celda frontal del jugador y lanza su texto por el EventRunner.
struct DialogueComponent {
    std::string text;
};

// Estado de movimiento por CASILLAS de una entidad (API de grid del scripting:
// self:try_step). Runtime-only: NO se serializa — es estado efímero de un paso en
// curso. El ScriptSystem lo crea on-demand al primer try_step, avanza el
// deslizamiento suave cada frame y escribe el Transform interpolado.
struct GridMover {
    IVec2 cell{ 0, 0 };          // celda actual (o destino mientras se desliza)
    IVec2 from{ 0, 0 };          // celda de origen durante el paso
    bool  moving = false;        // hay un paso en curso (bloquea nuevos try_step)
    float t      = 0.0f;         // 0..1 a lo largo del paso
    float stepDuration = 0.16f;  // segundos por celda (= overworld del jugador)
    bool  seeded = false;        // cell sembrada desde el Transform la 1ª vez
};

// Edición visual por celda (override): el tile usa estos valores en vez de los de su
// tipo. Vive dentro del TileMapComponent (key del mapa = y*width + x).
struct TileXform {
    Vec2  position{ 0.0f, 0.0f };
    float rotationDeg = 0.0f;
    Vec2  scale{ 1.0f, 1.0f };
    Vec4  tint{ 1.0f, 1.0f, 1.0f, 1.0f };
    int   layer = 0;
};

// Cámara 2D de la escena como componente: la posee una entidad singleton "Camera"
// (igual patrón que la entidad-mapa). El CENTRO de la cámara es el Transform.position
// de su entidad (así se mueve/edita/selecciona como cualquier sprite); aquí solo va el
// zoom (píxeles por unidad de mundo). El OverworldMode la LEE para emitir la cámara al
// renderer; el SEGUIMIENTO del jugador lo gobierna un script (camera_follow.lua), no C++.
struct CameraComponent {
    float zoom = 32.0f;   // píxeles por tile (zoom ortográfico)
};

// El MAPA de la escena como componente ("una escena = un mapa"): la grilla de tiles
// (TileMap: tipos + inicio del jugador) y los overrides visuales por celda. Lo posee
// una entidad singleton "Map"; el OverworldMode lo LEE y el editor de tiles lo edita.
// Se serializa con la escena (scene.json) — sustituye al antiguo overworld.json.
struct TileMapComponent {
    TileMap                            map;
    std::unordered_map<int, TileXform> overrides;   // key = y*width + x
};

// Lo que el camino de render 3D consume (PAUSADO: el juego corre en 2D, pero el
// Renderer aún referencia este tipo). Se resuelve contra el AssetManager.
struct DrawItem {
    MeshHandle     mesh;
    MaterialHandle material;
    Mat4           transform{ 1.0f };
    bool           transparent = false;
};

}  // namespace pk
