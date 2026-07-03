// Renderer/2D/SpriteSheet.h — hoja de sprites: una textura dividida en una grilla
// columns×rows de frames. Dado un índice de frame, da su uvRect en [0,1].
// Diseño: MotorGrafico_Plan2D.md (Paso 2 — animación de sprites).
#pragma once

#include "Core/Handle.h"
#include "Core/Math.h"

namespace pk {

struct SpriteSheet {
    TextureHandle texture;
    int           columns = 1;
    int           rows    = 1;

    Vec4 uvForFrame(int frame) const {
        if (columns < 1 || rows < 1) return Vec4(0.0f, 0.0f, 1.0f, 1.0f);
        const int total = columns * rows;
        int f = frame % total;
        if (f < 0) f += total;
        const int   cx = f % columns;
        const int   cy = f / columns;
        const float w  = 1.0f / static_cast<float>(columns);
        const float h  = 1.0f / static_cast<float>(rows);
        return Vec4(cx * w, cy * h, w, h);
    }
};

}  // namespace pk
