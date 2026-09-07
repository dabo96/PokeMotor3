// Game/GameStack.h — pila de modos de juego (overworld, combate, menú...).
// Solo el modo de la cima recibe input y updates (estilo Pokémon). Empujar un
// modo pausa el de abajo; sacarlo lo reanuda. Diseño: Fase 4.
#pragma once

#include "Game/GameMode.h"

#include <memory>
#include <vector>

namespace pk {

class GameStack {
public:
    void push(std::unique_ptr<GameMode> mode, GameContext& ctx);
    void pop(GameContext& ctx);
    void clear(GameContext& ctx);

    bool      empty() const { return m_modes.empty(); }
    size_t    size()  const { return m_modes.size(); }   // Stop cierra los overlays hasta el modo base
    GameMode* top()         { return m_modes.empty() ? nullptr : m_modes.back().get(); }

    // handleInput va solo a la cima; update/render descienden respetando los flags de
    // bloqueo de cada modo (ver GameMode::blocksUpdate/RenderBelow).
    void handleInput(GameContext& ctx);
    void fixedUpdate(GameContext& ctx, float dt);
    void variableUpdate(GameContext& ctx, float dt);
    void render(GameContext& ctx);

private:
    // Índice del modo más bajo a procesar: baja desde la cima mientras los modos no
    // bloqueen (con 'blocks' = blocksUpdateBelow o blocksRenderBelow según el caso).
    size_t lowestActive(bool (GameMode::*blocks)() const) const;

    std::vector<std::unique_ptr<GameMode>> m_modes;
};

}  // namespace pk
