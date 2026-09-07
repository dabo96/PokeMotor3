// Game/TileMap.cpp — implementación de la grilla.
#include "Game/TileMap.h"

#include <json.hpp>

#include <algorithm>
#include <fstream>
#include <utility>

namespace pk {

// Mapeo del ASCII de demo a ÍNDICES de tipo (convención del tileset por defecto:
// 0 Camino, 1 Cesped, 2 Hierba alta, 3 Agua, 4 Arbol).
static TileType fromChar(char c) {
    switch (c) {
        case 'g': return 1;
        case 'G': return 2;
        case 'w': return 3;
        case 'T': return 4;
        case 'P':
        case '.':
        default:  return kTileDefault;
    }
}

void TileMap::loadAscii(const std::vector<std::string>& rows) {
    m_h = static_cast<int>(rows.size());
    m_w = 0;
    for (const std::string& r : rows) m_w = std::max(m_w, static_cast<int>(r.size()));

    m_tiles.assign(static_cast<size_t>(m_w) * m_h, kTileDefault);
    for (int y = 0; y < m_h; ++y) {
        const std::string& row = rows[y];
        for (int x = 0; x < static_cast<int>(row.size()); ++x) {
            const char c = row[x];
            if (c == 'P') m_start = { x, y };
            m_tiles[static_cast<size_t>(y) * m_w + x] = fromChar(c);
        }
    }
}

void TileMap::assign(int w, int h, std::vector<TileType> tiles, IVec2 start) {
    m_w = w < 0 ? 0 : w;
    m_h = h < 0 ? 0 : h;
    const size_t n = static_cast<size_t>(m_w) * m_h;
    if (tiles.size() == n) m_tiles = std::move(tiles);
    else                   m_tiles.assign(n, kTileDefault);
    m_start = start;
}

void TileMap::resize(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    std::vector<TileType> nt(static_cast<size_t>(w) * h, kTileDefault);
    const int cw = std::min(w, m_w), ch = std::min(h, m_h);   // región conservada
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x)
            nt[static_cast<size_t>(y) * w + x] = m_tiles[static_cast<size_t>(y) * m_w + x];
    m_tiles = std::move(nt);
    m_w = w;
    m_h = h;
    m_start.x = std::clamp(m_start.x, 0, w - 1);   // reajusta el inicio si quedó fuera
    m_start.y = std::clamp(m_start.y, 0, h - 1);
}

TileType TileMap::at(int x, int y) const {
    if (!inBounds(x, y)) return kTileDefault;   // fuera de límites: colisión la cubre walkable()
    return m_tiles[static_cast<size_t>(y) * m_w + x];
}

void TileMap::set(int x, int y, TileType t) {
    if (!inBounds(x, y)) return;
    m_tiles[static_cast<size_t>(y) * m_w + x] = t;
}

bool TileMap::saveJson(const std::string& path) const {
    nlohmann::json j;
    j["width"]  = m_w;
    j["height"] = m_h;
    j["start"]  = { m_start.x, m_start.y };
    nlohmann::json cells = nlohmann::json::array();
    for (TileType t : m_tiles) cells.push_back(static_cast<int>(t));
    j["tiles"] = std::move(cells);

    std::ofstream f(path);
    if (!f) return false;
    f << j.dump(2);
    return true;
}

bool TileMap::loadJson(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    try {
        nlohmann::json j;
        f >> j;
        const int w = j.value("width", 0);
        const int h = j.value("height", 0);
        if (w <= 0 || h <= 0) return false;
        if (!j.contains("tiles") || !j["tiles"].is_array()) return false;
        const auto& cells = j["tiles"];
        if (static_cast<int>(cells.size()) != w * h) return false;

        m_w = w;
        m_h = h;
        m_tiles.resize(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < m_tiles.size(); ++i) {
            int v = cells[i].get<int>();
            if (v < 0 || v >= tileTypeCount()) v = 0;
            m_tiles[i] = static_cast<TileType>(v);
        }
        if (j.contains("start") && j["start"].is_array() && j["start"].size() == 2)
            m_start = IVec2(j["start"][0].get<int>(), j["start"][1].get<int>());
        else
            m_start = IVec2(0, 0);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace pk
