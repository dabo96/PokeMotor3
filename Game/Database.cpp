// Game/Database.cpp — carga de especies (JSON opcional + defaults).
#include "Game/Database.h"

#include "Core/Log.h"
#include "Core/Project.h"

#include <json.hpp>

#include <cstdio>
#include <fstream>

namespace pk {

namespace {
// Convención cuando el JSON no declara "sprite": la carpeta de sprites + el id a 3
// dígitos. Un único sitio con la ruta, para no repartir literales por el gameplay.
std::string spritePathForId(int id) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Assets/Models/Sprites/%03d.png", id);
    return buf;
}
}  // namespace

void Database::load(const std::string& path) {
    m_species.clear();
    std::ifstream f(Project::instance().resolveRead(path));
    if (!f) {
        // Proyecto sin datos de especies: no hay encuentros. El motor no inventa una
        // lista propia — el contenido lo pone el proyecto (o su plantilla).
        LOG_INFO("Database: '%s' no encontrado; sin especies (no habrá encuentros).", path.c_str());
        return;
    }
    try {
        nlohmann::json j;
        f >> j;
        m_species.clear();
        if (j.is_object()) {
            // Formato del proyecto: objeto { "Nombre": { "id": N, "sprite": "...", ... }, ... }.
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (!it.key().empty() && it.key()[0] == '_') continue;   // "_comment" y demás notas
                m_species.push_back({ it.value().value("id", 0), it.key(),
                                      it.value().value("sprite", std::string()) });
            }
        } else if (j.is_array()) {
            // Formato alterno: array [ { "id", "name", "sprite" }, ... ].
            for (const auto& e : j)
                m_species.push_back({ e.value("id", 0), e.value("name", std::string("???")),
                                      e.value("sprite", std::string()) });
        }
        // Sin "sprite" declarado, cae en la convención por id (nunca queda vacío).
        for (Species& s : m_species)
            if (s.sprite.empty()) s.sprite = spritePathForId(s.id);
        LOG_INFO("Database: %zu especies cargadas de '%s'.", m_species.size(), path.c_str());
    } catch (...) {
        LOG_WARN("Database: '%s' corrupto; se ignora (sin especies).", path.c_str());
        m_species.clear();
    }
}

// Sin especies cargadas devuelve un marcador: pick() ya no puede indexar un vector vacío
// (quien la llama debería comprobar empty(), pero esto lo hace seguro igualmente).
const Species& Database::pick(uint32_t r) const {
    static const Species kNone{ 0, "???", std::string() };
    if (m_species.empty()) return kNone;
    return m_species[r % m_species.size()];
}

}  // namespace pk
