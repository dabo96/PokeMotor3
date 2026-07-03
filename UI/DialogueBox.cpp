// UI/DialogueBox.cpp — implementación de la caja de diálogo.
#include "UI/DialogueBox.h"

#include "UI/UIInput.h"
#include "UI/UIRenderer.h"
#include "UI/UITheme.h"

#include <algorithm>
#include <cmath>

namespace pk {

namespace {
// Caja anclada abajo (lowRes 480x270), con padding interior para el texto.
const Rect      kBox{ 8.0f, UIRenderer::kScreenH - 64.0f, UIRenderer::kScreenW - 16.0f, 56.0f };
constexpr float kPad = 6.0f;
}  // namespace

float DialogueBox::fontSize() const { return m_theme->fontSize; }

void DialogueBox::reset() {
    m_full.clear(); m_pages.clear();
    m_page = 0; m_revealed = 0; m_revealTimer = 0.0f; m_blink = 0.0f;
    m_laidOut = false; m_finished = false;
    m_choices.clear(); m_onPick = nullptr; m_choiceActive = false; m_choiceSel = 0;
}

void DialogueBox::show(const std::string& text) { reset(); m_full = text; }

void DialogueBox::showChoice(const std::string& text, std::vector<std::string> opts,
                             std::function<void(int)> onPick) {
    reset();
    m_full    = text;
    m_choices = std::move(opts);
    m_onPick  = std::move(onPick);
}

bool DialogueBox::pageRevealed() const {
    if (!m_laidOut || m_pages.empty()) return false;
    return m_revealed >= m_pages[m_page].size();
}

void DialogueBox::layout(UIRenderer& r, const Rect& box) {
    const float px   = fontSize();
    const float maxW = box.w - kPad * 2.0f;
    m_lineHeight = r.lineHeight(px);
    if (m_lineHeight < 1.0f) m_lineHeight = px;
    const int linesPerPage = std::max(1, static_cast<int>((box.h - kPad * 2.0f) / m_lineHeight));
    m_textOrigin = Vec2(box.x + kPad, box.y + kPad);

    // 1) Word-wrap del texto a líneas que caben en maxW (mide con la fuente real).
    std::vector<std::string> lines;
    std::string line, word;
    auto flushWord = [&]() {
        if (word.empty()) return;
        const std::string cand = line.empty() ? word : line + " " + word;
        if (!line.empty() && r.measureText(cand, px) > maxW) { lines.push_back(line); line = word; }
        else                                                 { line = cand; }
        word.clear();
    };
    for (char ch : m_full) {
        if (ch == '\n')     { flushWord(); lines.push_back(line); line.clear(); }
        else if (ch == ' ') { flushWord(); }
        else                { word += ch; }
    }
    flushWord();
    if (!line.empty()) lines.push_back(line);

    // 2) Agrupa las líneas en páginas (linesPerPage por página).
    m_pages.clear();
    for (size_t i = 0; i < lines.size(); i += static_cast<size_t>(linesPerPage)) {
        std::string page;
        for (size_t k = i; k < std::min(lines.size(), i + static_cast<size_t>(linesPerPage)); ++k) {
            if (!page.empty()) page += '\n';
            page += lines[k];
        }
        m_pages.push_back(std::move(page));
    }
    if (m_pages.empty()) m_pages.push_back("");
}

void DialogueBox::onUpdate(float dt) {
    m_blink += dt;
    if (m_laidOut && !pageRevealed()) {
        m_revealTimer += dt;
        const float perChar = (m_charsPerSec > 0.0f) ? 1.0f / m_charsPerSec : 0.0f;
        while (perChar > 0.0f && m_revealTimer >= perChar && !pageRevealed()) {
            m_revealTimer -= perChar;
            ++m_revealed;
        }
    }
}

void DialogueBox::onDraw(UIRenderer& r) {
    r.drawNineSlice(m_theme->panel, kBox, 0);
    if (!m_laidOut) { layout(r, kBox); m_laidOut = true; }   // medir necesita el UIRenderer

    // Texto revelado de la página actual, línea a línea.
    const std::string& page = m_pages[m_page];
    const size_t rev = std::min(m_revealed, page.size());
    float  y = m_textOrigin.y;
    size_t start = 0;
    for (size_t i = 0; i <= rev; ++i) {
        if (i == rev || page[i] == '\n') {
            const std::string ln = page.substr(start, i - start);
            if (!ln.empty()) r.drawText(ln, Vec2(m_textOrigin.x, y), fontSize(), m_theme->textColor);
            y += m_lineHeight;
            start = i + 1;
        }
    }

    // Indicador "hay más" (cuadradito que parpadea) cuando la página está completa y
    // todavía no terminamos ni mostramos la elección.
    if (pageRevealed() && !m_choiceActive && !m_finished && std::fmod(m_blink, 0.8f) < 0.5f)
        r.drawRect(Rect{ kBox.right() - 10.0f, kBox.bottom() - 8.0f, 4.0f, 4.0f },
                   m_theme->highlightColor, 1);

    // Elección (si se activó al terminar el texto): mini-panel sobre la caja.
    if (m_choiceActive && !m_choices.empty()) {
        const float oh = 16.0f, w = 110.0f;
        const Rect cp{ kBox.right() - w,
                       kBox.y - static_cast<float>(m_choices.size()) * oh - 8.0f,
                       w, static_cast<float>(m_choices.size()) * oh + 8.0f };
        r.drawNineSlice(m_theme->panel, cp, 0);
        for (size_t i = 0; i < m_choices.size(); ++i) {
            const float yy  = cp.y + 4.0f + static_cast<float>(i) * oh;
            const bool  sel = (static_cast<int>(i) == m_choiceSel);
            if (sel) r.drawRect(Rect{ cp.x, yy, cp.w, oh }, m_theme->highlightColor, 1);
            r.drawText(m_choices[i], Vec2(cp.x + 12.0f, yy + 2.0f), fontSize(), m_theme->textColor);
            if (sel) r.drawText(">", Vec2(cp.x + 2.0f, yy + 2.0f), fontSize(), m_theme->textColor);
        }
    }
}

bool DialogueBox::onInput(const UIInput& in) {
    if (m_finished || !m_laidOut) return false;

    if (m_choiceActive) {
        const int n = static_cast<int>(m_choices.size());
        if (n == 0)     { m_finished = true; return true; }
        if (in.up)      { m_choiceSel = (m_choiceSel - 1 + n) % n; return true; }
        if (in.down)    { m_choiceSel = (m_choiceSel + 1) % n;     return true; }
        if (in.confirm) { if (m_onPick) m_onPick(m_choiceSel); m_finished = true; return true; }
        if (in.cancel)  { if (m_onPick) m_onPick(-1);          m_finished = true; return true; }
        return false;
    }

    if (in.confirm) {
        if (!pageRevealed()) { m_revealed = m_pages[m_page].size(); return true; }    // completa la página
        if (m_page + 1 < m_pages.size()) { ++m_page; m_revealed = 0; m_revealTimer = 0.0f; return true; }
        if (!m_choices.empty()) { m_choiceActive = true; return true; }              // pasa a la elección
        m_finished = true; return true;                                             // cierra
    }
    return false;
}

}  // namespace pk
