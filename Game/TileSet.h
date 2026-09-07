// Game/TileSet.h — registro DATA-DRIVEN de tipos de tile + metadatos del atlas.
// Los tipos ya no están fijos en C++: se cargan de Assets/Data/tileset.json (lista
// de { name, cell, walkable, encounter, color }). Si el archivo no existe el registro
// queda VACÍO (proyecto en blanco): los tipos se crean en el editor de tiles. El TileMap
// guarda por celda un ÍNDICE (uint8_t) hacia este registro; ver Game/Tile.h.
#pragma once

#include "Core/Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pk {

// Definición de un tipo de tile (una entrada del tileset.json).
struct TileTypeDef {
    std::string name      = "Tile";
    int         cell      = 0;        // celda en el atlas del tileset (índice lineal)
    bool        walkable  = true;     // ¿se puede pisar?
    bool        encounter = false;    // ¿dispara encuentros (hierba alta)?
    Vec4        color{ 1.0f, 1.0f, 1.0f, 1.0f };  // color plano de respaldo (sin atlas)
    // Nombre del GRUPO al que pertenece (vacío = tile suelto). Es solo ORGANIZATIVO: los
    // tipos de un grupo (p.ej. tronco+copa de un "Árbol") se pintan y renderizan igual que
    // los sueltos, celda a celda; el grupo solo los clusteriza en la lista del editor y
    // permite fijar "pisable" de golpe a todos sus miembros. No cambia mapa/runtime/colisión.
    std::string group;
};

// Registro global (singleton). El primer acceso carga Assets/Data/tileset.json.
class TileSet {
public:
    static TileSet& instance();

    bool load(const std::string& path);  // (re)carga desde JSON; true si lo leyó bien
    void ensureLoaded();                  // carga perezosa (registro vacío si no hay archivo)

    int                count() const { return static_cast<int>(m_types.size()); }
    const TileTypeDef& at(int i) const;   // acceso con clamp a rango válido

    // --- Grupos (organización de la lista del editor; no afectan mapa/runtime) ---
    // Nombres de grupo únicos por ORDEN DE APARICIÓN en m_types (sin incluir el vacío).
    std::vector<std::string> groupOrder() const;
    // Fija 'walkable' en TODOS los tipos del grupo (comodidad "un flag para todo el grupo").
    void                     setGroupWalkable(const std::string& group, bool walkable);

    // --- Mutación (para el editor de tiles) ---
    TileTypeDef& editType(int i);                 // acceso editable con clamp a rango válido
    int          addType(const TileTypeDef& d);   // añade un tipo; devuelve su índice
    bool         removeType(int i);               // borra el tipo i (mantiene >=1). OJO: el
                                                  // mapa guarda ÍNDICES — quien borre debe
                                                  // remapear las celdas del mapa (lo hace el editor).
    void         setTexture(const std::string& t) { m_texture = t; }
    void         setTileSize(int px);             // tamaño de tile en px (cuadrado); rededuce el grid
    // Escribe el registro a JSON (mismo formato que load). true si pudo escribir.
    bool         save(const std::string& path = "Assets/Data/tileset.json") const;

    const std::string& texture()    const { return m_texture; }
    int                columns()    const { return m_cols; }
    int                rows()       const { return m_rows; }
    int                tileWidth()  const { return m_tileW; }
    int                tileHeight() const { return m_tileH; }

    // Deduce columns/rows a partir del tamaño REAL de la imagen y el tamaño de tile
    // (columns = texW/tileW, rows = texH/tileH). Quien carga la textura lo llama con sus
    // dimensiones. Si el JSON fijó columns/rows a mano, se respetan (no recalcula).
    void resolveGrid(int texW, int texH);

private:
    void setEmpty();

    std::vector<TileTypeDef> m_types;
    std::string              m_texture      = "Assets/Textures/tileset.png";
    int                      m_cols         = 8;       // derivado (o override del JSON)
    int                      m_rows         = 8;
    int                      m_tileW        = 16;      // tamaño de tile en px (del JSON)
    int                      m_tileH        = 16;
    bool                     m_gridExplicit = false;   // el JSON fijó columns/rows a mano
    bool                     m_loaded       = false;
};

}  // namespace pk
