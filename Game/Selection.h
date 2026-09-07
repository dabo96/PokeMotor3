// Game/Selection.h — selección del editor: la ENTIDAD activa, compartida por la
// jerarquía, el picking del viewport y el inspector. El inspector edita los
// componentes de esa entidad directamente vía la Scene (ya no hay copia/dirty).
// POD: lo posee el Engine y se pasa por puntero.
#pragma once

#include "Core/ECS/Entity.h"

namespace pk {

struct Selection {
    Entity entity;   // entidad seleccionada (generación 0 = nada)

    // Dónde se ve la entidad EN PANTALLA (coordenadas del ratón / ventana lógica). Lo publica
    // el modo al renderizar —es quien conoce la cámara— y lo consume el editor para anclar el
    // inspector flotante junto a lo seleccionado. `screenValid` a false = el modo activo no
    // proyecta (o no hay selección): el editor recurre entonces a su posición por defecto.
    float screenX = 0.0f, screenY = 0.0f;
    // Radio EN PÍXELES que ocupa lo seleccionado con su gizmo alrededor. El editor lo usa para
    // apartar el inspector anclado lo justo: pegado al centro taparía las flechas y el anillo.
    float screenRadius = 0.0f;
    bool  screenValid = false;

    bool has() const { return entity.valid(); }
    void clear() { entity = Entity{}; screenValid = false; }
};

}  // namespace pk
