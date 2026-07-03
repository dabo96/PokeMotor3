// Game/MenuMode.cpp — implementación del menú overlay.
#include "Game/MenuMode.h"

#include "Core/Log.h"
#include "Game/GameMode.h"     // GameContext
#include "Input/ActionMap.h"
#include "Input/Input.h"
#include "Renderer/Renderer.h"
#include "UI/UIInput.h"
#include "UI/UIRenderer.h"
#include "UI/Widgets.h"

namespace pk {

MenuMode::MenuMode()  = default;   // aquí Widget es completo (para el unique_ptr<Widget>)
MenuMode::~MenuMode() = default;

void MenuMode::onEnter(GameContext&) {
    m_theme.panel.tint = Vec4(0.06f, 0.07f, 0.13f, 0.94f);   // sin textura → color plano
    m_theme.fontSize   = 12.0f;

    auto panel = std::make_unique<Panel>(m_theme);
    panel->rect = Rect{ 168.0f, 64.0f, 144.0f, 104.0f };

    auto menu = std::make_unique<Menu>(m_theme);
    menu->options    = { "Pokedex", "Pokemon", "Mochila", "Salir" };
    menu->itemHeight = 18.0f;
    menu->rect = Rect{ panel->rect.x + 12.0f, panel->rect.y + 14.0f, panel->rect.w - 24.0f, 0.0f };
    menu->onConfirm = [this](int i) {
        LOG_INFO("Menu: opcion %d", i);
        if (i == 3) m_close = true;          // "Salir"
    };
    menu->onCancel = [this] { m_close = true; };
    panel->add(std::move(menu));

    m_root = std::move(panel);
    LOG_INFO("Menu abierto (flechas mueven, Enter confirma, Esc cierra).");
}

void MenuMode::variableUpdate(GameContext& ctx, float dt) {
    if (!m_root) return;
    if (ctx.input && ctx.actions)
        m_root->handleInput(UIInput::fromActions(*ctx.input, *ctx.actions));
    m_root->update(dt);
    // El pop real lo hace el GameStack al ver wantsPop() (no aquí: nos autodestruiríamos).
}

void MenuMode::render(GameContext& ctx) {
    if (!ctx.renderer || !m_root) return;
    UIRenderer ui(*ctx.renderer);
    ui.begin();
    m_root->draw(ui);
    ui.flush();
}

}  // namespace pk
