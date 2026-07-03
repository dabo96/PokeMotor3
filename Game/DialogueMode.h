// Game/DialogueMode.h — modo OVERLAY que muestra una caja de diálogo (DialogueBox)
// sobre el modo de abajo: lo congela (blocksUpdateBelow) pero lo deja verse detrás
// (blocksRenderBelow=false). Se cierra solo cuando el diálogo termina (wantsPop). El
// EventRunner lo empuja por cada evento ShowText. Diseño: MotorGrafico_UIJuego.md.
#pragma once

#include "Game/GameMode.h"
#include "UI/UITheme.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pk {

class DialogueBox;   // unique_ptr a tipo incompleto → ctor/dtor en el .cpp

class DialogueMode : public GameMode {
public:
    explicit DialogueMode(std::string text);
    DialogueMode(std::string text, std::vector<std::string> choices, std::function<void(int)> onPick);
    ~DialogueMode() override;

    // Callback invocado UNA vez cuando el diálogo de TEXTO se cierra (lo usa el scripting
    // para reanudar la corrutina de evento). En modo elección el resultado va por onPick
    // (índice), así que onDone no se usa en ese caso.
    void setOnDone(std::function<void()> fn) { m_onDone = std::move(fn); }

    void onEnter(GameContext& ctx) override;
    void onExit(GameContext& ctx) override;
    void variableUpdate(GameContext& ctx, float dt) override;
    void render(GameContext& ctx) override;

    bool blocksUpdateBelow() const override { return true;  }   // congela el overworld
    bool blocksRenderBelow() const override { return false; }   // se ve detrás
    bool wantsPop() const override;

private:
    UITheme                      m_theme;
    std::unique_ptr<DialogueBox> m_box;
    std::string                  m_text;
    std::vector<std::string>     m_choices;
    std::function<void(int)>     m_onPick;
    std::function<void()>        m_onDone;   // al cerrarse (solo texto); reanuda la corrutina
};

}  // namespace pk
