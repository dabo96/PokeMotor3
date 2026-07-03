// Game/Selection.h — selección del editor: la ENTIDAD activa, compartida por la
// jerarquía, el picking del viewport y el inspector. El inspector edita los
// componentes de esa entidad directamente vía la Scene (ya no hay copia/dirty).
// POD: lo posee el Engine y se pasa por puntero.
#pragma once

#include "Core/ECS/Entity.h"

namespace pk {

struct Selection {
    Entity entity;   // entidad seleccionada (generación 0 = nada)

    bool has() const { return entity.valid(); }
    void clear() { entity = Entity{}; }
};

}  // namespace pk
