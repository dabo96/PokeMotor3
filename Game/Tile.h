// Game/Tile.h — tipo de una celda del mapa y sus propiedades.
// DATA-DRIVEN: el tipo es ahora un ÍNDICE (uint8_t) hacia el registro TileSet, que se
// carga de Assets/Data/tileset.json. Las propiedades (walkable/encounter/color/celda)
// se derivan de ahí; ya no están fijas en C++. Se conserva el nombre `TileType` por
// compatibilidad con el resto del motor (TileMap, serialización, editor).
#pragma once

#include "Core/Math.h"
#include "Game/TileSet.h"

#include <cstdint>

namespace pk {

// El tipo de una celda = índice dentro del TileSet.
using TileType = std::uint8_t;

// Índice del primer tipo: actúa como "vacío/suelo por defecto" (borrar, relleno inicial).
inline constexpr TileType kTileDefault = 0;

// Propiedades efectivas de un tipo (derivadas del registro). Antes era un flyweight
// hardcodeado; ahora se construye al vuelo desde el TileTypeDef.
struct TileProps {
    bool walkable;
    bool encounter;
    Vec4 color;       // color plano de respaldo (sin atlas)
    int  atlasCell;   // celda en el tileset/atlas
};

inline TileProps tileProps(TileType t) {
    const TileTypeDef& d = TileSet::instance().at(static_cast<int>(t));
    return { d.walkable, d.encounter, d.color, d.cell };
}

// Número de tipos definidos (para validar/iterar la paleta).
inline int tileTypeCount() { return TileSet::instance().count(); }

}  // namespace pk
