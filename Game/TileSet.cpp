// Game/TileSet.cpp — carga del registro de tipos de tile.
#include "Game/TileSet.h"

#include "Core/Log.h"
#include "Core/Project.h"

#include <json.hpp>

#include <algorithm>
#include <fstream>

namespace pk {

TileSet& TileSet::instance() {
    static TileSet s;
    s.ensureLoaded();   // garantiza datos válidos en cualquier acceso (idempotente)
    return s;
}

// Registro VACÍO: es lo que tiene un proyecto sin tileset.json. Los tipos los da de alta el
// usuario en el editor de tiles (o vienen con la plantilla). El motor ya no inventa cinco.
void TileSet::setEmpty() {
    m_types.clear();
    m_texture.clear();
    m_cols = 8;
    m_rows = 8;
    m_tileW = 16;
    m_tileH = 16;
    m_gridExplicit = false;
}

void TileSet::setTileSize(int px) {
    if (px < 1) px = 1;
    m_tileW = m_tileH = px;
    m_gridExplicit = false;   // que columns/rows se rededuzcan del tamaño real de la imagen
}

void TileSet::resolveGrid(int texW, int texH) {
    if (m_gridExplicit) return;            // columns/rows fijados a mano en el JSON
    if (m_tileW < 1 || m_tileH < 1) return;
    if (texW > 0) m_cols = std::max(1, texW / m_tileW);
    if (texH > 0) m_rows = std::max(1, texH / m_tileH);
}

const TileTypeDef& TileSet::at(int i) const {
    static const TileTypeDef fallback{};
    if (m_types.empty()) return fallback;
    if (i < 0) i = 0;
    if (i >= static_cast<int>(m_types.size())) i = static_cast<int>(m_types.size()) - 1;
    return m_types[i];
}

TileTypeDef& TileSet::editType(int i) {
    if (m_types.empty()) m_types.push_back(TileTypeDef{});   // garantiza un destino válido
    if (i < 0) i = 0;
    if (i >= static_cast<int>(m_types.size())) i = static_cast<int>(m_types.size()) - 1;
    return m_types[i];
}

int TileSet::addType(const TileTypeDef& d) {
    m_types.push_back(d);
    return static_cast<int>(m_types.size()) - 1;
}

bool TileSet::removeType(int i) {
    if (i < 0 || i >= static_cast<int>(m_types.size())) return false;
    if (m_types.size() <= 1) return false;   // siempre queda al menos un tipo
    m_types.erase(m_types.begin() + i);
    return true;
}

// Nombres de grupo únicos por orden de aparición (omite los tipos sueltos, group vacío).
std::vector<std::string> TileSet::groupOrder() const {
    std::vector<std::string> out;
    for (const auto& d : m_types) {
        if (d.group.empty()) continue;
        if (std::find(out.begin(), out.end(), d.group) == out.end()) out.push_back(d.group);
    }
    return out;
}

void TileSet::setGroupWalkable(const std::string& group, bool walkable) {
    if (group.empty()) return;
    for (auto& d : m_types)
        if (d.group == group) d.walkable = walkable;
}

bool TileSet::save(const std::string& path) const {
    nlohmann::json j;
    j["texture"] = m_texture;
    // Tamaño de tile: si es cuadrado, un solo campo; si no, ancho/alto por separado.
    if (m_tileW == m_tileH) {
        j["tileSize"] = m_tileW;
    } else {
        j["tileWidth"]  = m_tileW;
        j["tileHeight"] = m_tileH;
    }
    // columns/rows solo si fueron explícitos (override manual); si no, se deducen al cargar.
    if (m_gridExplicit) {
        j["columns"] = m_cols;
        j["rows"]    = m_rows;
    }
    nlohmann::json types = nlohmann::json::array();
    for (const auto& d : m_types) {
        nlohmann::json t = {
            { "name",      d.name },
            { "cell",      d.cell },
            { "walkable",  d.walkable },
            { "encounter", d.encounter },
            { "color",     { d.color.x, d.color.y, d.color.z, d.color.w } },
        };
        if (!d.group.empty()) t["group"] = d.group;   // solo si pertenece a un grupo
        types.push_back(std::move(t));
    }
    j["types"] = types;

    std::ofstream f(Project::instance().resolveWrite(path));
    if (!f) { LOG_WARN("TileSet: no se pudo escribir '%s'.", path.c_str()); return false; }
    f << j.dump(2);
    LOG_INFO("TileSet: guardado '%s' (%d tipos).", path.c_str(), static_cast<int>(m_types.size()));
    return true;
}

void TileSet::ensureLoaded() {
    if (m_loaded) return;
    load("Assets/Data/tileset.json");   // deja defaults si el archivo no existe
    m_loaded = true;
}

bool TileSet::load(const std::string& path) {
    std::ifstream f(Project::instance().resolveRead(path));
    if (!f) { setEmpty(); return false; }   // sin archivo → registro vacío (no es error)
    try {
        nlohmann::json j;
        f >> j;
        m_texture = j.value("texture", std::string());

        // Tamaño de tile en px (cuadrado por defecto; tileWidth/tileHeight lo separan).
        m_tileW = m_tileH = std::max(1, j.value("tileSize", 16));
        if (j.contains("tileWidth"))  m_tileW = std::max(1, j.value("tileWidth", m_tileW));
        if (j.contains("tileHeight")) m_tileH = std::max(1, j.value("tileHeight", m_tileH));

        // columns/rows: por defecto se deducen de la imagen (resolveGrid); si el JSON los
        // trae explícitos, mandan ellos (override manual).
        m_gridExplicit = j.contains("columns") && j.contains("rows");
        m_cols = std::max(1, j.value("columns", 8));
        m_rows = std::max(1, j.value("rows", 8));

        m_types.clear();
        if (j.contains("types") && j["types"].is_array()) {
            for (const auto& t : j["types"]) {
                TileTypeDef d;
                d.name      = t.value("name", std::string("Tile"));
                d.cell      = t.value("cell", 0);
                d.walkable  = t.value("walkable", true);
                d.encounter = t.value("encounter", false);
                d.group     = t.value("group", std::string());   // vacío = tile suelto
                if (t.contains("color") && t["color"].is_array() && t["color"].size() >= 3) {
                    const auto& col = t["color"];
                    d.color = Vec4(col[0].get<float>(), col[1].get<float>(), col[2].get<float>(),
                                   col.size() >= 4 ? col[3].get<float>() : 1.0f);
                }
                m_types.push_back(d);
            }
        }
        m_loaded = true;
        LOG_INFO("TileSet: %d tipos cargados de '%s'.", static_cast<int>(m_types.size()), path.c_str());
        return true;
    } catch (...) {
        setEmpty();
        return false;
    }
}

}  // namespace pk
