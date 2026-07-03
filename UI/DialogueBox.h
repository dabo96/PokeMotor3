// UI/DialogueBox.h — caja de diálogo del juego (estilo Pokémon): efecto máquina de
// escribir, paginado del texto según el ancho/alto de la caja y elección opcional al
// final. Es un Widget anclado abajo; el DialogueMode la mete en la pila como overlay.
// Diseño: MotorGrafico_UIJuego.md.
#pragma once

#include "Core/Math.h"
#include "UI/Rect.h"
#include "UI/Widget.h"

#include <functional>
#include <string>
#include <vector>

namespace pk {

struct UITheme;
class UIRenderer;
struct UIInput;

class DialogueBox : public Widget {
public:
    explicit DialogueBox(const UITheme& theme) : m_theme(&theme) {}

    void show(const std::string& text);
    void showChoice(const std::string& text, std::vector<std::string> opts,
                    std::function<void(int)> onPick);
    bool isFinished() const { return m_finished; }

protected:
    void onUpdate(float dt) override;          // avanza la máquina de escribir
    void onDraw(UIRenderer& r) override;       // caja + texto revelado (+ elección)
    bool onInput(const UIInput& in) override;  // Confirm: completa/avanza/cierra

private:
    void  reset();
    void  layout(UIRenderer& r, const Rect& box);  // mide y parte el texto en páginas
    bool  pageRevealed() const;                    // ¿la página actual está completa?
    float fontSize() const;

    const UITheme*           m_theme;
    std::string              m_full;
    std::vector<std::string> m_pages;        // texto por página (líneas separadas por '\n')
    size_t m_page        = 0;
    size_t m_revealed    = 0;                // chars revelados de la página actual
    float  m_revealTimer = 0.0f;
    float  m_blink       = 0.0f;
    float  m_charsPerSec = 40.0f;
    bool   m_laidOut     = false;            // ya medido (necesita el UIRenderer del 1er draw)
    bool   m_finished    = false;
    Vec2   m_textOrigin{ 0.0f, 0.0f };
    float  m_lineHeight  = 12.0f;

    // Elección opcional, mostrada al terminar de revelar la última página.
    std::vector<std::string> m_choices;
    std::function<void(int)> m_onPick;
    bool m_choiceActive = false;
    int  m_choiceSel    = 0;
};

}  // namespace pk
