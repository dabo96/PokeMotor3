// Core/ECS/ComponentPool.h — almacén de un tipo de componente como SPARSE SET.
// Diseño: MotorGrafico_BriefECS.md. Arrays densos (cache-friendly, O(1) add/get/
// remove con swap-and-pop); `has` compara el dueño en el denso → un handle viejo da
// false (use-after-free detectado). `IComponentPool` permite que Scene limpie todos
// los pools al destruir una entidad sin conocer los tipos.
#pragma once

#include "Core/ECS/Entity.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace pk {

struct IComponentPool {
    virtual ~IComponentPool() = default;
    virtual void remove(Entity e)       = 0;
    virtual bool has(Entity e) const    = 0;
};

template <typename T>
class ComponentPool : public IComponentPool {
    static constexpr uint32_t NIL = UINT32_MAX;
public:
    // Añade (o reemplaza si ya existe) el componente de e. Devuelve la referencia.
    T& add(Entity e, T comp) {
        if (e.id >= m_sparse.size()) m_sparse.resize(e.id + 1, NIL);
        if (m_sparse[e.id] != NIL && m_dense[m_sparse[e.id]] == e) {
            m_components[m_sparse[e.id]] = std::move(comp);   // reemplazo idempotente
            return m_components[m_sparse[e.id]];
        }
        m_sparse[e.id] = static_cast<uint32_t>(m_dense.size());
        m_dense.push_back(e);
        m_components.push_back(std::move(comp));
        return m_components.back();
    }

    bool has(Entity e) const override {
        return e.id < m_sparse.size() && m_sparse[e.id] != NIL
            && m_dense[m_sparse[e.id]] == e;   // compara dueño → valida la generación
    }

    T& get(Entity e) { return m_components[m_sparse[e.id]]; }   // precondición: has(e)

    void remove(Entity e) override {
        if (!has(e)) return;
        const uint32_t idx  = m_sparse[e.id];
        const uint32_t last = static_cast<uint32_t>(m_dense.size()) - 1;
        m_dense[idx]      = m_dense[last];               // swap-and-pop: denso sin huecos
        m_components[idx] = std::move(m_components[last]);
        m_sparse[m_dense[idx].id] = idx;                 // reapunta el que se movió
        m_dense.pop_back();
        m_components.pop_back();
        m_sparse[e.id] = NIL;
    }

    const std::vector<Entity>& entities()   const { return m_dense; }       // iteración densa
    std::vector<T>&            components()        { return m_components; }
    size_t                     size()       const { return m_dense.size(); }

private:
    std::vector<uint32_t> m_sparse;       // entity.id → índice en el denso, o NIL
    std::vector<Entity>   m_dense;         // qué entidad posee components[i]
    std::vector<T>        m_components;     // paralelo a m_dense
};

}  // namespace pk
