// Game/Database.h — capa de datos (flyweight). Para la demo: lista de especies
// para nombrar encuentros. Carga de JSON si existe, si no usa una lista por
// defecto (nunca queda vacía). Diseño: MotorGrafico_IndiceMaestro.md (Fase 4).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pk {

struct Species {
    int         id;
    std::string name;
};

class Database {
public:
    // Intenta leer JSON (array de {id,name}); ante cualquier fallo usa defaults.
    void load(const std::string& path = "Assets/Data/species.json");

    bool   empty() const { return m_species.empty(); }
    size_t count() const { return m_species.size(); }
    const Species& pick(uint32_t r) const;   // r = número aleatorio cualquiera

private:
    void loadDefaults();
    std::vector<Species> m_species;
};

}  // namespace pk
