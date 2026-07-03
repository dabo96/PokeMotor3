// Game/EventRunner.cpp — implementación del intérprete de eventos.
#include "Game/EventRunner.h"

#include "Game/DialogueMode.h"
#include "Game/GameMode.h"   // GameContext
#include "Game/GameStack.h"

#include <memory>
#include <utility>

namespace pk {

void EventRunner::showTexts(std::vector<std::string> texts, GameContext& ctx) {
    if (texts.empty()) return;
    m_queue = std::move(texts);
    m_index = 0;
    pushNext(ctx);
}

void EventRunner::pump(GameContext& ctx) {
    // Si teníamos un diálogo en curso y el modo dueño vuelve a correr (estamos en su
    // handleInput), es que el DialogueMode se cerró: avanza al siguiente o termina.
    if (!m_waiting) return;
    m_waiting = false;
    if (m_index < m_queue.size()) pushNext(ctx);
    else                          m_queue.clear();
}

void EventRunner::pushNext(GameContext& ctx) {
    if (!ctx.stack || m_index >= m_queue.size()) return;
    ctx.stack->push(std::make_unique<DialogueMode>(m_queue[m_index]), ctx);
    ++m_index;
    m_waiting = true;
}

}  // namespace pk
