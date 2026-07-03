// Scene/View.h — query del ECS: itera las entidades que tienen TODOS los Ts.
// Diseño: MotorGrafico_BriefECS.md. Recorre el pool del primer tipo y filtra el
// resto con has<>. Se instancia donde se use (Scene ya completo allí), por eso aquí
// basta una declaración adelantada de Scene. Uso: scene.view<A,B>().each([](Entity,
// A&, B&){ ... }).
#pragma once

#include "Core/ECS/ComponentPool.h"
#include "Core/ECS/Entity.h"

#include <tuple>
#include <vector>

namespace pk {

class Scene;   // definido en Scene.h; los cuerpos template se instancian con él completo

template <typename... Ts>
class View {
public:
    explicit View(Scene& scene) : m_scene(scene) {}

    template <typename Fn>
    void each(Fn&& fn) {
        using First = std::tuple_element_t<0, std::tuple<Ts...>>;
        auto* pool = m_scene.template poolPtr<First>();
        if (!pool) return;
        // Copia de ids: tolera que fn mute componentes (no estructural) sin invalidar.
        const std::vector<Entity> ents = pool->entities();
        for (Entity e : ents)
            if ((m_scene.template has<Ts>(e) && ...))
                fn(e, m_scene.template get<Ts>(e)...);
    }

private:
    Scene& m_scene;
};

}  // namespace pk
