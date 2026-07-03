// Scene/Scene.cpp — solo lo no-inline del registro ECS.
#include "Scene/Scene.h"

namespace pk {

std::vector<Entity> Scene::allEntities() const {
    std::vector<Entity> out;
    m_entities.forEachAlive([&out](Entity e) { out.push_back(e); });
    return out;
}

}  // namespace pk
