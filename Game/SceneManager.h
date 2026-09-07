// Game/SceneManager.h — gestiona la escena ACTIVA y la persiste a JSON.
// Diseño: MotorGrafico_BriefSceneManager.md. Una "escena" = el contenido autorado de
// un nivel/mapa (entidades + componentes), distinto del save del jugador. La escena
// activa es propiedad del SceneManager; editor/sistemas operan sobre current().
#pragma once

#include "Scene/Scene.h"

#include <json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pk {

class AssetManager;

class SceneManager {
public:
    void init(AssetManager* assets);          // registra los componentes serializables

    Scene& current() { return *m_current; }

    void newScene();                          // escena vacía y activa
    bool save(const std::string& path);       // serializa la activa
    bool load(const std::string& path);       // deserializa → nueva activa
    const std::string& currentPath() const { return m_currentPath; }

    // (De)serialización EN MEMORIA, mismo formato que el archivo. La usa el editor para
    // fotografiar la escena al pulsar Play y devolverla tal cual al parar, de modo que
    // jugar no altere lo que estabas editando. `fromJson` sustituye la escena activa (el
    // puntero de current() cambia: los modos lo detectan y re-vinculan sus entidades).
    nlohmann::json toJson() const;
    void           fromJson(const nlohmann::json& j);

private:
    // (De)serialización registrada por tipo de componente. Mantiene el ECS ajeno a JSON.
    struct TypeIO {
        std::string name;
        std::function<bool(Scene&, Entity)>                          has;
        std::function<void(Scene&, Entity, nlohmann::json&)>         save;
        std::function<void(Scene&, Entity, const nlohmann::json&)>   load;
    };
    template <typename T> void registerComponent(const std::string& name);

    std::vector<TypeIO>    m_types;
    std::unique_ptr<Scene> m_current = std::make_unique<Scene>();
    std::string            m_currentPath;
    AssetManager*          m_assets = nullptr;
};

template <typename T>
void SceneManager::registerComponent(const std::string& name) {
    TypeIO io;
    io.name = name;
    io.has  = [](Scene& s, Entity e) { return s.has<T>(e); };
    io.save = [](Scene& s, Entity e, nlohmann::json& j) { j = s.get<T>(e); };          // to_json<T>
    io.load = [](Scene& s, Entity e, const nlohmann::json& j) { s.add<T>(e, j.get<T>()); }; // from_json<T>
    m_types.push_back(std::move(io));
}

}  // namespace pk
