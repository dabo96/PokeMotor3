// Renderer/Lighting/Light.h — luz de escena (dato).
// Diseño: MotorGrafico_LucesYSombras.md. Las luces son dato de la escena; cada
// frame se recolectan en un buffer de GPU (set 0). El sol direccional va aparte
// (en el CameraUBO); aquí van las locales: puntuales y spot.
#pragma once

#include "Core/Math.h"

namespace pk {

struct Light {
    enum Type { Point = 0, Spot = 1 };
    Type  type      = Point;
    Vec3  position{ 0.0f, 0.0f, 0.0f };
    Vec3  direction{ 0.0f, -1.0f, 0.0f };  // spot
    Vec3  color{ 1.0f, 1.0f, 1.0f };
    float intensity = 1.0f;
    float range     = 10.0f;
    float innerDeg  = 20.0f;                // spot
    float outerDeg  = 30.0f;                // spot
};

}  // namespace pk
