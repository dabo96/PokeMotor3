// Game/DialogueMode.cpp — implementación del modo de diálogo.
#include "Game/DialogueMode.h"

#include "Game/GameMode.h"   // GameContext
#include "Input/ActionMap.h"
#include "Input/Input.h"
#include "Renderer/Renderer.h"
#include "UI/DialogueBox.h"
#include "UI/UIInput.h"
#include "UI/UIRenderer.h"

#include <utility>

namespace pk {

DialogueMode::DialogueMode(std::string text) : m_text(std::move(text)) {}

DialogueMode::DialogueMode(std::string text, std::vector<std::string> choices,
                           std::function<void(int)> onPick)
    : m_text(std::move(text)), m_choices(std::move(choices)), m_onPick(std::move(onPick)) {}

DialogueMode::~DialogueMode() = default;   // aquí DialogueBox es completo (para el unique_ptr)

void DialogueMode::onEnter(GameContext&) {
    m_theme.panel.tint = Vec4(0.06f, 0.07f, 0.13f, 0.94f);   // sin textura → color plano
    m_theme.fontSize   = 12.0f;

    m_box = std::make_unique<DialogueBox>(m_theme);
    if (m_choices.empty()) m_box->show(m_text);
    else                   m_box->showChoice(m_text, m_choices, m_onPick);
}

void DialogueMode::onExit(GameContext&) {
    // El diálogo se cerró (lo sacó el GameStack al ver wantsPop). En modo TEXTO avisamos
    // aquí para reanudar la corrutina; en modo elección el aviso ya lo dio onPick con el
    // índice (no duplicar). m_onDone se limpia tras usarlo (no reentrar).
    if (m_choices.empty() && m_onDone) {
        std::function<void()> cb = std::move(m_onDone);
        m_onDone = nullptr;
        cb();
    }
}

void DialogueMode::variableUpdate(GameContext& ctx, float dt) {
    if (!m_box) return;
    if (ctx.input && ctx.actions)
        m_box->handleInput(UIInput::fromActions(*ctx.input, *ctx.actions));
    m_box->update(dt);
    // El pop real lo hace el GameStack al ver wantsPop() (no aquí: nos autodestruiríamos).
}

void DialogueMode::render(GameContext& ctx) {
    if (!ctx.renderer || !m_box) return;
    UIRenderer ui(*ctx.renderer);
    ui.begin();
    m_box->draw(ui);
    ui.flush();
}

bool DialogueMode::wantsPop() const { return m_box && m_box->isFinished(); }

}  // namespace pk
