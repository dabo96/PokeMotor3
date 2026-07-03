// Core/ECS/Entity.h — handle de entidad con generación + el gestor que los crea.
// Diseño: MotorGrafico_BriefECS.md. La generación invalida handles a entidades ya
// destruidas (mata el use-after-free). Base del ECS; lo usan ComponentPool y Scene.
#pragma once

#include <cstdint>
#include <vector>

namespace pk {

// Handle a una entidad. generation==0 ⇒ inválido (nunca se asigna a una entidad viva).
struct Entity {
    uint32_t id         = 0;
    uint32_t generation = 0;

    bool valid() const { return generation != 0; }
    bool operator==(Entity o) const { return id == o.id && generation == o.generation; }
    bool operator!=(Entity o) const { return !(*this == o); }
};

// Crea/destruye ids con free list + generaciones. Reusa ids liberados subiendo su
// generación, de modo que un handle viejo al mismo id queda inválido.
class EntityManager {
public:
    Entity create() {
        uint32_t id;
        if (!m_free.empty()) {
            id = m_free.back();
            m_free.pop_back();
        } else {
            id = static_cast<uint32_t>(m_generations.size());
            m_generations.push_back(1);   // primera generación válida (≠0)
            m_alive.push_back(false);
        }
        m_alive[id] = true;
        return { id, m_generations[id] };
    }

    void destroy(Entity e) {
        if (!alive(e)) return;
        ++m_generations[e.id];            // invalida handles viejos a este id
        m_alive[e.id] = false;
        m_free.push_back(e.id);
    }

    bool alive(Entity e) const {
        return e.id < m_generations.size() && m_alive[e.id]
            && m_generations[e.id] == e.generation;
    }

    // Llama fn(Entity) para cada entidad viva (para la jerarquía / allEntities).
    template <typename Fn>
    void forEachAlive(Fn&& fn) const {
        for (uint32_t id = 0; id < m_generations.size(); ++id)
            if (m_alive[id]) fn(Entity{ id, m_generations[id] });
    }

private:
    std::vector<uint32_t> m_generations;   // generación actual por id
    std::vector<uint8_t>  m_alive;         // ¿el id está vivo ahora? (vector<bool> evita)
    std::vector<uint32_t> m_free;          // ids libres para reusar
};

}  // namespace pk
