// Input/ActionMap.cpp — resolución de acciones contra el Input.
#include "Input/ActionMap.h"

#include "Input/Input.h"

namespace pk {

void ActionMap::bind(Action a, Key k) {
    m_bindings[(int)a].push_back(k);
}

bool ActionMap::isActive(const Input& in, Action a) const {
    auto it = m_bindings.find((int)a);
    if (it == m_bindings.end()) return false;
    for (Key k : it->second)
        if (in.isKeyDown(k)) return true;
    return false;
}

bool ActionMap::wasTriggered(const Input& in, Action a) const {
    auto it = m_bindings.find((int)a);
    if (it == m_bindings.end()) return false;
    for (Key k : it->second)
        if (in.wasKeyPressed(k)) return true;   // cualquiera de las teclas vale
    return false;
}

}  // namespace pk
