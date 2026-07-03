// UI/NineSlice.h — caja estirable de 9 zonas para marcos de UI (diálogos, menús).
// La textura se corta por sus márgenes (en píxeles de la TEXTURA): las 4 esquinas no
// se deforman, los 4 bordes se estiran en un eje y el centro en ambos. Sin textura
// válida, el UIRenderer la pinta como un rectángulo de color 'tint'.
// Diseño: MotorGrafico_UIJuego.md.
#pragma once

#include "Core/Handle.h"
#include "Core/Math.h"

namespace pk {

struct NineSlice {
    TextureHandle texture;                 // marco (opcional; inválida = color plano)
    Vec2  texSize{ 0.0f, 0.0f };           // tamaño de la textura en píxeles (para las UVs)
    float left = 0, right = 0, top = 0, bottom = 0;  // márgenes en píxeles de la textura
    Vec4  tint{ 1.0f, 1.0f, 1.0f, 1.0f };  // color/opacidad
};

}  // namespace pk
