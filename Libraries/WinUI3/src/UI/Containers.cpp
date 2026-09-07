// ─── Brief 31: fachadas callback std::function (contenedores esporádicos) ────
//
// Definiciones de las fachadas de Modal/Flyout/ContextMenu/Menu/MenuBar. Se
// mantienen fuera del header (a diferencia de las estructurales template) para
// no inflar el umbrella con templates que casi nadie instancia. Todas siguen el
// contrato B2: el `content` y el End se ejecutan SOLO si el Begin devolvió true
// (verificado contra impl + call-sites). Un End emparejado mal corrompe el stack
// de layout — por eso B2 es obligatorio aquí, no una preferencia estética.

#include "UI/Widgets.h" // trae Begin*/End* + Containers.h (umbrella)

namespace FluentUI {

// ─── Modal (B2; EndModal NO tiene guard defensivo → B2 imprescindible) ───────
bool Modal(const std::string &id, const std::string &title, bool *open,
           std::function<void()> content, const Vec2 &size) {
  bool visible = BeginModal(id, title, open, size);
  if (visible) {
    content();
    EndModal();
  }
  return visible;
}

bool Modal(const std::string &id, const std::string &title,
           uint32_t iconCodepoint, bool *open, std::function<void()> content,
           const Vec2 &size) {
  bool visible = BeginModal(id, title, iconCodepoint, open, size);
  if (visible) {
    content();
    EndModal();
  }
  return visible;
}

// ─── Flyout (B2; EndFlyout balancea PushOpacity de BeginFlyout) ──────────────
bool Flyout(const std::string &id, const Rect &anchorRect,
            std::function<void()> content, FlyoutPlacement placement) {
  bool open = BeginFlyout(id, anchorRect, placement);
  if (open) {
    content();
    EndFlyout();
  }
  return open;
}

// ─── ContextMenu (B2) ────────────────────────────────────────────────────────
bool ContextMenu(const std::string &id, std::function<void()> content) {
  bool open = BeginContextMenu(id);
  if (open) {
    content();
    EndContextMenu();
  }
  return open;
}

// ─── Menu (B2; EndMenu popea menuIdStack — B2 obligatorio en anidamiento) ────
bool Menu(const std::string &label, std::function<void()> content) {
  bool open = BeginMenu(label);
  if (open) {
    content();
    EndMenu();
  }
  return open;
}

bool Menu(const std::string &label, bool enabled,
          std::function<void()> content) {
  bool open = BeginMenu(label, enabled);
  if (open) {
    content();
    EndMenu();
  }
  return open;
}

bool Menu(const std::string &label, uint32_t iconCodepoint,
          std::function<void()> content) {
  bool open = BeginMenu(label, iconCodepoint);
  if (open) {
    content();
    EndMenu();
  }
  return open;
}

bool Menu(const std::string &label, uint32_t iconCodepoint, bool enabled,
          std::function<void()> content) {
  bool open = BeginMenu(label, iconCodepoint, enabled);
  if (open) {
    content();
    EndMenu();
  }
  return open;
}

// ─── MenuBar (B2 por convención; BeginMenuBar solo devuelve false si !ctx) ────
bool MenuBar(std::function<void()> content) {
  bool open = BeginMenuBar();
  if (open) {
    content();
    EndMenuBar();
  }
  return open;
}

} // namespace FluentUI
