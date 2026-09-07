// Game/Database.h — capa de datos (flyweight): lista de especies para nombrar los
// encuentros. Sale del JSON del PROYECTO; si no lo hay, queda vacía y no hay encuentros
// (el motor no trae contenido). Diseño: MotorGrafico_IndiceMaestro.md (Fase 4).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pk {

struct Species {
    int         id;
    std::string name;
    // Sprite de combate. Sale del campo "sprite" del JSON; si falta, se deduce por
    // convención de la carpeta de sprites y el id ("Assets/Models/Sprites/001.png").
    std::string sprite;
};

class Database {
public:
    // Lee el JSON del proyecto (array/objeto de especies); si falta o falla, queda vacía.
    void load(const std::string& path = "Assets/Data/species.json");

    bool   empty() const { return m_species.empty(); }
    size_t count() const { return m_species.size(); }
    const Species& pick(uint32_t r) const;   // r = número aleatorio cualquiera

private:
    std::vector<Species> m_species;
};

}  // namespace pk
