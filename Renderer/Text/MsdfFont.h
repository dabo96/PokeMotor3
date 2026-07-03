// Renderer/Text/MsdfFont.h — fuente MSDF para la UI del juego (texto nítido a
// cualquier tamaño). Carga un atlas msdfgen (PNG de distancias + JSON de métricas)
// reutilizando el formato del editor (FluentUI), pero SIN depender de FluentUI: la
// textura se sube por el AssetManager del motor (como UNORM, no sRGB) y las métricas
// se parsean con nlohmann/json. El layout de glifos lo hace TextBatch; el render, el
// pipeline text2d del Renderer. Dependencia del plan MotorGrafico_UIJuego.md (Fase 1).
#pragma once

#include "Core/Handle.h"
#include "Core/Math.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace pk {

class AssetManager;

// Decodifica el siguiente codepoint UTF-8 de s desde i (avanza i). Devuelve el
// codepoint; bytes inválidos se tratan como Latin-1 (avanza 1). Compartido por
// MsdfFont::measure y TextBatch.
uint32_t utf8Next(const std::string& s, size_t& i);

class MsdfFont {
public:
    // Métricas de un glifo. plane* en unidades de fuente (em), relativas al baseline,
    // con +Y hacia ARRIBA (convención msdfgen). uv0 = esquina sup-izq, uv1 = inf-der
    // ya en convención Vulkan (V hacia abajo). advance en unidades de fuente.
    struct Glyph {
        float planeLeft = 0, planeBottom = 0, planeRight = 0, planeTop = 0;
        Vec2  uv0{ 0, 0 }, uv1{ 0, 0 };
        float advance = 0;
        bool  valid   = false;
    };

    // Carga métricas (jsonPath) + atlas (imagePath, UNORM) vía el AssetManager.
    bool load(AssetManager& assets, const std::string& jsonPath, const std::string& imagePath);
    bool valid() const { return m_valid; }

    const Glyph* glyph(uint32_t cp) const;
    float kerning(uint32_t a, uint32_t b) const;     // unidades de fuente (0 si no hay)
    float advanceOf(uint32_t cp) const;              // unidades de fuente (espacio incl.)

    float lineHeight()    const { return m_lineHeight; }    // unidades de fuente
    float ascender()      const { return m_ascender; }      // unidades de fuente
    float emSize()        const { return m_emSize; }
    float distanceRange() const { return m_distanceRange; } // píxeles del atlas (MSDF)
    TextureHandle atlas() const { return m_atlas; }

    // Ancho en píxeles de pantalla del texto a tamaño pixelSize (em = pixelSize px).
    float measure(const std::string& utf8, float pixelSize) const;

private:
    std::unordered_map<uint32_t, Glyph> m_glyphs;
    std::unordered_map<uint64_t, float> m_kern;   // (a<<32 | b) -> advance (em)
    float m_lineHeight    = 1.2f;
    float m_ascender      = 0.8f;
    float m_emSize        = 1.0f;
    float m_distanceRange = 4.0f;
    TextureHandle m_atlas;
    bool  m_valid = false;
};

}  // namespace pk
