// Game/MenuMode.h — modo OVERLAY: un menú de UI (árbol retenido) sobre el modo de
// abajo. Congela su lógica (blocksUpdateBelow) pero lo deja verse detrás
// (blocksRenderBelow=false). Diseño: MotorGrafico_UIJuego.md (pila de modos).
#pragma once

#include "Game/GameMode.h"
#include "UI/UITheme.h"

#include <memory>

namespace pk {

class Widget;   // unique_ptr a tipo incompleto → destructor en el .cpp

class MenuMode : public GameMode {
public:
    MenuMode();                  // = default en el .cpp (Widget completo para el unique_ptr)
    ~MenuMode() override;

    void onEnter(GameContext& ctx) override;
    void variableUpdate(GameContext& ctx, float dt) override;
    void render(GameContext& ctx) override;

    bool blocksUpdateBelow() const override { return true;  }   // congela el overworld
    bool blocksRenderBelow() const override { return false; }   // se ve detrás
    bool wantsPop() const override { return m_close; }

private:
    UITheme                 m_theme;
    std::unique_ptr<Widget> m_root;
    bool                    m_close = false;
};

}  // namespace pk
