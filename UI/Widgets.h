// UI/Widgets.h — widgets concretos de la UI del juego. Fase 2a: Panel (caja 9-slice)
// y Label (texto). Menu/Image llegan en la 2b. Diseño: MotorGrafico_UIJuego.md.
#pragma once

#include "UI/UIInput.h"
#include "UI/UIRenderer.h"
#include "UI/UITheme.h"
#include "UI/Widget.h"

#include <functional>
#include <string>
#include <vector>

namespace pk {

// Caja con fondo 9-slice del tema (o uno propio). Contenedor de otros widgets.
class Panel : public Widget {
public:
    explicit Panel(const UITheme& theme) : m_theme(&theme) {}

    bool      useThemeBackground = true;   // false → usa 'background'
    NineSlice background;                  // marco propio (si no se usa el del tema)
    int       layer = 0;                   // orden de pintado del fondo

protected:
    void onDraw(UIRenderer& r) override {
        r.drawNineSlice(useThemeBackground ? m_theme->panel : background, rect, layer);
    }

private:
    const UITheme* m_theme;
};

// Texto. Usa tamaño/color del tema por defecto; se pueden sobreescribir. El texto se
// dibuja desde la esquina sup-izq del rect.
class Label : public Widget {
public:
    explicit Label(const UITheme& theme) : m_theme(&theme) {}

    std::string text;
    float       pixelSize = 0.0f;            // 0 = theme.fontSize
    Vec4        color{ 0.0f, 0.0f, 0.0f, 0.0f };  // alpha 0 = theme.textColor

protected:
    void onDraw(UIRenderer& r) override {
        if (text.empty()) return;
        const float sz  = pixelSize > 0.0f ? pixelSize : m_theme->fontSize;
        const Vec4  col = color.a > 0.0f ? color : m_theme->textColor;
        r.drawText(text, Vec2(rect.x, rect.y), sz, col);
    }

private:
    const UITheme* m_theme;
};

// Icono/retrato/cursor: un sprite en screen-space.
class Image : public Widget {
public:
    TextureHandle texture;
    Vec4 uv{ 0.0f, 0.0f, 1.0f, 1.0f };       // sub-rect (x, y, w, h) del atlas
    Vec4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
    int  layer = 0;

protected:
    void onDraw(UIRenderer& r) override { r.drawSprite(texture, rect, uv, color, layer); }
};

// Lista navegable con cursor (el núcleo interactivo de la UI). Up/Down mueve la
// selección (con wrap); Confirm/Cancel disparan los callbacks. La opción activa lleva
// un realce y un cursor ">"; sin asset de cursor todavía.
class Menu : public Widget {
public:
    explicit Menu(const UITheme& theme) : m_theme(&theme) {}

    std::vector<std::string> options;
    int   selected   = 0;
    float itemHeight = 16.0f;
    int   layer      = 1;                       // realce SOBRE el panel de fondo
    std::function<void(int)> onConfirm;
    std::function<void()>    onCancel;

protected:
    void onDraw(UIRenderer& r) override {
        for (size_t i = 0; i < options.size(); ++i) {
            const float y   = rect.y + static_cast<float>(i) * itemHeight;
            const bool  sel = (static_cast<int>(i) == selected);
            if (sel)
                r.drawRect(Rect{ rect.x, y, rect.w, itemHeight }, m_theme->highlightColor, layer);
            const Vec4 col = sel ? Vec4(1.0f, 1.0f, 1.0f, 1.0f) : m_theme->textColor;
            r.drawText(options[i], Vec2(rect.x + 12.0f, y + 3.0f), m_theme->fontSize, col);
            if (sel)
                r.drawText(">", Vec2(rect.x + 2.0f, y + 3.0f), m_theme->fontSize, m_theme->textColor);
        }
    }

    bool onInput(const UIInput& in) override {
        if (options.empty()) return false;
        const int n = static_cast<int>(options.size());
        bool used = false;
        if (in.down)    { selected = (selected + 1) % n;         used = true; }
        if (in.up)      { selected = (selected - 1 + n) % n;     used = true; }
        if (in.confirm) { if (onConfirm) onConfirm(selected);    used = true; }
        if (in.cancel)  { if (onCancel)  onCancel();             used = true; }
        return used;
    }

private:
    const UITheme* m_theme;
};

}  // namespace pk
