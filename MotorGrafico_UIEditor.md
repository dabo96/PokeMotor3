# Motor Pokémon — UI del motor (editor y herramientas)

> Módulo de herramientas para el desarrollador (NO la UI del juego). Paneles de
> depuración, inspección y afinado en vivo. Backend: **FluentUI** (Libraries/WinUI3)
> sobre Vulkan, en **modo compartido** (reusa el device/cola/command buffer del motor).

**Estado:** borrador de diseño · **Posición:** capa de overlay a nivel Engine, fuera de release

---

## UI del motor ≠ UI del juego

Son dos subsistemas distintos, no dos sabores del mismo.

| | UI del motor (editor) | UI del juego |
|---|---|---|
| Para quién | el desarrollador | el jugador |
| Tecnología | FluentUI (immediate-mode, Libraries/WinUI3) | sistema propio (retained/estilizado) |
| Estilo | funcional, denso (look Fluent 2) | artístico, animado, estética Pokémon |
| Navegación | ratón/teclado | mando, foco entre opciones |
| En release | **compilado fuera** (`#ifdef ENGINE_EDITOR`) | enviado con el juego |
| Cuándo | desde fase 1 (herramienta) | fase 7 (contenido) |

Este documento cubre solo la UI del motor.

---

## La decisión: FluentUI

Misma filosofía que vk-bootstrap / VMA: usa la librería que resuelve el problema,
tu valor está en el diseño. **FluentUI** (en `Libraries/WinUI3`) es la librería de
UI inmediata del proyecto — estilo Dear ImGui pero con look Fluent 2 y, crucialmente,
ya integrada en el motor con un **backend Vulkan en modo compartido** (graba en el
command buffer del engine, sin crear su propio device/swapchain). Trae además
docking, drag&drop, undo, atajos y serialización de layout (ver
`docs/FluentUI_Analisis.md`).

**Por qué immediate-mode encaja aquí:** la UI del motor refleja estado vivo que
cambia cada frame (FPS, valores de luces, parámetros de post). En immediate-mode
describes la UI como una **función del estado actual** — la reconstruyes cada
frame. (FluentUI es híbrido: API inmediata sobre un árbol retenido que conserva
scroll/selección/foco, lo mejor de ambos.) La UI del juego es estilizada/animada
→ sistema retained propio. Por eso son subsistemas separados.

**Sin bloat:** el editor se compila fuera en release (`#ifdef ENGINE_EDITOR`). El
juego enviado no carga el editor. Es herramienta de desarrollo, no parte del producto.

---

## Dónde encaja

Capa de overlay, dibujada la última, a nivel `Engine` (fuera de los modos de
juego), detrás de un flag de compilación.

```
[ ...render del juego... ] ─► [ game UI ] ─► [ EDITOR UI (FluentUI) ] ─► swapchain
                                                     ▲
                                         solo en builds de editor;
                                         fuera en release (#ifdef ENGINE_EDITOR)
```

---

## Arquitectura de paneles

Cada herramienta es un panel: una función que llama a FluentUI cada frame contra
**referencias vivas** a los sistemas que edita (patrón inspector).

```cpp
// Editor/EditorPanel.h
class EditorPanel {
public:
    virtual ~EditorPanel() = default;
    virtual const char* name() const = 0;
    virtual void draw() = 0;     // llama a FluentUI contra estado vivo
    bool visible = true;
};
```

Ejemplo clave: afinar el look HD-2D en vivo, sin recompilar.

```cpp
// Editor/Panels/PostProcessPanel.h
class PostProcessPanel : public EditorPanel {
public:
    PostProcessPanel(PostSettings& s) : m_s(s) {}
    const char* name() const override { return "Post-proceso"; }
    void draw() override {
        if (FluentUI::BeginPanel(name())) {
            FluentUI::SliderFloat("Bloom threshold", &m_s.bloomThreshold, 0.0f, 5.0f);
            FluentUI::SliderFloat("DoF foco",        &m_s.dofFocusDistance, 0.0f, 100.0f);
            FluentUI::SliderFloat("DoF intensidad",  &m_s.dofStrength, 0.0f, 1.0f);
        }
        FluentUI::EndPanel();
    }
private:
    PostSettings& m_s;    // referencia viva: editar aquí cambia el render YA
};
```

El orquestador posee los paneles y los recorre:

```cpp
// Editor/EditorUI.h
class EditorUI {
public:
    void init(VulkanContext& ctx, Window& window);   // FluentUI::CreateContext (Vulkan compartido)
    void addPanel(std::unique_ptr<EditorPanel> panel);

    void beginFrame(float dt);              // FluentUI::NewFrame(dt)
    void draw();                            // menubar/dockspace + recorre paneles visibles
    void render(RenderPassContext& ctx);    // SetFrameCommandBuffer + RenderDeferredDropdowns + Render

    bool wantsInput() const;                // FluentUI quiere ratón/teclado
private:
    std::vector<std::unique_ptr<EditorPanel>> m_panels;
};
```

### Paneles típicos

- **Stats** — FPS, frame time, draw calls, memoria.
- **Inspector de luces** — color, intensidad, dirección del sol en vivo.
- **Post-proceso** — bloom, DoF/tilt-shift (afinar el HD-2D).
- **Visor del render graph** — los passes y sus tiempos (depurar el frame).
- **Jerarquía de escena** (`TreeView`) y **navegador de assets** (`Grid` + drag&drop).
- **Consola** — visor del log del Core (`ScrollView` de labels).

FluentUI ya aporta **docking** (`DockSpace`/`DockPanel`) para acoplar estos paneles,
y `LayoutSerializer` para persistir el layout entre sesiones.

---

## Integración con lo existente

### Render graph: último pass

```cpp
graph.addPass("editor_ui", [&](RenderGraphBuilder& b) {
    b.write(swapchain);                       // encima de todo
    return [&](RenderPassContext& ctx) { m_editor.render(ctx); };
});
```

En modo compartido, `EditorUI::render` hace
`FluentUI::GetBackend()->SetFrameCommandBuffer(ctx.cmd)`, luego
`FluentUI::RenderDeferredDropdowns()` y `FluentUI::Render()` (el engine cierra el
render pass y hace submit/present).

### Input: enrutado

Cuando el ratón está sobre un panel, FluentUI quiere ese input y el juego no debe
recibirlo. El `Engine` enruta según `wantsInput()`:

```cpp
if (!m_editor.wantsInput())
    m_game.handleInput(m_input);   // el juego recibe input solo si FluentUI no lo usa
```

FluentUI expone esto vía `FluentUI::WantCaptureMouse()` (y la consulta equivalente
de teclado). Los `SDL_Event` se le pasan con `uiCtx->input.ProcessEvent(ev)`.

---

## Cómo crecerlo

1. **Overlay de depuración** (fase 1-2): stats + un par de inspectores, toggle con
   F1. De lo más rentable que puedes añadir temprano.
2. **Editor completo** (más adelante): jerarquía de escena, gizmos en el viewport
   (gizmo propio de mover/rotar/escalar sobre el `Image` del viewport), editor de
   tilemaps, editor de materiales.

Mismo patrón: lo simple primero, el editor como extensión.

---

## Estructura de carpetas

```
Editor/                    # todo bajo #ifdef ENGINE_EDITOR
├── EditorUI.h             # orquestador + init de FluentUI (Vulkan compartido)
├── EditorPanel.h          # interfaz
└── Panels/
    ├── StatsPanel.h
    ├── LightInspector.h
    ├── PostProcessPanel.h
    ├── RenderGraphViewer.h
    ├── SceneHierarchy.h
    ├── AssetBrowser.h
    └── ConsolePanel.h
```

---

## Lugar en el plan de implementación

Cruza la arquitectura como **herramienta de desarrollo**, no como capa del juego.
Se puede añadir desde la **fase 1-2** como overlay (stats + inspectores) y crece
en paralelo a las demás fases. Depende del `VulkanContext` (backend FluentUI en
modo compartido) y lee/edita los sistemas que inspecciona por referencia.

---

## Decisiones aún abiertas (para revisitar)

- **Gizmos de viewport**: gizmo propio (mover/rotar/escalar) sobre el viewport;
  añadir cuando exista un editor de escena real.
- **Docking / multi-viewport**: FluentUI ya lo trae (`DockSpace`); activar cuando
  los paneles crezcan.
- **Editor de tilemaps in-engine** vs editar datos a mano: el editor llega cuando
  el flujo de crear mapas a mano duela.
- **Persistir layout del editor**: vía `LayoutSerializer` de FluentUI; activar pronto.
