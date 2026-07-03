// Game/GameStack.cpp — implementación de la pila de modos.
#include "Game/GameStack.h"

namespace pk {

void GameStack::push(std::unique_ptr<GameMode> mode, GameContext& ctx) {
    m_modes.push_back(std::move(mode));
    m_modes.back()->onEnter(ctx);
}

void GameStack::pop(GameContext& ctx) {
    if (m_modes.empty()) return;
    m_modes.back()->onExit(ctx);
    m_modes.pop_back();
}

void GameStack::clear(GameContext& ctx) {
    while (!m_modes.empty()) pop(ctx);
}

// Modo más bajo a procesar: desde la cima, baja mientras los de arriba NO bloqueen.
// (blocks = blocksUpdateBelow para update, blocksRenderBelow para render.)
size_t GameStack::lowestActive(bool (GameMode::*blocks)() const) const {
    size_t low = m_modes.size() - 1;
    while (low > 0 && !((*m_modes[low]).*blocks)()) --low;
    return low;
}

void GameStack::handleInput(GameContext& ctx) {
    if (GameMode* m = top()) m->handleInput(ctx);
}

void GameStack::fixedUpdate(GameContext& ctx, float dt) {
    if (m_modes.empty()) return;
    const size_t low = lowestActive(&GameMode::blocksUpdateBelow);
    for (size_t i = low; i < m_modes.size(); ++i) m_modes[i]->fixedUpdate(ctx, dt);
}

void GameStack::variableUpdate(GameContext& ctx, float dt) {
    if (m_modes.empty()) return;
    const size_t low = lowestActive(&GameMode::blocksUpdateBelow);
    for (size_t i = low; i < m_modes.size(); ++i) m_modes[i]->variableUpdate(ctx, dt);

    // Pops diferidos: un modo que pidió cerrarse se saca AQUÍ, ya fuera de su update
    // (hacerlo dentro se autodestruiría a media función). Solo la cima puede auto-cerrarse.
    while (!m_modes.empty() && m_modes.back()->wantsPop()) pop(ctx);
}

void GameStack::render(GameContext& ctx) {
    if (m_modes.empty()) return;
    const size_t low = lowestActive(&GameMode::blocksRenderBelow);
    for (size_t i = low; i < m_modes.size(); ++i) m_modes[i]->render(ctx);
}

}  // namespace pk
