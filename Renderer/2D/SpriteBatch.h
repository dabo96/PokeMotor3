// Renderer/2D/SpriteBatch.h — convierte miles de sprites en pocos draw calls.
// Diseño: MotorGrafico_2DyCamara.md. Acumula sprites, los ordena por (capa, atlas)
// y genera un buffer de vértices + "runs": cada tramo contiguo con la misma textura
// = una llamada de dibujo. Es puro CPU (agnóstico de Vulkan); el Renderer sube el
// buffer y emite los draws.
#pragma once

#include "Core/Handle.h"
#include "Renderer/2D/Sprite.h"

#include <cstdint>
#include <vector>

namespace pk {

// Un tramo de vértices que comparten textura → un solo vkCmdDraw.
struct SpriteRun {
    TextureHandle texture;
    uint32_t      firstVertex = 0;
    uint32_t      vertexCount = 0;
};

class SpriteBatch {
public:
    void clear();
    void add(const Sprite& s) { m_sprites.push_back(s); }
    void build();   // ordena, genera vértices y runs

    const std::vector<Vertex2D>&  vertices() const { return m_verts; }
    const std::vector<SpriteRun>& runs()     const { return m_runs; }
    bool  empty() const { return m_verts.empty(); }

private:
    std::vector<Sprite>    m_sprites;
    std::vector<Vertex2D>  m_verts;
    std::vector<SpriteRun> m_runs;
};

}  // namespace pk
