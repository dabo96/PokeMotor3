// UI/UITheme.h — look consistente y swappable de la UI del juego: el marco de las
// cajas (9-slice), el tamaño/color de texto, el acento de selección y el cursor.
// Data-driven donde tiene sentido. Diseño: MotorGrafico_UIJuego.md.
#pragma once

#include "Core/Handle.h"
#include "Core/Math.h"
#include "UI/NineSlice.h"

namespace pk {

struct UITheme {
    NineSlice     panel;                                  // marco de cajas/paneles
    float         fontSize       = 12.0f;                 // tamaño de texto por defecto (px)
    Vec4          textColor      { 1.0f, 1.0f, 1.0f, 1.0f };
    Vec4          highlightColor { 0.29f, 0.62f, 1.0f, 1.0f };  // selección/acento (#4a9eff)
    TextureHandle cursor;                                 // sprite del cursor del menú
    float         padding        = 8.0f;                  // margen interior de los paneles
};

}  // namespace pk
