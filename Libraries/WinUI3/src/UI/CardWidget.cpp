// ─── Brief 32: widget Card (data-driven) + one-shots ─────────────────────────
//
// Un solo contenedor Card parametrizado por CardConfig. Es una fachada semántica
// sobre primitivas existentes: material (WidgetRole::Card) + sombra por elevación
// + fill/borde SDF (que consumen reveal) + un scope de layout. Layout modelado
// sobre BeginPanel (medir→clip→cursor interior→AdvanceCursor) pero sin
// título/drag/resize. El auto-alto usa el alto de contenido medido el frame
// anterior (1 frame de latencia, igual que Panel).

#include "UI/CardWidget.h"
#include "UI/Widgets.h"        // Label/Button/Image/BeginVertical/Horizontal facade…
#include "UI/WidgetHelpers.h"
#include "core/Context.h"
#include "core/UIKey.h"
#include "Theme/Material.h"
#include "core/Elevation.h"

#include <algorithm>
#include <vector>
#include <utility>
#include <unordered_map>

namespace FluentUI {

namespace {

// Marco transitorio Begin→End de una card.
struct CardFrame {
  uint32_t id = 0;
  Vec2 pos, size;
  float padding = 0.0f;
  float radius = 0.0f;
  bool clickable = false, selectable = false, interactive = false;
  bool hasFocus = false;
  bool *selectedPtr = nullptr;
  Color accent;
  // captura de interactivos internos (auto-exclusión); guarda el scope externo.
  size_t captureFocusStart = 0;
  bool prevCapActive = false;
  size_t prevCapFocusStart = 0;
  std::vector<std::pair<uint32_t, Rect>> prevCapItems;
};

std::vector<CardFrame> g_cardStack;

// Ancho de contenido medido el frame anterior (para fitContent / shrink-to-fit),
// por id de card. 1 frame de latencia, igual que el auto-alto.
std::unordered_map<uint32_t, float> g_cardContentW;

// Flag en WidgetState.intVal.
constexpr int kFinalized = 1 << 1; // valor finalizado el frame anterior (retorno de BeginCard)

// Mapea CardConfig + estado → FluentMaterial (parte de ResolveMaterial(Card) y
// ajusta por CardStyle). Sin render nuevo.
FluentMaterial ResolveCardMaterial(UIContext *ctx, const CardConfig &cfg,
                                    WidgetState st, bool interactive, float radius) {
  const ColorTokens tok = Tokens::FromStyle(ctx->style);
  FluentMaterial m = ResolveMaterial(WidgetRole::Card, st, ctx->style);
  m.radius = radius;
  switch (cfg.style) {
  case CardStyle::Elevated:
    m.elevationZ = cfg.elevationZ >= 0.0f ? cfg.elevationZ : Elevation::Z::Card;
    m.borderWidth = 0.0f;
    break;
  case CardStyle::Outlined:
    m.elevationZ = 0.0f;
    m.borderWidth = std::max(1.0f, ctx->dpiScale);
    // Conservar el borde de contenedor del panel (ps.borderColor =
    // ContainerBorder*, diseñado para ser VISIBLE) en vez del trazo genérico
    // tok.controlStrokeDefault, demasiado tenue sobre el fondo oscuro de la card
    // y que hacía que el borde en reposo pareciese ausente.
    break;
  case CardStyle::Filled:
    m.elevationZ = 0.0f;
    m.borderWidth = 0.0f;
    // brief 34 Parte B: una card "Filled" usa el fill de card WinUI (compone sobre
    // el fondo) en vez del antiguo surfaceAlt opaco calibrado a ojo.
    m.fill = tok.cardBackgroundSecondary;
    break;
  case CardStyle::Acrylic:
    // Brief 34 Parte F: sin acrylic en HighContrast (fill opaco, como Filled).
    m.acrylic = !ctx->style.isHighContrast;
    m.elevationZ = cfg.elevationZ >= 0.0f ? cfg.elevationZ : Elevation::Z::Card;
    break;
  }
  // Una card clicable se eleva un punto en hover (como los botones).
  if (interactive && st == WidgetState::Hover) m.elevationZ += 2.0f;
  // Feedback de puntero = el FONDO (trazo sutil WinUI compuesto sobre el fill), nunca
  // el borde. Sustituye al reveal (que iluminaba la banda del contorno por proximidad
  // del cursor y se leía como "el borde cambia de color al hacer hover").
  if (interactive &&
      (st == WidgetState::Hover || st == WidgetState::Pressed)) {
    const Color ov = (st == WidgetState::Pressed) ? tok.subtleFillTertiary
                                                  : tok.subtleFillSecondary;
    m.fill = Color(m.fill.r + (ov.r - m.fill.r) * ov.a,
                   m.fill.g + (ov.g - m.fill.g) * ov.a,
                   m.fill.b + (ov.b - m.fill.b) * ov.a, m.fill.a);
  }
  m.revealIntensity = (cfg.reveal && interactive &&
                       (st == WidgetState::Hover || st == WidgetState::Pressed))
                          ? 1.0f
                          : 0.0f;
  return m;
}

bool EndCardImpl();

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
bool BeginCard(const std::string &id, const CardConfig &cfg, bool *selected) {
  UIContext *ctx = GetContext();
  if (!ctx) return false;

  const uint32_t cardId = GenerateId("CARD:", id.c_str());
  auto &ws = ctx->GetWidgetState(cardId);
  auto S = [&](float v) { return DPIScale(ctx, v); };

  const bool interactive = cfg.clickable || cfg.selectable;
  const float radius = cfg.radius > 0.0f ? cfg.radius : CornerTokens::Medium;
  const float pad = cfg.padding > 0.0f ? cfg.padding : S(16.0f);

  // ── Tamaño (auto-alto con 1 frame de latencia, como Panel) ──
  LayoutConstraints constraints = ConsumeNextConstraints();
  Vec2 avail = GetCurrentAvailableSpace(ctx);
  float w;
  if (cfg.fitContent) {
    // shrink-to-fit: ancho = ancho del contenido medido el frame anterior + padding.
    // MeasureText queda un poco por debajo del ancho REAL de render, y ese desfase
    // crece con la longitud del texto, así que se compensa PROPORCIONALMENTE (con
    // cota para no pasarse en textos muy largos/cortos). Con la compensación, el
    // elemento más ancho llena su caja y el padding queda ~simétrico a ambos lados.
    auto it = g_cardContentW.find(cardId);
    float mw = (it != g_cardContentW.end()) ? it->second : 0.0f;
    float comp = std::clamp(mw * 0.05f, S(2.0f), S(20.0f));
    w = mw > 0.0f ? mw + comp + pad * 2.0f
                  : (avail.x > 0.0f ? avail.x : S(280.0f));
  } else {
    w = cfg.size.x > 0.0f ? cfg.size.x : (avail.x > 0.0f ? avail.x : S(280.0f));
  }
  float measured = ws.floatVal; // alto de contenido medido el frame anterior
  float h = cfg.size.y > 0.0f
                ? cfg.size.y
                : (measured > 0.0f ? measured + pad * 2.0f : pad * 2.0f + S(24.0f));
  Vec2 finalSize = ApplyConstraints(ctx, constraints, Vec2(w, h));
  Vec2 pos = ctx->cursorPos;

  // ── Foco por teclado (solo cards interactivas) ──
  bool hasFocus = false;
  if (interactive) {
    ctx->focusableWidgets.push_back(cardId);
    if (ctx->focusIndex < 0 && ctx->focusableWidgets.size() == 1) {
      ctx->focusIndex = 0;
      ctx->focusedWidgetId = cardId;
    }
    hasFocus = (ctx->focusedWidgetId == cardId);
  }

  // ── Hit-test / estado (solo si interactiva) ──
  Vec2 mouse(ctx->input.MouseX(), ctx->input.MouseY());
  bool hover = interactive && !IsMouseInputBlocked(ctx) &&
               PointInRect(mouse, pos, finalSize);

  WidgetState st = WidgetState::Rest;
  if (interactive) {
    bool pressing = hover && ctx->input.IsMouseDown(0);
    if (pressing)      st = WidgetState::Pressed;
    else if (hover)    st = WidgetState::Hover;
    else if (hasFocus) st = WidgetState::Focused;
  }

  // ── Material data-driven + dibujo (sombra → fill/borde, con reveal) ──
  FluentMaterial m = ResolveCardMaterial(ctx, cfg, st, interactive, radius);
  if (IsRectInViewport(ctx, pos, finalSize)) {
    if (m.elevationZ > 0.0f)
      ctx->renderer.DrawElevationShadow(pos, finalSize, m.radius, m.elevationZ);
    if (hasFocus && interactive)
      DrawFocusRing(ctx, pos, finalSize, m.radius);
    ctx->renderer.SetNextRevealIntensity(m.revealIntensity);
    if (m.acrylic)
      ctx->renderer.DrawRectAcrylic(pos, finalSize, m.fill, m.radius,
                                    m.luminosityOpacity);
    else
      ctx->renderer.DrawRectFilled(pos, finalSize, m.fill, m.radius);
    // Selección conocida a la entrada (se refleja el frame siguiente). Se calcula
    // ANTES del borde base para poder omitirlo cuando la card está seleccionada:
    // ahí el borde de acento ES el borde, y pintar además el bisel base (brief 34
    // Parte E) los mezclaba y ensuciaba el contorno de acento.
    const bool sel = cfg.selectable && (selected ? *selected : ws.boolVal);

    if (m.borderWidth > 0.0f && !sel)
      ctx->renderer.DrawRect(pos, finalSize, m.border, m.radius, m.borderBottom);

    // ── Marca de selección: borde de acento (2px) ──
    // Se dibuja AQUÍ, antes del PushClipRect, igual que el fondo/borde base — que
    // sí se pintan en todas las cards. Dibujarla en EndCard (tras PopClipRect)
    // hacía que solo sobreviviese la de la ÚLTIMA card por la interacción con el
    // clip/batch de las cards siguientes.
    if (sel) {
      ctx->renderer.DrawRect(pos, finalSize, ctx->style.accentColor, m.radius);
      ctx->renderer.DrawRect(pos + Vec2(1.0f, 1.0f), finalSize - Vec2(2.0f, 2.0f),
                             ctx->style.accentColor, std::max(0.0f, m.radius - 1.0f));
    }
  }

  // ── Scope de contenido (clip → cursor interior → layout vertical) ──
  // Recortar al ÁREA DE CONTENIDO (con padding), no al rect completo de la card,
  // para que el contenido respete el padding en TODOS los lados (antes el texto
  // largo invadía el padding derecho hasta el borde). Igual que hace BeginPanel.
  Vec2 contentOrigin = pos + Vec2(pad, pad);
  float cw = std::max(0.0f, finalSize.x - pad * 2.0f);
  float ch = std::max(0.0f, finalSize.y - pad * 2.0f);
  // Confirmar el fondo (sombra + fill redondeado + borde) con el clip del PADRE
  // ANTES de empujar el clip del contenido. PushClipRect NO vacía el batch, así que
  // sin este flush el scissor del clip interior recorta RETROACTIVAMENTE el fondo
  // pendiente a un rectángulo inset → esquinas redondeadas cortadas a cuadradas y
  // sombra perdida. El Acrylic no sufría esto porque DrawRectAcrylic hace su propio
  // flush interno. Mismo patrón que Flyout (OverlayWidgets) y el fix del PasswordBox.
  ctx->renderer.FlushBatch();
  // fitContent: la card ya se ajusta al contenido, así que NO se recorta
  // horizontalmente (evita cortar el elemento más ancho por el desfase de
  // MeasureText). El resto recorta al área de contenido con padding.
  if (cfg.fitContent)
    ctx->renderer.PushClipRect(Vec2(pos.x, contentOrigin.y), Vec2(finalSize.x, ch));
  else
    ctx->renderer.PushClipRect(contentOrigin, Vec2(cw, ch));
  ctx->cursorPos = contentOrigin;
  BeginVertical(ctx->style.spacing, Vec2(cw, 0.0f), Vec2(0.0f, 0.0f));
  PushID(id.c_str());

  CardFrame fr;
  fr.id = cardId;
  fr.pos = pos;
  fr.size = finalSize;
  fr.padding = pad;
  fr.radius = radius;
  fr.clickable = cfg.clickable;
  fr.selectable = cfg.selectable;
  fr.interactive = interactive;
  fr.hasFocus = hasFocus;
  fr.selectedPtr = selected;
  fr.accent = ctx->style.accentColor;

  // Auto-exclusión: capturar los bboxes de los interactivos internos (guardando
  // el scope externo para poder anidar cards).
  if (interactive) {
    fr.prevCapActive = ctx->interactiveCapture.active;
    fr.prevCapFocusStart = ctx->interactiveCapture.focusStart;
    fr.prevCapItems = std::move(ctx->interactiveCapture.items);
    ctx->interactiveCapture.active = true;
    ctx->interactiveCapture.focusStart = ctx->focusableWidgets.size();
    ctx->interactiveCapture.items.clear();
    fr.captureFocusStart = ctx->interactiveCapture.focusStart;
  }

  g_cardStack.push_back(std::move(fr));

  // Valor finalizado el frame anterior (ver nota en el header).
  return (ws.intVal & kFinalized) != 0;
}

namespace {

bool EndCardImpl() {
  UIContext *ctx = GetContext();
  if (!ctx || g_cardStack.empty()) return false;

  CardFrame fr = std::move(g_cardStack.back());
  g_cardStack.pop_back();
  auto &ws = ctx->GetWidgetState(fr.id);

  // Medir el alto y el ancho del contenido (como EndPanel). contentSize.x del
  // layout vertical = ancho del hijo más ancho (para shrink-to-fit).
  float cursorH = 0.0f;
  float contentW = 0.0f;
  if (!ctx->layoutStack.empty()) {
    auto &stk = ctx->layoutStack.back();
    cursorH = stk.cursor.y - stk.contentStart.y;
    if (stk.itemCount > 0 && stk.spacing > 0.0f) cursorH -= stk.spacing;
    contentW = stk.contentSize.x;
  }
  EndVertical(false);
  float measuredH = std::max(cursorH, ctx->lastItemSize.y);
  ws.floatVal = measuredH;           // para el auto-alto del próximo frame
  g_cardContentW[fr.id] = contentW;  // para el shrink-to-fit del próximo frame

  PopID();
  // Confirmar el contenido con el clip interior ANTES de restaurarlo (simétrico al
  // flush de BeginCard): PopClipRect tampoco vacía el batch.
  ctx->renderer.FlushBatch();
  ctx->renderer.PopClipRect();

  // Recolectar rects de interactivos internos y restaurar el scope externo.
  std::vector<Rect> excluded;
  if (fr.interactive) {
    excluded = CollectInteractiveRects(ctx, fr.captureFocusStart,
                                       ctx->interactiveCapture.items);
    ctx->interactiveCapture.active = fr.prevCapActive;
    ctx->interactiveCapture.focusStart = fr.prevCapFocusStart;
    ctx->interactiveCapture.items = std::move(fr.prevCapItems);
  }

  // ── Finalizar activación (click válido = release dentro + press empezó dentro
  //    + NO cayó sobre un interactivo interno). También Enter/Space con foco. ──
  bool activated = false;
  if (fr.interactive) {
    // Activación basada en press (como Button: clicked = hover && IsMousePressed),
    // excluyendo el press que cayó sobre un interactivo interno capturado.
    Vec2 mouse(ctx->input.MouseX(), ctx->input.MouseY());
    bool pressed = ctx->input.IsMousePressed(0);
    bool insideCard = PointInRect(mouse, fr.pos, fr.size);
    bool onChild = false;
    for (const Rect &r : excluded) {
      if (r.Contains(mouse)) { onChild = true; break; }
    }
    if (pressed && insideCard && !onChild) activated = true;
    if (fr.hasFocus && (ctx->input.IsKeyPressed(UIKey::Enter) ||
                        ctx->input.IsKeyPressed(UIKey::KeypadEnter) ||
                        ctx->input.IsKeyPressed(UIKey::Space)))
      activated = true;
  }

  // ── Selección (toggle en activación) ──
  bool selected = fr.selectedPtr ? *fr.selectedPtr : ws.boolVal;
  if (fr.selectable && activated) {
    selected = !selected;
    if (fr.selectedPtr) *fr.selectedPtr = selected;
    else ws.boolVal = selected;
  }

  // La marca de selección (borde de acento) se dibuja ahora en BeginCard (antes
  // del clip) para que se pinte en TODAS las cards, no solo en la última. Aquí
  // solo se actualiza el estado; el borde se verá con el valor nuevo el frame que
  // viene (lag de 1 frame, aceptable en immediate-mode).

  // Retorno + persistir para el BeginCard del próximo frame.
  bool ret = fr.selectable ? selected : activated;
  if (ret) ws.intVal |= kFinalized;
  else ws.intVal &= ~kFinalized;

  ctx->lastItemPos = fr.pos;
  AdvanceCursor(ctx, fr.size);
  return ret;
}

} // namespace

void EndCard() { EndCardImpl(); }

bool Card(const std::string &id, const CardConfig &cfg,
          std::function<void()> content) {
  BeginCard(id, cfg, nullptr);
  if (content) content();
  return EndCardImpl();
}

// ─────────────────────────────────────────────────────────────────────────────
// One-shots de conveniencia
// ─────────────────────────────────────────────────────────────────────────────

bool InfoCard(const std::string &id, const std::string &title,
              const std::string &description, uint32_t icon, bool clickable) {
  CardConfig cfg;
  cfg.style = CardStyle::Elevated;
  cfg.clickable = clickable;
  return Card(id, cfg, [&] {
    if (icon != 0u)
      Label(title, icon, std::nullopt, TypographyStyle::Subtitle);
    else
      Label(title, std::nullopt, TypographyStyle::Subtitle);
    Spacing(4.0f);
    LabelWrapped(description, 0.0f, std::nullopt, TypographyStyle::Caption);
  });
}

void SettingsCard(const std::string &id, uint32_t icon, const std::string &title,
                  const std::string &subtitle, std::function<void()> action) {
  CardConfig cfg;
  cfg.style = CardStyle::Filled;
  BeginCard(id, cfg, nullptr);
  // Layout horizontal: [icono + (título/subtítulo)]  ...  [action]. La `action`
  // queda auto-excluida de la activación por el mecanismo de captura de BeginCard.
  Horizontal(12.0f, [&] {
    if (icon != 0u)
      Label(std::string(), icon, std::nullopt, TypographyStyle::Body);
    Vertical(2.0f, [&] {
      Label(title, std::nullopt, TypographyStyle::BodyStrong);
      if (!subtitle.empty())
        Label(subtitle, std::nullopt, TypographyStyle::Caption);
    });
    if (action) action();
  });
  EndCard();
}

bool MediaCard(const std::string &id, void *imageTexture, Vec2 imageSize,
               std::function<void()> content, bool clickable) {
  CardConfig cfg;
  cfg.style = CardStyle::Elevated;
  cfg.clickable = clickable;
  return Card(id, cfg, [&] {
    if (imageTexture)
      Image(id + "_img", imageTexture, imageSize);
    // Nota (brief 32, fuera de alcance): el recorte de la imagen a las esquinas
    // superiores redondeadas se hará cuando el Renderer soporte clip redondeado
    // por-imagen; por ahora la card redondeada queda por encima.
    if (content) {
      Spacing(8.0f);
      content();
    }
  });
}

} // namespace FluentUI
