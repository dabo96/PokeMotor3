// UI/UIRenderer.cpp — implementación de la fachada screen-space.
#include "UI/UIRenderer.h"

#include "Renderer/Renderer.h"

namespace pk {

void UIRenderer::begin() {
    m_sprites.clear();
    m_texts.clear();
}

void UIRenderer::flush() {
    m_renderer.setUISprites(m_sprites);
    m_renderer.setTexts(m_texts);
}

void UIRenderer::drawRect(const Rect& rc, const Vec4& color, int layer) {
    drawSprite(TextureHandle{}, rc, Vec4(0.0f, 0.0f, 1.0f, 1.0f), color, layer);  // textura inválida → blanca
}

void UIRenderer::drawSprite(TextureHandle tex, const Rect& rc, const Vec4& uv, const Vec4& color, int layer) {
    Sprite s;
    s.position = Vec2(rc.x, rc.y);
    s.size     = Vec2(rc.w, rc.h);
    s.uvRect   = uv;            // (u, v, ancho, alto) en [0,1] del atlas
    s.color    = color;
    s.texture  = tex;
    s.layer    = layer;
    m_sprites.push_back(s);
}

void UIRenderer::drawNineSlice(const NineSlice& ns, const Rect& rc, int layer) {
    // Sin textura → caja de color plano (suficiente hasta tener el marco como asset).
    if (!ns.texture.valid() || ns.texSize.x <= 0.0f || ns.texSize.y <= 0.0f) {
        drawRect(rc, ns.tint, layer);
        return;
    }
    const float tw = ns.texSize.x, th = ns.texSize.y;
    // Cortes en X/Y del rect destino (esquinas a tamaño fijo, centro estirado).
    const float xs[4] = { rc.x, rc.x + ns.left, rc.right() - ns.right, rc.right() };
    const float ys[4] = { rc.y, rc.y + ns.top,  rc.bottom() - ns.bottom, rc.bottom() };
    // Cortes equivalentes en UV (fracción de la textura).
    const float us[4] = { 0.0f, ns.left / tw, 1.0f - ns.right / tw, 1.0f };
    const float vs[4] = { 0.0f, ns.top / th,  1.0f - ns.bottom / th, 1.0f };

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const Rect cell{ xs[col], ys[row], xs[col + 1] - xs[col], ys[row + 1] - ys[row] };
            if (cell.w <= 0.0f || cell.h <= 0.0f) continue;   // rect más chico que los márgenes
            const Vec4 uv(us[col], vs[row], us[col + 1] - us[col], vs[row + 1] - vs[row]);
            drawSprite(ns.texture, cell, uv, ns.tint, layer);
        }
    }
}

void UIRenderer::drawText(const std::string& text, Vec2 pos, float pixelSize, const Vec4& color) {
    TextItem t;
    t.text      = text;
    t.pos       = pos;
    t.pixelSize = pixelSize;
    t.color     = color;
    m_texts.push_back(t);
}

float UIRenderer::measureText(const std::string& text, float pixelSize) const {
    return m_renderer.uiFont().measure(text, pixelSize);
}

float UIRenderer::lineHeight(float pixelSize) const {
    return m_renderer.uiFont().lineHeight() * pixelSize;
}

}  // namespace pk
