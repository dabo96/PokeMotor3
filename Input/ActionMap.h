// Input/ActionMap.h — capa semántica encima del input crudo.
// Diseño: MotorGrafico_Input.md (capa de acciones). El juego pregunta por
// intenciones (Confirm, MoveUp), no por teclas físicas. Rebindear es editar la
// tabla; la lógica no cambia.
#pragma once

#include "Input/KeyCode.h"

#include <unordered_map>
#include <vector>

namespace pk {

class Input;

enum class Action {
    MoveUp, MoveDown, MoveLeft, MoveRight,
    Confirm, Cancel, Menu, Run,
    Count
};

class ActionMap {
public:
    void bind(Action a, Key k);                          // un Action puede tener varias teclas

    bool isActive(const Input& in, Action a) const;      // ¿activa ahora? (polling)
    bool wasTriggered(const Input& in, Action a) const;  // ¿flanco este frame?

private:
    std::unordered_map<int, std::vector<Key>> m_bindings;  // clave = (int)Action
};

}  // namespace pk
