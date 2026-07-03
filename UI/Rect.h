// UI/Rect.h — rectángulo en píxeles de pantalla (origen sup-izq, +Y abajo), la
// unidad de toda la UI del juego (lowRes 480x270). Diseño: MotorGrafico_UIJuego.md.
#pragma once

#include "Core/Math.h"

namespace pk {

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

    float right()  const { return x + w; }
    float bottom() const { return y + h; }
    Vec2  center() const { return Vec2(x + w * 0.5f, y + h * 0.5f); }
    bool  contains(Vec2 p) const { return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h; }

    // Rect encogido 'm' píxeles por cada lado (para el padding interior de un panel).
    Rect inset(float m) const { return Rect{ x + m, y + m, w - 2.0f * m, h - 2.0f * m }; }
};

}  // namespace pk
