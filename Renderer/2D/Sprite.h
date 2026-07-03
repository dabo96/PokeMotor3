// Renderer/2D/Sprite.h — un sprite del camino 2D clásico + el vértice que produce.
// Diseño: MotorGrafico_2DyCamara.md ("el corazón: batching de sprites").
// Posición/tamaño en unidades de mundo (coords de tile); uvRect en [0,1] dentro
// del atlas; layer = orden de pintado. Textura inválida → textura blanca (tinte).
#pragma once

#include "Core/Handle.h"
#include "Core/Math.h"

namespace pk {

struct Sprite {
    Vec2          position{ 0.0f, 0.0f };
    Vec2          size{ 1.0f, 1.0f };
    Vec4          uvRect{ 0.0f, 0.0f, 1.0f, 1.0f };   // x,y,w,h en [0,1]
    Vec4          color{ 1.0f, 1.0f, 1.0f, 1.0f };
    float         rotation = 0.0f;                     // radianes, alrededor del centro
    int           layer = 0;
    TextureHandle texture;                             // inválida → blanca
};

// Vértice que consume el pipeline sprite2d (stride 32 bytes).
struct Vertex2D {
    Vec2 pos;
    Vec2 uv;
    Vec4 color;
};

}  // namespace pk
