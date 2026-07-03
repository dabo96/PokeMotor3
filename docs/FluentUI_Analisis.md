# FluentUI (Libraries/WinUI3) — Análisis de referencia

> Análisis a fondo de la librería de UI que usa el motor (estilo ImGui con look
> Fluent 2). ~34K líneas propias (sin glm/glad/stb). `namespace FluentUI`.
> Backends OpenGL / Vulkan / DX11; el motor la usa en **modo Vulkan compartido**.

---

## 1. Qué es y cómo está hecho

- **API en modo inmediato** (como ImGui): cada frame se reconstruye la UI con
  llamadas tipo `Button(...)`, `BeginPanel(...) ... EndPanel()`. Las funciones de
  acción devuelven `bool` = "ocurrió el evento ESTE frame"; el valor editable se
  pasa **por puntero** (`bool*`, `int*`, `float*`, `std::string*`, `Color*`...).
- **Pero por debajo es retenido/reconciliado**: un `WidgetTree` de `WidgetNode`
  persiste entre frames (lookup O(1) por id-hash). `FindOrCreate(id)` reutiliza el
  nodo; `Reconcile(gracePeriod=2)` elimina los no vistos en N frames (estilo React).
  Ese árbol guarda lo que el inmediato puro no tendría: scroll, selección, caret,
  foco, animaciones, bounds cacheados, datos de accesibilidad, `styleOverride`.
- **Estado** → todo vive en un `UIContext` global (singleton, `GetContext()`),
  en `unordered_map<uint32_t,...>` por id de widget.
- **Identidad por hash de string (djb2)** del prefijo del widget + su label/id
  textual (NO la posición), para que el estado sobreviva al moverse el widget.
  Prefijos: `BTN: CHK: RADIO: SLDR_F: TXT: COMBO: PANEL: SCROLLVIEW: ...`.
  ⚠️ Dos widgets con el mismo label en el mismo scope **colisionan de estado** →
  usar labels únicos o variantes `*NoLabel(id,...)` (el 1er arg es solo id).

### Capa declarativa: `UIBuilder`
API fluida con lambdas que envuelve el modo inmediato (contenedores, widgets,
menús, overlays, docking, push/pop de estilo, helpers DPI). Es la cara
"declarativa" del mismo árbol; **modo recomendado para construir el editor**.

---

## 2. Ciclo de frame e integración en el motor (lo más importante)

### Ciclo estándar
```
SetPreferredBackend(RenderBackendType::Vulkan)              // una vez
CreateContext(window, Vulkan, &vulkanSharedContext)          // una vez
--- por frame ---
input.ProcessEvent(sdlEvent)        // alimentar input (por evento)
NewFrame(dt)                        // avanza anim, resetea flags, reconcilia árbol
... construir widgets ...           // Button(), BeginPanel()/EndPanel(), etc.
GetBackend()->SetFrameCommandBuffer(cmd)   // (Vulkan compartido) DENTRO del render pass del engine
RenderDeferredDropdowns()           // dropdowns/menús/tooltips por encima
Render()                            // dibuja todo (llama al backend en EndFrame)
// el ENGINE hace submit + present
```
- `NewFrame(dt)`: actualiza tiempo, avanza solo animaciones activas, resetea
  cursor/tooltip/flags de scroll/`mouseOverAnyWidget`, navegación Tab, GC
  amortizado, **reconcilia el WidgetTree**, y `renderer.BeginFrame(clearColor)`
  (que NO toca el backend, solo limpia buffers).
- `Render()`: dibuja preview de drag-drop y llama `renderer.EndFrame()` → aquí
  sí se invoca el backend. No hay un `EndFrame()` público aparte.

### Renderer (batching)
- Vértice `{x,y, r,g,b,a, u,v}`; shaders `Basic / Text / MSDF / Image`.
- Acumula quads/líneas; `EnsureBatchState` flushea solo si cambia shader/textura/
  color/clip; `FlushBatch` **fusiona** con el último batch compatible. Batches por
  **capa**: `Default → Overlay → Tooltip` (orden de dibujo). Clip por rects.
- Proyección ortográfica cacheada (recomputada en `SetViewport`).

### Modo Vulkan compartido (cómo lo integra el motor) — clave
- `VulkanSharedContext` (el engine rellena `instance/physicalDevice/device/
  graphicsQueue/queueFamilyIndex` + `renderPass`+`sampleCount` **o**
  `dynamicRendering=true`+`colorFormat`). Se pasa como `existingContext` a
  `CreateContext`.
- `VulkanBackend::Init` en compartido: `ownsDevice=false`, `ownsRenderPass=false`;
  crea SOLO lo suyo (shader modules, pipelines compatibles, buffers en anillo,
  sampler/descriptores, pool de staging). NO crea instance/device/swapchain ni
  sync de frame. Detecta target sRGB para linealizar colores (evita doble gamma).
- Cada frame, **dentro de su render pass activo**, el engine:
  `GetBackend()->SetFrameCommandBuffer(cmd)`. El backend graba en ESE cmd
  (`BeginFrame` solo fija viewport/scissor con Y-flip GL→VK; `EndFrame` solo
  resetea el cmd). El **engine** cierra el render pass, hace submit y present.
- `RegisterExternalTexture(VkImageView)`: envuelve una textura que YA posee el
  engine (p.ej. el viewport 3D renderizado) en un descriptor; `external=true` →
  no la destruye. Así el viewport del juego se dibuja dentro de la UI con `Image()`.
- `WantCaptureMouse()`: si el ratón estuvo sobre algún widget → el motor omite el
  picking 3D.

### Texto MSDF y fuentes
- Texto principal = **MSDF** (signed distance, nítido a cualquier escala).
- Carga `assets/fonts/atlas.png` + `atlas.json` (parser propio, sin lib JSON).
  Fallback a fuente del sistema vía FreeType + `MSDFGenerator` (genera glyphs al
  vuelo en un atlas dinámico 2048px). Icon font **Lucide** (`lucide.ttf`) a 32px.
- `FontManager`: fuentes nombradas, cadena de fallback, rangos de iconos.

---

## 3. Patrón de uso (layout, retorno, constraints)

- **Begin/End emparejados**; dibujar hijos de `BeginPanel`/`BeginModal`/
  `BeginContextMenu` **solo si devolvieron `true`**. `EndVertical/EndHorizontal`
  hacen no-op si el tipo no casa (no crashea, descuadra).
- **Cursor**: vertical → avanza Y (resetea X); horizontal → avanza X. Cada widget
  publica `lastItemPos/Size` (base de `SameLine` y `Tooltip`).
- **Posición absoluta**: casi todos aceptan `std::optional<Vec2> pos`; si se pasa,
  NO avanza el cursor.
- **Constraints** (one-shot): `SetNextConstraints(FixedSize(w,h) / FillSize() /
  FillWidth() / AutoSize())`. Muchos widgets de fila (Checkbox, Slider, TextInput,
  Combo, Separator, ProgressBar, CollapsingHeader) usan `Fill` de ancho por defecto.
- **DPI**: `GetDPIScale()`, `Scaled(v)`.
- **Overlays bloquean input**: ContextMenu/Modal bloquean todo el fondo; Combo/Menu
  solo bajo su dropdown (los clics no se "filtran").

---

## 4. Catálogo de widgets

### Básicos
| Widget | Firma (resumen) | Retorno / efecto |
|---|---|---|
| `Button` | `Button(label, size={0,0}, pos, enabled)` (+ `ButtonSize`, +icono) | `true` al clic; ripple+anim+sombra; Enter/Space con foco |
| `IconButton` | `IconButton(icon, size, pos, enabled)` | botón cuadrado solo-icono (toolbar) |
| `SegmentedControl` | `SegmentedControl(id, options, int* active, pos)` (+iconos) | `true` al cambiar; una opción activa (Move/Rotate/Scale) |
| `Label` | `Label(text, pos, variant=Body, disabled)` (+icono) | estático; `variant`=Typography (Title/Subtitle/Body/Caption…) |
| `IconLabel` | `IconLabel(icon, size, color, pos)` | icono inline (badge) |
| `LabelWrapped` | `LabelWrapped(text, maxWidth=0, ...)` | texto con word-wrap |
| `LabelRich` | `LabelRich(markup, maxWidth, pos, variant, onLinkClicked)` | markup `<b><i><color=#><size=N><a href>`; enlaces clicables |
| `Separator` | `Separator()` | línea (detecta eje H/V) |
| `Spacing`/`SameLine` | `Spacing(px)` / `SameLine(offset=0)` | espacio / mismo renglón |
| `Image` | `Image(id, void* tex, size, uv0, uv1, pos)` | dibuja textura GPU (handle backend) |
| `ProgressBar` | `ProgressBar(fraction, size, overlay, pos)` | barra determinada 0..1 + texto |
| `ColorPicker` | `ColorPicker(label, Color* value, pos)` | `true` al cambiar; HSV+RGB+hex+alpha+cuentagotas |

### Input
| Widget | Firma (resumen) | Retorno / efecto |
|---|---|---|
| `Checkbox` | `Checkbox(label, bool* value, pos)` (+icono) | `true` al cambiar; Space/Enter |
| `RadioButton` | `RadioButton(label, int* value, optionValue, group, pos)` | `true` al seleccionar; `group` aísla grupos |
| `SliderFloat` | `SliderFloat(label, float* v, min, max, width, format, pos)` | `true` al cambiar; teclado ←→/Shift/Home/End |
| `SliderInt` | `SliderInt(label, int* v, min, max, width, pos)` | idem entero |
| `TextInput` | `TextInput(label, std::string* v, width, multiline, pos, placeholder, maxLength)` (+callbacks) | edición completa: selección, Ctrl+A/C/X/V/Z/Y, IME, scroll, palabra; callbacks Edit/Completion(Tab)/History(↑↓)/CharFilter |
| `ComboBox` | `ComboBox(label, int* cur, items, width, pos)` (+iconos) | `true` al cambiar; navegación teclado |
| `ComboBoxSearchable` | `…Searchable(...)` | + filtro substring inline |
| `ComboBoxNoLabel` | `…NoLabel(id, ...)` | sin cabecera (1er arg solo id) |
| `DragFloat` | `DragFloat(label, float* v, speed, min, max, format, pos)` | arrastre horizontal; doble-clic=teclado; Shift ralentiza |
| `DragInt` | `DragInt(label, int* v, speed, min, max, pos)` | idem entero |
| `DragFloat3` | `DragFloat3(label, float v[3], speed, min, max, format, pos)` | XYZ rojo/verde/azul (pos/escala) |

### Contenedores / layout
| Widget | Firma (resumen) | Notas |
|---|---|---|
| `BeginVertical`/`EndVertical` | `BeginVertical(spacing=-1, size, padding)` | apilado vertical |
| `BeginHorizontal`/`EndHorizontal` | idem | apilado horizontal |
| `BeginGrid`/`GridNextCell`/`EndGrid` | `BeginGrid(id, columns, rowHeight=0)` | rejilla N columnas iguales |
| `BeginPanel`/`EndPanel` | `BeginPanel(id, size, reserveLayoutSpace, useAcrylic, acrylicOpacity, pos, maxHeight)` (+icono) | ventana arrastrable/colapsable/scroll/acrílico; `true`=visible |
| `CollapsingHeader` | `CollapsingHeader(label, bool* open, icon, pos)` | sección plegable (Inspector); **sin End**, auto-indenta |
| `BeginScrollView`/`EndScrollView` | `BeginScrollView(id, size, Vec2* offset, pos)` | región scrolleable |
| `BeginTabView`/`EndTabView` | `BeginTabView(id, int* active, labels, size, pos)` (+iconos) | pestañas |
| `BeginSplitter`/`SplitterPanel`/`EndSplitter` | `BeginSplitter(id, bool vertical, float* ratio, size)` | 2 paneles redimensionables, anidable |

### Listas / tablas
| Widget | Notas |
|---|---|
| `BeginListView`/`EndListView` | filas 32px, scroll virtual, indicador de selección. 4 sobrecargas: índice único / multi (`vector<int>*`) / con icono por fila. Multi: click=único, **Ctrl**=toggle, **Shift**=rango |
| `BeginTreeView`/`EndTreeView` + `TreeNode`/`TreeNodeMulti`/`TreeNodePush`/`TreeNodePop` | árbol; `TreeNode` retorna `true` si abierto y con hijos (`isOpen!=null`); indentación manual con Push/Pop; multi con Ctrl/Shift (orden DFS) |
| `BeginTable`/`TableNextRow`/`TableSetCell`/`TableRowSelectable`/`EndTable` | DataGrid: columnas ordenables (click cabecera), **redimensionables** (arrastrar borde), **congeladas** (`frozenColumns`), zebra, scroll virtual V+H, selección de filas. ⚠️ reordenar los datos lo hace el llamante (solo expone `sortColumn/sortAscending`) |

### Menús
- `BeginMenuBar`/`EndMenuBar` (barra superior 32px), `BeginMenu`/`EndMenu`,
  `MenuItem`/`MenuSeparator` (+iconos). Dropdowns **diferidos**.
- `BeginToolbar`/`EndToolbar` (40px), `BeginStatusBar`/`EndStatusBar` (24px, abajo).

### Overlays
- `Tooltip(text, delay=0.5)` (sobre el último widget; multilínea), `ProgressBar`,
  `BeginContextMenu`/`ContextMenuItem`/`ContextMenuSeparator`/`EndContextMenu`
  (click derecho, modal), `BeginModal`/`EndModal` (backdrop oscuro, arrastrable,
  X/Escape, auto-alto). `RenderDeferredDropdowns()` una vez/frame antes de `Render()`.

### Plots y fecha/hora
- `PlotLines`, `PlotHistogram` (auto-escala, ring-buffer con `offset`, tooltip de
  hover), `Sparkline` (mini-línea inline).
- `DatePicker` (calendario 7×6, "hoy", navegación mes/año), `TimePicker`
  (spinners HH:MM:SS, rueda), `DateTimePicker` (combinado). `DateTimeValue{...}`.

---

## 5. Sistemas de editor

- **Docking** (`DockSpace`/`DockNode`): árbol binario (Split/Tab/Leaf), `DockPanel
  (id, Left/Right/Top/Bottom/Center/Float, relativeTo)`, divisores arrastrables
  (6px), zonas de drop con preview, auto-compactación al quitar paneles. Base del
  layout multipanel (Hierarchy/Inspector/Viewport/Assets/Console).
- **Drag & Drop** (RAII tipado): `DragDropSource(payloadType).SetPayload<T>(...)`
  + `DragPreview(lambda)`; `DragDropTarget.AcceptPayload<T>(type, &out)` (`true`
  al soltar). Umbral 5px. Tipado evita drops incorrectos (assets→inspector, etc.).
- **Undo/Redo** (`UndoStack`): patrón Command (`execute`/`undo` lambdas), grupos
  (`BeginGroup/EndGroup`), `MakeValueCommand<T>` (captura valor anterior),
  `Undo/RedoDescription()` para el menú Edit.
- **Atajos** (`ShortcutRegistry`): `Register(actionId, KeyCombo, cb)`,
  `ProcessFrame(input)`, `GetShortcutText("Ctrl+Shift+S")` para mostrar en menús.
- **Serialización de layout** (`LayoutSerializer`): guarda/carga el árbol de
  docking + paneles + **viewports desacoplados** (multi-ventana) en texto propio.
- **Accesibilidad**: modelo por nodo (role/name/value/expanded) + eventos UIA;
  el bridge UIA está como **stub** (hooks sí, `IRawElementProviderSimple` no).
- **Animación/efectos**: `AnimatedValue<T>` (easing por enum, sin `std::function`),
  `Elevation` (sombras por profundidad z: ButtonRest=0, Flyout=8, Dialog=16),
  `RippleEffect` (onda de click). Coste cero cuando nada anima.
- **File dialogs** nativos no bloqueantes (sobre `SDL_dialog`): open/save/folder
  con `FileFilter{name, "png;jpg"}` y callback (posiblemente en otro hilo).
- **Iconos Lucide** (`Icons.h`, ~1700): enum no-scoped `Icons::Lucide : uint32_t`
  (convierte implícito a `uint32_t` → se pasa directo a cualquier `iconCodepoint`).
  Aliases semánticos: `Icons::Pointer/Move/Rotate/Cube/Trash/Open/New/Close/...`.

---

## 6. Tema / estilo

- `Style` agrega: `backgroundColor`, `accentColor`, `spacing`, `padding`,
  `Typography` (8 niveles Caption→Display), y sub-estilos `ButtonStyle`/`LabelStyle`/
  `PanelStyle`/`SeparatorStyle` con `cornerRadius`/`borderWidth`/sombra. Estados de
  color por `ColorState{normal/hover/pressed/disabled}`.
- Paleta `FluentColors` (Light/Dark, 6 acentos, semánticos Error/Success/Warning).
- Temas factory: `GetDefaultFluentStyle()`, `GetDarkFluentStyle()`,
  `GetHighContrastStyle()`, `CreateCustomFluentStyle(accent, dark)`, y
  **`GetEditorDarkStyle()`** — el que usa el motor: jerarquía de grises
  (`#141414` viewport → `#1e1e1e` toolbar → `#252525` paneles → `#2e2e2e`
  headers/botones), acento `#3b82f6`, tipografía Fluent 2 (10/12/14/16/20/24),
  paneles flat `cornerRadius=0` para docking. **Plantilla a clonar para el editor.**
- Override por scope: `UIBuilder::pushStyle/popStyle`, `pushButtonStyle`,
  `pushPanelStyle`, `pushTextColor`. Por nodo: `WidgetNode::styleOverride`.

---

## 7. Limitaciones / huecos

- **No hay toasts/notificaciones/snackbar/InfoBar** → improvisar con `Modal` o
  panel flotante en capa Tooltip (patrón de `ItemPicker.cpp`).
- **Accesibilidad UIA = stub** (modelo listo, bridge a screen readers no).
- **Colisiones de id por label duplicado** en el mismo scope (usar labels únicos
  o `*NoLabel`).
- **Tabla**: el llamante reordena los datos; `columns` se pasa por **ref no const**
  (el resize escribe `width`).
- **TreeView**: no anida solo (indentar con `TreeNodePush/Pop`).
- **Auto-grow** de Panel/ScrollView/TabView mide el frame anterior → posible 1
  frame de desfase con contenido muy dinámico.
- `DateTimeValue`: meses/días en inglés hardcodeados, semana empieza domingo.

---

## 8. Cómo lo usa el motor hoy y qué se puede construir

- El motor ya integra FluentUI en **Vulkan compartido** (`legacy/src/main.cpp` del
  motor anterior: `CreateContext(window, Vulkan, &uiShared)`, `SetUICallback` que
  llama `RenderDeferredDropdowns()` + `Render()`, viewport 3D vía
  `RegisterExternalTexture`, tema `GetEditorDarkStyle()`, fuente Lucide).
- **Editor completo construible**: `MenuBar` + `Toolbar` (iconos, `SegmentedControl`
  para gizmo) + `StatusBar`; `DockSpace` con paneles **Hierarchy** (`TreeView`/
  `ListView` multi-selección), **Inspector** (`CollapsingHeader` + `DragFloat3`/
  `ColorPicker`/`ComboBox`/`Checkbox`), **Viewport** (`Image` de la textura del
  engine), **Asset Browser** (`BeginGrid` de thumbnails + `DragDropSource`),
  **Console** (`BeginScrollView` de `Label`s). Con `ContextMenu` (click derecho),
  `Modal` (diálogos), undo/redo, atajos, y persistencia de layout.

---

*Archivos clave: `include/UI/Widgets.h` (API widgets), `include/core/Context.h`
(contexto+ciclo), `include/core/RenderBackend.h` + `VulkanBackend.*` (modo
compartido), `include/core/UIBuilder.h` (API fluida), `include/Theme/FluentTheme.*`
(tema), `include/UI/Icons.h` (iconos). Implementaciones en `src/UI/*` y `src/Core/*`.*
