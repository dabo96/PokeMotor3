// Game/TileMap.h — la grilla del mundo: un TileType por celda + dónde arranca el
// jugador. Renderer-agnóstico: el OverworldMode lo traduce a entidades/sprites.
// Diseño: MotorGrafico_2DyCamara.md ("el Sprite2DRenderer lee el TileMap").
#pragma once

#include "Core/Math.h"
#include "Game/Tile.h"

#include <string>
#include <vector>

namespace pk {

class TileMap {
public:
    // Carga desde filas ASCII. Símbolos:
    //   '.' Path   'g' Grass   'G' TallGrass   'w' Water   'T' Tree
    //   'P' inicio del jugador (la celda queda como Path)
    void loadAscii(const std::vector<std::string>& rows);

    int  width()  const { return m_w; }
    int  height() const { return m_h; }
    bool inBounds(int x, int y) const { return x >= 0 && y >= 0 && x < m_w && y < m_h; }

    TileType at(int x, int y) const;                                  // fuera de límites → tipo 0
    void     set(int x, int y, TileType t);                           // edición (editor)

    // Reemplaza el contenido en memoria (deserializar el TileMapComponent / copia del
    // editor). 'tiles' debe medir w*h; si no, se rellena con el tipo por defecto.
    void     assign(int w, int h, std::vector<TileType> tiles, IVec2 start);

    bool saveJson(const std::string& path) const;                     // T1: persistencia del mapa
    bool loadJson(const std::string& path);                           // devuelve false si no existe/corrupto
    // Fuera de límites = muro infranqueable (el borde del mapa siempre bloquea).
    bool walkable(int x, int y)    const { return inBounds(x, y) && tileProps(at(x, y)).walkable; }
    bool isEncounter(int x, int y) const { return inBounds(x, y) && tileProps(at(x, y)).encounter; }

    IVec2 playerStart() const { return m_start; }

private:
    int                   m_w = 0;
    int                   m_h = 0;
    std::vector<TileType> m_tiles;
    IVec2                 m_start{ 0, 0 };
};

}  // namespace pk
