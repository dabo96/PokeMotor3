// Editor/EditorTheme.h — tema FluentUI del editor derivado del design system
// "WinUI" (winui.css de Claude Design). Compartido por la ventana principal
// (EditorUI) y las ventanas de herramientas (ToolWindow) para un look uniforme.
#pragma once

#include "Math/Color.h"
#include "Theme/FluentTheme.h"

namespace pk {

inline FluentUI::Style winuiEditorStyle() {
    using FluentUI::Color;
    FluentUI::Style s = FluentUI::GetEditorDarkStyle();

    const Color bg1 = Color::FromHex("#1a1d22");   // fondo de ventana
    const Color bg2 = Color::FromHex("#22262d");   // paneles
    const Color bg3 = Color::FromHex("#2a2e36");   // headers / toolbar / botones
    const Color bg4 = Color::FromHex("#333842");   // hover
    const Color bg5 = Color::FromHex("#3d4350");   // activo / pressed
    const Color b1  = Color::FromHex("#2c313a");   // borde sutil
    const Color b2  = Color::FromHex("#383e49");
    const Color b3  = Color::FromHex("#4a5160");
    const Color t1  = Color::FromHex("#e6e8ec");   // texto primario
    const Color t2  = Color::FromHex("#a8aebb");   // texto secundario
    const Color acc = Color::FromHex("#4a9eff");   // acento cian-azul

    s.isDarkTheme     = true;
    s.backgroundColor = bg1;
    s.accentColor     = acc;
    s.sliderFillColor = acc;

    s.panel.background       = bg2;
    s.panel.headerBackground = bg3;
    s.panel.borderColor      = b1;
    s.panel.borderWidth      = 1.0f;
    s.panel.cornerRadius     = 6.0f;
    s.panel.headerText.color = t2;

    s.button.background.normal  = bg3;
    s.button.background.hover   = bg4;
    s.button.background.pressed = bg5;
    s.button.border.normal      = b2;
    s.button.border.hover       = b3;
    s.button.foreground.normal  = t1;
    s.button.text.color         = t1;
    s.button.cornerRadius       = 3.0f;

    s.separator.color          = b1;
    s.label.text.color         = t1;
    s.typography.body.color    = t1;
    s.typography.caption.color = t2;

    return s;
}

}  // namespace pk
