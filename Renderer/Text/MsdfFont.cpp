// Renderer/Text/MsdfFont.cpp — carga del atlas msdfgen (JSON + PNG).
#include "Renderer/Text/MsdfFont.h"

#include "Assets/AssetManager.h"
#include "Core/Log.h"

#include <json.hpp>

#include <fstream>

namespace pk {

uint32_t utf8Next(const std::string& s, size_t& i) {
    if (i >= s.size()) return 0;
    const unsigned char c0 = static_cast<unsigned char>(s[i]);
    auto cont = [&](size_t k) -> bool {
        return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    };
    if (c0 < 0x80) { i += 1; return c0; }
    if ((c0 & 0xE0) == 0xC0 && cont(1)) {
        uint32_t cp = ((c0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
        i += 2; return cp;
    }
    if ((c0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
        uint32_t cp = ((c0 & 0x0Fu) << 12) |
                      ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                      (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
        i += 3; return cp;
    }
    if ((c0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
        uint32_t cp = ((c0 & 0x07u) << 18) |
                      ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                      ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
                      (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
        i += 4; return cp;
    }
    i += 1; return c0;   // byte suelto: trátalo como Latin-1
}

namespace {
// El espacio (U+0020) no suele tener glifo en el atlas; avanza un cuarto de em.
constexpr float kSpaceAdvanceEm = 0.25f;
}

bool MsdfFont::load(AssetManager& assets, const std::string& jsonPath,
                    const std::string& imagePath) {
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
        LOG_ERROR("MsdfFont: no se pudo abrir '%s'", jsonPath.c_str());
        return false;
    }

    nlohmann::json j;
    try { f >> j; }
    catch (const std::exception& e) {
        LOG_ERROR("MsdfFont: JSON inválido en '%s' (%s)", jsonPath.c_str(), e.what());
        return false;
    }

    // El atlas es textura de DATOS (distancias firmadas): cargar UNORM, no sRGB, y
    // como recurso interno (no debe aparecer en el navegador de assets del editor).
    m_atlas = assets.loadTexture(imagePath, /*srgb=*/false, /*browsable=*/false);
    if (!m_atlas.valid()) {
        LOG_ERROR("MsdfFont: atlas '%s' no cargó", imagePath.c_str());
        return false;
    }

    const auto& atlas = j.at("atlas");
    const float atlasW = atlas.value("width", 1.0f);
    const float atlasH = atlas.value("height", 1.0f);
    m_distanceRange    = atlas.value("distanceRange", 4.0f);

    if (j.contains("metrics")) {
        const auto& m = j["metrics"];
        m_emSize     = m.value("emSize", 1.0f);
        m_lineHeight = m.value("lineHeight", 1.2f);
        m_ascender   = m.value("ascender", 0.8f);
    }
    if (m_emSize <= 0.0f) m_emSize = 1.0f;

    auto rect = [](const nlohmann::json& o, float& l, float& b, float& r, float& t) {
        l = o.value("left", 0.0f);  b = o.value("bottom", 0.0f);
        r = o.value("right", 0.0f); t = o.value("top", 0.0f);
    };

    for (const auto& g : j.at("glyphs")) {
        Glyph gl;
        gl.valid   = true;
        gl.advance = g.value("advance", 0.0f);
        const uint32_t cp = g.value("unicode", 0u);

        if (g.contains("planeBounds"))
            rect(g["planeBounds"], gl.planeLeft, gl.planeBottom, gl.planeRight, gl.planeTop);
        if (g.contains("atlasBounds")) {
            float al, ab, ar, at;
            rect(g["atlasBounds"], al, ab, ar, at);
            // msdfgen usa origen ABAJO; Vulkan muestrea con V hacia abajo → 1 - y/H.
            gl.uv0 = Vec2(al / atlasW, 1.0f - at / atlasH);   // sup-izq
            gl.uv1 = Vec2(ar / atlasW, 1.0f - ab / atlasH);   // inf-der
        }
        m_glyphs[cp] = gl;
    }

    if (j.contains("kerning")) {
        for (const auto& k : j["kerning"]) {
            const uint32_t a = k.value("unicode1", 0u);
            const uint32_t b = k.value("unicode2", 0u);
            m_kern[(static_cast<uint64_t>(a) << 32) | b] = k.value("advance", 0.0f);
        }
    }

    m_valid = !m_glyphs.empty();
    if (m_valid)
        LOG_INFO("MsdfFont: %zu glifos, %zu pares de kerning (%s)",
                 m_glyphs.size(), m_kern.size(), jsonPath.c_str());
    return m_valid;
}

const MsdfFont::Glyph* MsdfFont::glyph(uint32_t cp) const {
    auto it = m_glyphs.find(cp);
    return (it != m_glyphs.end() && it->second.valid) ? &it->second : nullptr;
}

float MsdfFont::kerning(uint32_t a, uint32_t b) const {
    auto it = m_kern.find((static_cast<uint64_t>(a) << 32) | b);
    return it != m_kern.end() ? it->second : 0.0f;
}

float MsdfFont::advanceOf(uint32_t cp) const {
    if (const Glyph* g = glyph(cp)) return g->advance;
    if (cp == ' ') return kSpaceAdvanceEm * m_emSize;
    return 0.0f;
}

float MsdfFont::measure(const std::string& utf8, float pixelSize) const {
    const float factor = pixelSize / m_emSize;
    float penX = 0.0f;
    uint32_t prev = 0;
    for (size_t i = 0; i < utf8.size();) {
        const uint32_t cp = utf8Next(utf8, i);
        if (prev) penX += kerning(prev, cp) * factor;
        penX += advanceOf(cp) * factor;
        prev = cp;
    }
    return penX;
}

}  // namespace pk
