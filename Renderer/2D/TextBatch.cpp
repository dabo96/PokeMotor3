// Renderer/2D/TextBatch.cpp — layout de glifos MSDF a Vertex2D.
#include "Renderer/2D/TextBatch.h"

#include "Renderer/Text/MsdfFont.h"

namespace pk {

void TextBatch::clear() {
    m_items.clear();
    m_verts.clear();
}

void TextBatch::build(const MsdfFont& font) {
    m_verts.clear();
    if (m_items.empty() || !font.valid()) return;

    for (const TextItem& it : m_items) {
        const float factor   = it.pixelSize / font.emSize();   // unidad de fuente → px
        const float lineStep = font.lineHeight() * it.pixelSize;
        float baseY = it.pos.y + font.ascender() * it.pixelSize;   // baseline de la 1ª línea
        float penX  = it.pos.x;
        uint32_t prev = 0;

        for (size_t i = 0; i < it.text.size();) {
            const uint32_t cp = utf8Next(it.text, i);
            if (cp == '\n') {
                baseY += lineStep;
                penX   = it.pos.x;
                prev   = 0;
                continue;
            }
            if (prev) penX += font.kerning(prev, cp) * factor;

            if (const MsdfFont::Glyph* g = font.glyph(cp)) {
                // plane* (em, +Y arriba, baseline-relativo) → píxeles (Y abajo).
                const float l = penX + g->planeLeft  * factor;
                const float r = penX + g->planeRight * factor;
                const float t = baseY - g->planeTop    * factor;   // top: menor y
                const float b = baseY - g->planeBottom * factor;   // bottom: mayor y

                const Vertex2D tl{ { l, t }, { g->uv0.x, g->uv0.y }, it.color };
                const Vertex2D tr{ { r, t }, { g->uv1.x, g->uv0.y }, it.color };
                const Vertex2D br{ { r, b }, { g->uv1.x, g->uv1.y }, it.color };
                const Vertex2D bl{ { l, b }, { g->uv0.x, g->uv1.y }, it.color };
                m_verts.push_back(tl); m_verts.push_back(tr); m_verts.push_back(br);
                m_verts.push_back(tl); m_verts.push_back(br); m_verts.push_back(bl);
            }

            penX += font.advanceOf(cp) * factor;
            prev  = cp;
        }
    }
}

}  // namespace pk
