// UI/UIRenderer.h — fachada de dibujo screen-space de la UI del juego. Acumula
// sprites + textos en píxeles de pantalla (lowRes 480x270, +Y abajo) durante el frame
// y al hacer flush() los vuelca al Renderer (setUISprites + setTexts). Mide el texto
// con la fuente MSDF del Renderer para hacer layout. Diseño: MotorGrafico_UIJuego.md.
#pragma once

#include "Core/Handle.h"
#include "Core/Math.h"
#include "Renderer/2D/Sprite.h"
#include "Renderer/2D/TextBatch.h"   // TextItem
#include "UI/NineSlice.h"
#include "UI/Rect.h"

#include <string>
#include <vector>

namespace pk {

class Renderer;

class UIRenderer {
public:
    explicit UIRenderer(Renderer& r) : m_renderer(r) {}

    void begin();   // limpia los acumuladores del frame
    void flush();   // vuelca lo acumulado al Renderer (setUISprites + setTexts)

    // Todo en píxeles de pantalla. layer ordena el pintado (mayor = más arriba).
    void drawRect(const Rect& rc, const Vec4& color, int layer = 0);
    void drawSprite(TextureHandle tex, const Rect& rc, const Vec4& uv, const Vec4& color, int layer = 0);
    void drawNineSlice(const NineSlice& ns, const Rect& rc, int layer = 0);
    void drawText(const std::string& text, Vec2 pos, float pixelSize, const Vec4& color);

    // Medidas (para layout: centrar, ajustar cajas al texto…).
    float measureText(const std::string& text, float pixelSize) const;
    float lineHeight(float pixelSize) const;

    // Resolución lógica de la UI (target lowRes). Anclar a estos bordes.
    static constexpr float kScreenW = 480.0f;
    static constexpr float kScreenH = 270.0f;

private:
    Renderer&             m_renderer;
    std::vector<Sprite>   m_sprites;
    std::vector<TextItem> m_texts;
};

}  // namespace pk
