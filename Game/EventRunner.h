// Game/EventRunner.h — ejecuta una SECUENCIA de eventos de juego sobre la pila de
// modos. v1: ShowText (una caja de diálogo por texto). Como un DialogueMode CONGELA al
// modo dueño (overworld), el runner no necesita un update continuo: avanza al siguiente
// texto cuando el dueño recupera el control (su handleInput vuelve a correr = el diálogo
// se cerró). Lo posee el OverworldMode. Diseño: MotorGrafico_UIJuego.md (EventRunner).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pk {

struct GameContext;

class EventRunner {
public:
    // Arranca una secuencia: muestra cada texto en un DialogueMode, uno tras otro.
    void showTexts(std::vector<std::string> texts, GameContext& ctx);

    // Llamar al INICIO del handleInput del modo dueño. Si el diálogo en curso se cerró,
    // empuja el siguiente texto o termina la secuencia.
    void pump(GameContext& ctx);

    // true mientras hay un diálogo de la secuencia en la pila (el dueño debe ignorar su
    // propio input ese frame).
    bool active() const { return m_waiting; }

private:
    void pushNext(GameContext& ctx);

    std::vector<std::string> m_queue;
    std::size_t              m_index   = 0;
    bool                     m_waiting = false;   // hay un DialogueMode nuestro en la pila
};

}  // namespace pk
