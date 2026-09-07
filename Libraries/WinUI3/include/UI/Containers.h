#pragma once
// ─── Brief 31: capa de fachada sobre los contenedores Begin*/End* ─────────────
//
// Azúcar ADITIVO sobre las primitivas Begin*/End*. No sustituye a nada: los
// Begin*/End* siguen siendo la primitiva (control de flujo fino). Aquí se añaden
// dos capas encima, por contenedor:
//
//   1) Fachada callback  →  X(id, ..., content)     (la más legible; el 90% de usos)
//   2) Scope-guard RAII  →  ScopedX x(id, ...);      (cierre automático + control de
//                                                     flujo total: return/break)
//
// El `content` de la fachada callback va SIEMPRE al final (tras los args del Begin).
//
// Rendimiento (decisión del brief):
//   - Contenedores ESTRUCTURALES (muchos por frame): fachada `template<class F>`
//     → cero allocations, se inline-a. Definición aquí en el header.
//   - Contenedores ESPORÁDICOS (Modal/Flyout/ContextMenu/Menu/MenuBar): fachada
//     `std::function` declarada aquí, definida en src/UI/Containers.cpp (no infla
//     el header con templates que casi nadie instancia).
//
// Contrato de End (VERIFICADO caso por caso contra impl + call-sites, no adivinado):
//   - Contrato A  (Begin void; End SIEMPRE): Horizontal, Vertical, Toolbar,
//     StatusBar, Grid, UniformGrid, WrapPanel, Canvas.
//   - Contrato B2 (Begin bool; content y End SOLO si devolvió true): Panel,
//     ScrollView, TabView, Splitter, Expander, TreeView, Table, ListView(*),
//     Modal, Flyout, ContextMenu, Menu, MenuBar.
//   (*) EndListView es un no-op; se envuelve como B2 por uniformidad y seguridad.
//
// Se incluye al final de UI/Widgets.h (tras todas las declaraciones Begin*/End*).

#include <functional>
#include <utility>
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

#include "Math/Vec2.h"
#include "Math/Rect.h"

namespace FluentUI {

// Los Begin*/End* ya están declarados por Widgets.h (este header se incluye al
// final del umbrella). No se redeclaran aquí.

// ─────────────────────────────────────────────────────────────────────────────
// Helpers internos para los scope-guards.
// ─────────────────────────────────────────────────────────────────────────────
// FLUENT_SCOPED_A  → contenedor de contrato A (Begin void, End siempre).
// FLUENT_SCOPED_B  → contenedor de contrato B/B2 (Begin bool, End solo si true).
// El constructor variádico reenvía TODOS los overloads/defaults del BeginX.

#define FLUENT_SCOPED_A(ScopedName, BeginFn, EndFn)                            \
  struct ScopedName {                                                          \
    template <class... A>                                                      \
    explicit ScopedName(A &&...a) { BeginFn(std::forward<A>(a)...); }          \
    ~ScopedName() { EndFn(); }                                                 \
    ScopedName(const ScopedName &) = delete;                                   \
    ScopedName &operator=(const ScopedName &) = delete;                        \
    ScopedName(ScopedName &&) = delete;                                        \
    ScopedName &operator=(ScopedName &&) = delete;                             \
    explicit operator bool() const { return true; }                           \
  }

#define FLUENT_SCOPED_B(ScopedName, BeginFn, EndFn)                            \
  struct ScopedName {                                                          \
    bool open;                                                                 \
    template <class... A>                                                      \
    explicit ScopedName(A &&...a) : open(BeginFn(std::forward<A>(a)...)) {}    \
    ~ScopedName() { if (open) EndFn(); }                                       \
    ScopedName(const ScopedName &) = delete;                                   \
    ScopedName &operator=(const ScopedName &) = delete;                        \
    ScopedName(ScopedName &&) = delete;                                        \
    ScopedName &operator=(ScopedName &&) = delete;                             \
    explicit operator bool() const { return open; }                           \
  }

// ═════════════════════════════════════════════════════════════════════════════
// CAPA 2 — Fachadas callback (template, contenedores estructurales)
// ═════════════════════════════════════════════════════════════════════════════

// ─── Layout: Vertical / Horizontal (contrato A) ──────────────────────────────
template <class F>
inline void Vertical(F &&content) {
  BeginVertical();
  content();
  EndVertical();
}
template <class F>
inline void Vertical(float spacing, F &&content) {
  BeginVertical(spacing);
  content();
  EndVertical();
}
template <class F>
inline void Vertical(float spacing, std::optional<Vec2> size, F &&content) {
  BeginVertical(spacing, size);
  content();
  EndVertical();
}
template <class F>
inline void Vertical(float spacing, std::optional<Vec2> size,
                     std::optional<Vec2> padding, F &&content) {
  BeginVertical(spacing, size, padding);
  content();
  EndVertical();
}

template <class F>
inline void Horizontal(F &&content) {
  BeginHorizontal();
  content();
  EndHorizontal();
}
template <class F>
inline void Horizontal(float spacing, F &&content) {
  BeginHorizontal(spacing);
  content();
  EndHorizontal();
}
template <class F>
inline void Horizontal(float spacing, std::optional<Vec2> size, F &&content) {
  BeginHorizontal(spacing, size);
  content();
  EndHorizontal();
}
template <class F>
inline void Horizontal(float spacing, std::optional<Vec2> size,
                       std::optional<Vec2> padding, F &&content) {
  BeginHorizontal(spacing, size, padding);
  content();
  EndHorizontal();
}

// ─── Panel (contrato B2 estricto: false si minimizado) ───────────────────────
template <class F>
inline bool Panel(const std::string &id, F &&content) {
  bool open = BeginPanel(id);
  if (open) { content(); EndPanel(); }
  return open;
}
template <class F>
inline bool Panel(const std::string &id, const Vec2 &size, F &&content) {
  bool open = BeginPanel(id, size);
  if (open) { content(); EndPanel(); }
  return open;
}
template <class F>
inline bool Panel(const std::string &id, uint32_t iconCodepoint, F &&content) {
  bool open = BeginPanel(id, iconCodepoint);
  if (open) { content(); EndPanel(); }
  return open;
}
template <class F>
inline bool Panel(const std::string &id, uint32_t iconCodepoint,
                  const Vec2 &size, F &&content) {
  bool open = BeginPanel(id, iconCodepoint, size);
  if (open) { content(); EndPanel(); }
  return open;
}

// ─── ScrollView (contrato B2; always-true en la práctica) ────────────────────
template <class F>
inline bool ScrollView(const std::string &id, const Vec2 &size, F &&content) {
  bool open = BeginScrollView(id, size);
  if (open) { content(); EndScrollView(); }
  return open;
}
template <class F>
inline bool ScrollView(const std::string &id, const Vec2 &size,
                       Vec2 *scrollOffset, F &&content) {
  bool open = BeginScrollView(id, size, scrollOffset);
  if (open) { content(); EndScrollView(); }
  return open;
}

// ─── TabView (contrato B2: false si labels vacíos) ───────────────────────────
template <class F>
inline bool TabView(const std::string &id, int *activeTab,
                    const std::vector<std::string> &tabLabels, F &&content) {
  bool open = BeginTabView(id, activeTab, tabLabels);
  if (open) { content(); EndTabView(); }
  return open;
}
template <class F>
inline bool TabView(const std::string &id, int *activeTab,
                    const std::vector<std::string> &tabLabels, const Vec2 &size,
                    F &&content) {
  bool open = BeginTabView(id, activeTab, tabLabels, size);
  if (open) { content(); EndTabView(); }
  return open;
}
template <class F>
inline bool TabView(const std::string &id, int *activeTab,
                    const std::vector<std::pair<std::string, uint32_t>> &tabLabels,
                    F &&content) {
  bool open = BeginTabView(id, activeTab, tabLabels);
  if (open) { content(); EndTabView(); }
  return open;
}
template <class F>
inline bool TabView(const std::string &id, int *activeTab,
                    const std::vector<std::pair<std::string, uint32_t>> &tabLabels,
                    const Vec2 &size, F &&content) {
  bool open = BeginTabView(id, activeTab, tabLabels, size);
  if (open) { content(); EndTabView(); }
  return open;
}

// ─── Splitter (contrato B2; always-true en la práctica) ──────────────────────
// El cuerpo dibuja el panel A, luego SplitterPanel(), luego el panel B.
template <class F>
inline bool Splitter(const std::string &id, bool vertical, float *ratio,
                     F &&content) {
  bool open = BeginSplitter(id, vertical, ratio);
  if (open) { content(); EndSplitter(); }
  return open;
}
template <class F>
inline bool Splitter(const std::string &id, bool vertical, float *ratio,
                     const Vec2 &size, F &&content) {
  bool open = BeginSplitter(id, vertical, ratio, size);
  if (open) { content(); EndSplitter(); }
  return open;
}

// ─── Expander (contrato B2 estricto: false si colapsado) ─────────────────────
template <class F>
inline bool Expander(const std::string &id, const std::string &header,
                     F &&content) {
  bool open = BeginExpander(id, header);
  if (open) { content(); EndExpander(); }
  return open;
}
template <class F>
inline bool Expander(const std::string &id, const std::string &header,
                     uint32_t icon, F &&content) {
  bool open = BeginExpander(id, header, icon);
  if (open) { content(); EndExpander(); }
  return open;
}
template <class F>
inline bool Expander(const std::string &id, const std::string &header,
                     uint32_t icon, bool *expanded, F &&content) {
  bool open = BeginExpander(id, header, icon, expanded);
  if (open) { content(); EndExpander(); }
  return open;
}

// ─── TreeView (contrato B2; End no protegido, always-true) ───────────────────
template <class F>
inline bool TreeView(const std::string &id, const Vec2 &size, F &&content) {
  bool open = BeginTreeView(id, size);
  if (open) { content(); EndTreeView(); }
  return open;
}

// ─── Table (contrato B2: false si columnas vacías) ───────────────────────────
template <class F>
inline bool Table(const std::string &id, std::vector<TableColumn> &columns,
                  int rowCount, F &&content) {
  bool open = BeginTable(id, columns, rowCount);
  if (open) { content(); EndTable(); }
  return open;
}
template <class F>
inline bool Table(const std::string &id, std::vector<TableColumn> &columns,
                  int rowCount, const Vec2 &size, F &&content) {
  bool open = BeginTable(id, columns, rowCount, size);
  if (open) { content(); EndTable(); }
  return open;
}
template <class F>
inline bool Table(const std::string &id, std::vector<TableColumn> &columns,
                  int rowCount, const Vec2 &size, TableState *state,
                  F &&content) {
  bool open = BeginTable(id, columns, rowCount, size, state);
  if (open) { content(); EndTable(); }
  return open;
}

// ─── ListView (EndListView es no-op; envuelto como B2 por uniformidad) ───────
template <class F>
inline bool ListView(const std::string &id, const Vec2 &size, int *selectedItem,
                     const std::vector<std::string> &items, F &&content) {
  bool open = BeginListView(id, size, selectedItem, items);
  if (open) { content(); EndListView(); }
  return open;
}
template <class F>
inline bool ListView(const std::string &id, const Vec2 &size, int *selectedItem,
                     const std::vector<std::pair<std::string, uint32_t>> &items,
                     F &&content) {
  bool open = BeginListView(id, size, selectedItem, items);
  if (open) { content(); EndListView(); }
  return open;
}
template <class F>
inline bool ListView(const std::string &id, const Vec2 &size,
                     std::vector<int> *selectedItems,
                     const std::vector<std::string> &items, F &&content) {
  bool open = BeginListView(id, size, selectedItems, items);
  if (open) { content(); EndListView(); }
  return open;
}
template <class F>
inline bool ListView(const std::string &id, const Vec2 &size,
                     std::vector<int> *selectedItems,
                     const std::vector<std::pair<std::string, uint32_t>> &items,
                     F &&content) {
  bool open = BeginListView(id, size, selectedItems, items);
  if (open) { content(); EndListView(); }
  return open;
}

// ─── Grid (contrato A) ───────────────────────────────────────────────────────
template <class F>
inline void Grid(const std::string &id, int columns, F &&content) {
  BeginGrid(id, columns);
  content();
  EndGrid();
}
template <class F>
inline void Grid(const std::string &id, int columns, float rowHeight,
                 F &&content) {
  BeginGrid(id, columns, rowHeight);
  content();
  EndGrid();
}

// ─── UniformGrid (contrato A; 2 overloads: columns int / minCellWidth float) ─
template <class F>
inline void UniformGrid(const std::string &id, int columns, F &&content) {
  BeginUniformGrid(id, columns);
  content();
  EndUniformGrid();
}
template <class F>
inline void UniformGrid(const std::string &id, int columns, float gap,
                        F &&content) {
  BeginUniformGrid(id, columns, gap);
  content();
  EndUniformGrid();
}
template <class F>
inline void UniformGrid(const std::string &id, float minCellWidth, F &&content) {
  BeginUniformGrid(id, minCellWidth);
  content();
  EndUniformGrid();
}
template <class F>
inline void UniformGrid(const std::string &id, float minCellWidth, float gap,
                        F &&content) {
  BeginUniformGrid(id, minCellWidth, gap);
  content();
  EndUniformGrid();
}

// ─── WrapPanel (contrato A) ──────────────────────────────────────────────────
template <class F>
inline void WrapPanel(const std::string &id, F &&content) {
  BeginWrapPanel(id);
  content();
  EndWrapPanel();
}
template <class F>
inline void WrapPanel(const std::string &id, float hGap, float vGap,
                      F &&content) {
  BeginWrapPanel(id, hGap, vGap);
  content();
  EndWrapPanel();
}

// ─── Canvas (contrato A) ─────────────────────────────────────────────────────
template <class F>
inline void Canvas(const std::string &id, Vec2 size, F &&content) {
  BeginCanvas(id, size);
  content();
  EndCanvas();
}

// ─── Toolbar / StatusBar (contrato A) ────────────────────────────────────────
template <class F>
inline void Toolbar(F &&content) {
  BeginToolbar();
  content();
  EndToolbar();
}
template <class F>
inline void StatusBar(F &&content) {
  BeginStatusBar();
  content();
  EndStatusBar();
}
template <class F>
inline void StatusBar(const std::string &text, F &&content) {
  BeginStatusBar(text);
  content();
  EndStatusBar();
}

// ═════════════════════════════════════════════════════════════════════════════
// CAPA 2 — Fachadas callback (std::function, contenedores esporádicos)
// Declaradas aquí; definidas en src/UI/Containers.cpp. Todas contrato B2.
// ═════════════════════════════════════════════════════════════════════════════

bool Modal(const std::string &id, const std::string &title, bool *open,
           std::function<void()> content, const Vec2 &size = Vec2(400, 300));
bool Modal(const std::string &id, const std::string &title,
           uint32_t iconCodepoint, bool *open, std::function<void()> content,
           const Vec2 &size = Vec2(400, 300));

bool Flyout(const std::string &id, const Rect &anchorRect,
            std::function<void()> content,
            FlyoutPlacement placement = FlyoutPlacement::Bottom);

bool ContextMenu(const std::string &id, std::function<void()> content);

bool Menu(const std::string &label, std::function<void()> content);
bool Menu(const std::string &label, bool enabled, std::function<void()> content);
bool Menu(const std::string &label, uint32_t iconCodepoint,
          std::function<void()> content);
bool Menu(const std::string &label, uint32_t iconCodepoint, bool enabled,
          std::function<void()> content);

bool MenuBar(std::function<void()> content);

// ═════════════════════════════════════════════════════════════════════════════
// CAPA 3 — Scope-guards RAII (cierre automático + control de flujo total)
// ═════════════════════════════════════════════════════════════════════════════

// Contrato A (End siempre)
FLUENT_SCOPED_A(ScopedVertical, BeginVertical, EndVertical);
FLUENT_SCOPED_A(ScopedHorizontal, BeginHorizontal, EndHorizontal);
FLUENT_SCOPED_A(ScopedGrid, BeginGrid, EndGrid);
FLUENT_SCOPED_A(ScopedUniformGrid, BeginUniformGrid, EndUniformGrid);
FLUENT_SCOPED_A(ScopedWrapPanel, BeginWrapPanel, EndWrapPanel);
FLUENT_SCOPED_A(ScopedCanvas, BeginCanvas, EndCanvas);
FLUENT_SCOPED_A(ScopedToolbar, BeginToolbar, EndToolbar);
FLUENT_SCOPED_A(ScopedStatusBar, BeginStatusBar, EndStatusBar);

// Contrato B2 (End solo si Begin devolvió true)
FLUENT_SCOPED_B(ScopedPanel, BeginPanel, EndPanel);
FLUENT_SCOPED_B(ScopedScrollView, BeginScrollView, EndScrollView);
FLUENT_SCOPED_B(ScopedTabView, BeginTabView, EndTabView);
FLUENT_SCOPED_B(ScopedSplitter, BeginSplitter, EndSplitter);
FLUENT_SCOPED_B(ScopedExpander, BeginExpander, EndExpander);
FLUENT_SCOPED_B(ScopedTreeView, BeginTreeView, EndTreeView);
FLUENT_SCOPED_B(ScopedTable, BeginTable, EndTable);
FLUENT_SCOPED_B(ScopedListView, BeginListView, EndListView);
FLUENT_SCOPED_B(ScopedModal, BeginModal, EndModal);
FLUENT_SCOPED_B(ScopedFlyout, BeginFlyout, EndFlyout);
FLUENT_SCOPED_B(ScopedContextMenu, BeginContextMenu, EndContextMenu);
FLUENT_SCOPED_B(ScopedMenu, BeginMenu, EndMenu);
FLUENT_SCOPED_B(ScopedMenuBar, BeginMenuBar, EndMenuBar);

} // namespace FluentUI
