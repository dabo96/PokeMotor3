// Game/Database.cpp — carga de especies (JSON opcional + defaults).
#include "Game/Database.h"

#include "Core/Log.h"
#include "Core/Project.h"

#include <json.hpp>

#include <fstream>

namespace pk {

void Database::loadDefaults() {
    m_species = {
        { 1,   "Bulbasaur" }, { 4,  "Charmander" }, { 7,  "Squirtle" },
        { 10,  "Caterpie"  }, { 16, "Pidgey"     }, { 19, "Rattata"  },
        { 25,  "Pikachu"   }, { 43, "Oddish"     }, { 129,"Magikarp" },
    };
}

void Database::load(const std::string& path) {
    std::ifstream f(Project::instance().resolveRead(path));
    if (!f) {
        LOG_INFO("Database: '%s' no encontrado; usando lista por defecto.", path.c_str());
        loadDefaults();
        return;
    }
    try {
        nlohmann::json j;
        f >> j;
        m_species.clear();
        if (j.is_object()) {
            // Formato del proyecto: objeto { "Nombre": { "id": N, ... }, ... }.
            for (auto it = j.begin(); it != j.end(); ++it)
                m_species.push_back({ it.value().value("id", 0), it.key() });
        } else if (j.is_array()) {
            // Formato alterno: array [ { "id", "name" }, ... ].
            for (const auto& e : j)
                m_species.push_back({ e.value("id", 0), e.value("name", std::string("???")) });
        }
        if (m_species.empty()) loadDefaults();
        else LOG_INFO("Database: %zu especies cargadas de '%s'.", m_species.size(), path.c_str());
    } catch (...) {
        LOG_WARN("Database: '%s' corrupto; usando lista por defecto.", path.c_str());
        loadDefaults();
    }
}

const Species& Database::pick(uint32_t r) const {
    return m_species[r % m_species.size()];
}

}  // namespace pk
