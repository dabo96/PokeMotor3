// Renderer/2D/TextBatch.h — convierte cadenas en quads de glifos MSDF (Vertex2D),
// análogo a SpriteBatch pero para texto. Todo el texto comparte el atlas de la fuente
// → un único buffer de vértices. Puro CPU; el Renderer sube el buffer y emite el draw
// con el pipeline text2d. Trabaja en espacio de PANTALLA (píxeles, +Y hacia abajo);
// pos = esquina superior-izquierda de la primera línea. Plan MotorGrafico_UIJuego.md.
#pragma once

#include "Core/Math.h"
#include "Renderer/2D/Sprite.h"   // Vertex2D

#include <string>
#include <vector>

namespace pk {

class MsdfFont;

// Un texto a dibujar. \n separa líneas (avanza lineHeight * pixelSize).
struct TextItem {
    std::string text;
    Vec2        pos{ 0.0f, 0.0f };   // esquina superior-izquierda, en píxeles de pantalla
    float       pixelSize = 16.0f;   // alto del em en píxeles
    Vec4        color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

class TextBatch {
public:
    void clear();
    void add(const TextItem& t) { m_items.push_back(t); }
    void build(const MsdfFont& font);   // genera los vértices de todos los items

    const std::vector<Vertex2D>& vertices() const { return m_verts; }
    bool  empty() const { return m_verts.empty(); }

private:
    std::vector<TextItem> m_items;
    std::vector<Vertex2D> m_verts;
};

}  // namespace pk
