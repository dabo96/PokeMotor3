// Game/SceneManager.cpp — serialización de la escena a/desde JSON.
#include "Game/SceneManager.h"

#include "Assets/AssetManager.h"
#include "Core/Log.h"
#include "Core/Project.h"
#include "Scene/Components.h"

#include <fstream>

namespace pk {

// (De)serialización por componente. En namespace pk para que nlohmann las encuentre
// por ADL al instanciar registerComponent<T>. tex (handle de runtime) NO se guarda:
// se re-resuelve al cargar desde texturePath (ver init()).
void to_json(nlohmann::json& j, const Transform& t) {
    j = { { "x", t.position.x }, { "y", t.position.y }, { "rot", t.rotation },
          { "sx", t.scale.x }, { "sy", t.scale.y } };
}
void from_json(const nlohmann::json& j, Transform& t) {
    t.position = Vec2(j.value("x", 0.0f), j.value("y", 0.0f));
    t.rotation = j.value("rot", 0.0f);
    t.scale    = Vec2(j.value("sx", 1.0f), j.value("sy", 1.0f));
}

void to_json(nlohmann::json& j, const SpriteComponent& s) {
    j = { { "texture", s.texturePath },
          { "uv",   { s.uvRect.x, s.uvRect.y, s.uvRect.z, s.uvRect.w } },
          { "tint", { s.tint.x, s.tint.y, s.tint.z, s.tint.w } },
          { "layer", s.layer },
          { "filter", static_cast<int>(s.filter) } };   // 0=Pixel, 1=Smooth (calidad del asset)
}
void from_json(const nlohmann::json& j, SpriteComponent& s) {
    s.texturePath = j.value("texture", std::string{});
    if (j.contains("uv") && j["uv"].size() == 4)
        s.uvRect = Vec4(j["uv"][0].get<float>(), j["uv"][1].get<float>(),
                        j["uv"][2].get<float>(), j["uv"][3].get<float>());
    if (j.contains("tint") && j["tint"].size() == 4)
        s.tint = Vec4(j["tint"][0].get<float>(), j["tint"][1].get<float>(),
                      j["tint"][2].get<float>(), j["tint"][3].get<float>());
    s.layer  = j.value("layer", 0);
    s.filter = static_cast<FilterMode>(j.value("filter", static_cast<int>(FilterMode::Smooth)));
}

void to_json(nlohmann::json& j, const NameComponent& n) { j = { { "value", n.value } }; }
void from_json(const nlohmann::json& j, NameComponent& n) { n.value = j.value("value", std::string{}); }

void to_json(nlohmann::json& j, const PlayerTag&) { j = nlohmann::json::object(); }
void from_json(const nlohmann::json&, PlayerTag&) {}

void to_json(nlohmann::json& j, const ScriptComponent& s) { j = { { "path", s.path } }; }
void from_json(const nlohmann::json& j, ScriptComponent& s) { s.path = j.value("path", std::string{}); }

void to_json(nlohmann::json& j, const DialogueComponent& d) { j = { { "text", d.text } }; }
void from_json(const nlohmann::json& j, DialogueComponent& d) { d.text = j.value("text", std::string{}); }

void to_json(nlohmann::json& j, const CameraComponent& c) { j = { { "zoom", c.zoom } }; }
void from_json(const nlohmann::json& j, CameraComponent& c) { c.zoom = j.value("zoom", 32.0f); }

// El mapa de la escena: tipos+inicio (TileMap) + overrides visuales por celda. Mismo
// formato JSON que el antiguo overworld.json (width/height/start/tiles/overrides), ahora
// dentro de la escena. El TileMap se reconstruye en memoria con assign() (sin tocar disco).
void to_json(nlohmann::json& j, const TileMapComponent& c) {
    const TileMap& m = c.map;
    j["width"]  = m.width();
    j["height"] = m.height();
    j["start"]  = { m.playerStart().x, m.playerStart().y };
    nlohmann::json cells = nlohmann::json::array();
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            cells.push_back(static_cast<int>(m.at(x, y)));
    j["tiles"] = std::move(cells);

    const int w = m.width() > 0 ? m.width() : 1;
    nlohmann::json ovs = nlohmann::json::array();
    for (const auto& kv : c.overrides) {
        const int x = kv.first % w, y = kv.first / w;
        const TileXform& xf = kv.second;
        ovs.push_back({ { "x", x }, { "y", y },
                        { "pos",   { xf.position.x, xf.position.y } },
                        { "rot",   xf.rotationDeg },
                        { "scale", { xf.scale.x, xf.scale.y } },
                        { "tint",  { xf.tint.x, xf.tint.y, xf.tint.z, xf.tint.w } },
                        { "layer", xf.layer } });
    }
    j["overrides"] = std::move(ovs);
}
void from_json(const nlohmann::json& j, TileMapComponent& c) {
    const int w = j.value("width", 0), h = j.value("height", 0);
    const size_t n = (w > 0 && h > 0) ? static_cast<size_t>(w) * h : 0;
    std::vector<TileType> tiles(n, kTileDefault);
    if (n > 0 && j.contains("tiles") && j["tiles"].is_array() && j["tiles"].size() == n) {
        for (size_t i = 0; i < n; ++i) {
            int v = j["tiles"][i].get<int>();
            if (v < 0 || v >= tileTypeCount()) v = 0;
            tiles[i] = static_cast<TileType>(v);
        }
    }
    IVec2 start(0, 0);
    if (j.contains("start") && j["start"].is_array() && j["start"].size() == 2)
        start = IVec2(j["start"][0].get<int>(), j["start"][1].get<int>());
    c.map.assign(w, h, std::move(tiles), start);

    c.overrides.clear();
    if (w > 0 && h > 0 && j.contains("overrides") && j["overrides"].is_array()) {
        for (const auto& o : j["overrides"]) {
            const int x = o.value("x", -1), y = o.value("y", -1);
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            TileXform xf;
            xf.position = Vec2(static_cast<float>(x), static_cast<float>(y));
            if (o.contains("pos") && o["pos"].size() == 2)
                xf.position = Vec2(o["pos"][0].get<float>(), o["pos"][1].get<float>());
            xf.rotationDeg = o.value("rot", 0.0f);
            if (o.contains("scale") && o["scale"].size() == 2)
                xf.scale = Vec2(o["scale"][0].get<float>(), o["scale"][1].get<float>());
            if (o.contains("tint") && o["tint"].size() == 4)
                xf.tint = Vec4(o["tint"][0].get<float>(), o["tint"][1].get<float>(),
                               o["tint"][2].get<float>(), o["tint"][3].get<float>());
            xf.layer = o.value("layer", 0);
            c.overrides[y * w + x] = xf;
        }
    }
}

void SceneManager::init(AssetManager* assets) {
    m_assets = assets;
    registerComponent<Transform>("Transform");
    registerComponent<NameComponent>("Name");
    registerComponent<PlayerTag>("PlayerTag");
    registerComponent<ScriptComponent>("Script");
    registerComponent<DialogueComponent>("Dialogue");
    registerComponent<TileMapComponent>("TileMap");
    registerComponent<CameraComponent>("Camera");

    // Sprite: igual que el genérico, pero al cargar resuelve el handle desde la ruta.
    TypeIO io;
    io.name = "Sprite";
    io.has  = [](Scene& s, Entity e) { return s.has<SpriteComponent>(e); };
    io.save = [](Scene& s, Entity e, nlohmann::json& j) { j = s.get<SpriteComponent>(e); };
    io.load = [this](Scene& s, Entity e, const nlohmann::json& j) {
        SpriteComponent c = j.get<SpriteComponent>();
        // Sin ruta (p.ej. el jugador, cuya textura la pone el modo desde la hoja de
        // animación, o una entidad a la que aún no se le asignó imagen) se usa la textura
        // blanca: un placeholder VISIBLE es mejor que una entidad que no se ve. Un
        // loadTexture("") solo dejaría un aviso en la consola.
        if (m_assets)
            c.tex = c.texturePath.empty() ? m_assets->whiteTexture()
                                          : m_assets->loadTexture(c.texturePath, true, true, c.filter);
        s.add<SpriteComponent>(e, std::move(c));
    };
    m_types.push_back(std::move(io));
}

void SceneManager::newScene() {
    m_current = std::make_unique<Scene>();
    m_currentPath.clear();
}

// Escena activa → JSON. Mismo formato que el archivo (es literalmente lo que se escribe):
// así el snapshot de Play y el guardado a disco no pueden divergir.
nlohmann::json SceneManager::toJson() const {
    nlohmann::json j;
    j["entities"] = nlohmann::json::array();
    for (Entity e : m_current->allEntities()) {
        nlohmann::json comps = nlohmann::json::object();
        for (const TypeIO& io : m_types)
            if (io.has(*m_current, e)) {
                nlohmann::json cj;
                io.save(*m_current, e, cj);
                comps[io.name] = std::move(cj);
            }
        j["entities"].push_back({ { "components", std::move(comps) } });
    }
    return j;
}

// JSON → escena NUEVA que pasa a ser la activa. El puntero de `current()` cambia a
// propósito: es la señal por la que los modos re-vinculan sus entidades (ver el GOTCHA de
// que un Entity solo vale en SU escena).
void SceneManager::fromJson(const nlohmann::json& j) {
    auto fresh = std::make_unique<Scene>();
    if (j.contains("entities") && j["entities"].is_array()) {
        for (const auto& ej : j["entities"]) {
            Entity e = fresh->createEntity();
            if (!ej.contains("components")) continue;
            for (auto it = ej["components"].begin(); it != ej["components"].end(); ++it)
                for (const TypeIO& io : m_types)
                    if (io.name == it.key()) { io.load(*fresh, e, it.value()); break; }
        }
    }
    m_current = std::move(fresh);
}

bool SceneManager::save(const std::string& path) {
    const nlohmann::json j = toJson();

    std::ofstream f(path);
    if (!f) { LOG_WARN("SceneManager: no se pudo escribir '%s'", path.c_str()); return false; }
    f << j.dump(2);
    m_currentPath = path;
    // Registra esta escena como la inicial del proyecto → al reabrirlo se recarga.
    Project::instance().noteSceneSaved(path);
    LOG_INFO("Escena guardada en %s (%zu entidades).", path.c_str(), m_current->allEntities().size());
    return true;
}

bool SceneManager::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) { LOG_WARN("SceneManager: no hay escena en '%s'", path.c_str()); return false; }
    nlohmann::json j;
    try { f >> j; }
    catch (const std::exception& e) { LOG_ERROR("SceneManager: JSON inválido (%s)", e.what()); return false; }

    fromJson(j);
    m_currentPath = path;
    LOG_INFO("Escena cargada de %s (%zu entidades).", path.c_str(), m_current->allEntities().size());
    return true;
}

}  // namespace pk
