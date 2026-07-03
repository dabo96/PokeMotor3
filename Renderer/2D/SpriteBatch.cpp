// Renderer/2D/SpriteBatch.cpp — ordenado + generación de quads/runs.
#include "Renderer/2D/SpriteBatch.h"

#include <algorithm>
#include <cmath>

namespace pk {

void SpriteBatch::clear() {
    m_sprites.clear();
    m_verts.clear();
    m_runs.clear();
}

void SpriteBatch::build() {
    m_verts.clear();
    m_runs.clear();
    if (m_sprites.empty()) return;

    // Orden estable por (capa, textura): así los sprites de la misma textura quedan
    // contiguos y se agrupan en un solo draw, respetando el pintado por capas.
    std::stable_sort(m_sprites.begin(), m_sprites.end(),
        [](const Sprite& a, const Sprite& b) {
            if (a.layer != b.layer) return a.layer < b.layer;
            if (a.texture.index != b.texture.index) return a.texture.index < b.texture.index;
            return a.texture.generation < b.texture.generation;
        });

    m_verts.reserve(m_sprites.size() * 6);
    SpriteRun run{};
    bool haveRun = false;

    for (const Sprite& s : m_sprites) {
        // Abre un run nuevo al cambiar de textura.
        if (!haveRun || !(s.texture == run.texture)) {
            if (haveRun) m_runs.push_back(run);
            run.texture     = s.texture;
            run.firstVertex = static_cast<uint32_t>(m_verts.size());
            run.vertexCount = 0;
            haveRun = true;
        }

        const float x0 = s.position.x,            y0 = s.position.y;
        const float x1 = s.position.x + s.size.x, y1 = s.position.y + s.size.y;
        const float u0 = s.uvRect.x,              v0 = s.uvRect.y;
        const float u1 = s.uvRect.x + s.uvRect.z, v1 = s.uvRect.y + s.uvRect.w;

        // Rotación alrededor del centro del sprite (0 = sin coste extra real).
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
        const float cs = std::cos(s.rotation), sn = std::sin(s.rotation);
        auto rot = [&](float px, float py) -> Vec2 {
            const float dx = px - cx, dy = py - cy;
            return { cx + dx * cs - dy * sn, cy + dx * sn + dy * cs };
        };

        const Vertex2D tl{ rot(x0, y0), { u0, v0 }, s.color };
        const Vertex2D tr{ rot(x1, y0), { u1, v0 }, s.color };
        const Vertex2D br{ rot(x1, y1), { u1, v1 }, s.color };
        const Vertex2D bl{ rot(x0, y1), { u0, v1 }, s.color };

        // Dos triángulos (sin índices: el batch prioriza simplicidad).
        m_verts.push_back(tl); m_verts.push_back(tr); m_verts.push_back(br);
        m_verts.push_back(tl); m_verts.push_back(br); m_verts.push_back(bl);
        run.vertexCount += 6;
    }
    if (haveRun) m_runs.push_back(run);
}

}  // namespace pk
