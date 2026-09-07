// Game/BattleMode.cpp — implementación del combate esqueleto (2D).
#include "Game/BattleMode.h"

#include "Assets/AssetManager.h"
#include "Core/Log.h"
#include "Game/GameStack.h"
#include "Input/ActionMap.h"
#include "Input/Input.h"
#include "Renderer/2D/Sprite.h"
#include "Renderer/Renderer.h"

#include <cmath>
#include <utility>
#include <vector>

namespace pk {

BattleMode::BattleMode(std::string foeName, int foeId, std::string foeSprite)
    : m_foeName(std::move(foeName)), m_foeId(foeId), m_foeSprite(std::move(foeSprite)) {}

void BattleMode::onEnter(GameContext& ctx) {
    LOG_INFO("=== COMBATE: ¡%s salvaje! Pulsa Enter/Espacio para volver. ===",
             m_foeName.c_str());
    if (ctx.assets && !m_foeSprite.empty()) {
        m_foeTex = ctx.assets->loadTexture(m_foeSprite);
        // loadTexture nunca falla (devuelve la blanca): avisamos para que se note que
        // falta el arte de esa especie en vez de dibujar un cuadro blanco en silencio.
        if (m_foeTex == ctx.assets->whiteTexture())
            LOG_WARN("Combate: falta el sprite '%s' de %s (revisa species.json).",
                     m_foeSprite.c_str(), m_foeName.c_str());
    }
}

void BattleMode::handleInput(GameContext& ctx) {
    if (!ctx.input || !ctx.actions) return;
    // Confirm (Enter/Espacio) cierra el combate y reanuda el overworld.
    if (ctx.actions->wasTriggered(*ctx.input, Action::Confirm)) {
        LOG_INFO("Escapaste del combate contra %s.", m_foeName.c_str());
        if (ctx.stack) ctx.stack->pop(ctx);   // saca ESTE modo → vuelve el overworld
    }
}

void BattleMode::variableUpdate(GameContext& /*ctx*/, float dt) {
    m_t += dt;   // solo lógica (el "bob"); la emisión va en render()
}

void BattleMode::render(GameContext& ctx) {
    if (!ctx.renderer) return;

    // Cámara de combate: fija, centrada en el origen, con bastante zoom.
    m_cam.setViewport(480.0f, 270.0f);
    m_cam.setZoom(64.0f);
    m_cam.setCenter(Vec2(0.0f, 0.0f));

    // El Pokémon salvaje, centrado y "respirando" (sin tiles: pantalla de combate).
    const float bob = std::sin(m_t * 3.0f) * 0.06f;
    Sprite foe;
    foe.size     = Vec2(3.0f, 3.0f);
    foe.position = Vec2(-1.5f, -1.5f + bob);   // centrado (origen - size/2)
    foe.texture  = m_foeTex;
    foe.layer    = 0;

    ctx.renderer->setSprites(std::vector<Sprite>{ foe });
    ctx.renderer->set2DCamera(m_cam.viewProjection());
}

}  // namespace pk
