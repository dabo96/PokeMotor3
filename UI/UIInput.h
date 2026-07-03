// UI/UIInput.h — entrada de la UI del juego como ACCIONES semánticas (flancos), no
// teclas físicas: igual con teclado o mando. Se construye desde el ActionMap.
// Diseño: MotorGrafico_UIJuego.md.
#pragma once

namespace pk {

class Input;
class ActionMap;

struct UIInput {
    bool up = false, down = false, left = false, right = false;
    bool confirm = false, cancel = false;

    bool any() const { return up || down || left || right || confirm || cancel; }

    // Toma los flancos del frame (una pulsación = un evento) del ActionMap.
    static UIInput fromActions(const Input& in, const ActionMap& actions);
};

}  // namespace pk
