// Game/BattleMode.h — modo de combate (esqueleto) en 2D. Presenta al Pokémon
// salvaje centrado con cámara ortográfica y vuelve al overworld al confirmar.
// Su razón de ser: demostrar la pila de modos (overworld se pausa, el combate
// toma el control, al sacarlo el overworld se reanuda). Diseño: Fase 4 + Plan2D.
#pragma once

#include "Core/Handle.h"
#include "Core/Math.h"
#include "Game/GameMode.h"
#include "Renderer/Camera/OrthographicCamera.h"

#include <string>

namespace pk {

class BattleMode : public GameMode {
public:
    // El sprite del rival llega COMO DATO (Species::sprite, de species.json); el combate
    // no sabe qué imagen usa cada especie ni la deduce.
    BattleMode(std::string foeName, int foeId, std::string foeSprite);

    void onEnter(GameContext& ctx) override;
    void handleInput(GameContext& ctx) override;
    void variableUpdate(GameContext& ctx, float dt) override;
    void render(GameContext& ctx) override;       // emite el sprite del rival + cámara
    // Hereda blocksUpdateBelow()=true y blocksRenderBelow()=true: el combate pausa y TAPA
    // el overworld (pantalla de combate propia).

private:
    std::string        m_foeName;
    int                m_foeId = 0;
    std::string        m_foeSprite;   // ruta del arte del rival (vacía = sin textura)
    TextureHandle      m_foeTex;
    OrthographicCamera m_cam;
    float              m_t = 0.0f;   // tiempo en combate (para el "bob" del sprite)
};

}  // namespace pk
