// Scene/Scene.h — el registro ECS: posee las entidades y un pool por tipo de
// componente; expone la API que consumen sistemas, editor, SceneManager y scripts.
// Diseño: MotorGrafico_BriefECS.md.
//   Entity e = scene.createEntity();
//   scene.add<Transform>(e, {...}); scene.get<Sprite>(e); scene.has<...>(e);
//   for (...) scene.view<Transform, Sprite>().each([](Entity, Transform&, Sprite&){});
//   scene.destroyEntity(e);  // limpia TODOS sus componentes
#pragma once

#include "Core/ECS/ComponentPool.h"
#include "Core/ECS/Entity.h"
#include "Scene/View.h"

#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace pk {

class Scene {
public:
    Entity createEntity() { return m_entities.create(); }

    void destroyEntity(Entity e) {
        if (!m_entities.alive(e)) return;
        for (auto& kv : m_pools) kv.second->remove(e);   // sin componentes huérfanos
        m_entities.destroy(e);
    }

    bool alive(Entity e) const { return m_entities.alive(e); }

    template <typename T> T&   add(Entity e, T c)  { return pool<T>().add(e, std::move(c)); }
    template <typename T> void remove(Entity e)    { if (auto* p = poolPtr<T>()) p->remove(e); }
    template <typename T> bool has(Entity e) const { auto* p = poolPtr<T>(); return p && p->has(e); }
    template <typename T> T&   get(Entity e)       { return pool<T>().get(e); }

    template <typename... Ts> View<Ts...> view() { return View<Ts...>(*this); }
    std::vector<Entity> allEntities() const;

    // Pool del tipo T (lo crea al primer uso); poolPtr no lo crea (nullptr si no hay).
    template <typename T> ComponentPool<T>& pool() {
        const std::type_index idx(typeid(T));
        auto it = m_pools.find(idx);
        if (it == m_pools.end())
            it = m_pools.emplace(idx, std::make_unique<ComponentPool<T>>()).first;
        return *static_cast<ComponentPool<T>*>(it->second.get());
    }
    template <typename T> ComponentPool<T>* poolPtr() const {
        auto it = m_pools.find(std::type_index(typeid(T)));
        return it == m_pools.end() ? nullptr
                                   : static_cast<ComponentPool<T>*>(it->second.get());
    }

private:
    EntityManager m_entities;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> m_pools;
};

}  // namespace pk
