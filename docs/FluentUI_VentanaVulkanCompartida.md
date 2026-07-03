# FluentUI — 3er modo Vulkan: device compartido + swapchain propio

> Spec para extender FluentUI de modo que una **ventana secundaria** comparta el
> `VkDevice` del motor pero **posea su propio surface/swapchain/present**. Hoy esto
> se hace a mano en `Editor/ToolWindow.cpp`; el objetivo es plegarlo en la librería
> para que `ToolWindow` quede como un wrapper fino.

## Por qué
FluentUI tiene 2 modos (`Libraries/WinUI3/include/core/VulkanBackend.h:11-18`):
- **Shared** (`existingContext != null`): toma prestado el device del engine y graba
  en el command buffer que le pasa el host (`SetFrameCommandBuffer`); **no presenta**.
- **Standalone** (`existingContext == null`): crea su propio instance/device/swapchain
  y maneja el frame entero (acquire→submit→present).

El multi-ventana de fábrica (`FluentApp`/`AppWindow`/`detachPanelToWindow`) es **OpenGL**
(`src/Core/FluentApp.cpp:20-65`: `SDL_WINDOW_OPENGL`, `SDL_GL_SHARE_WITH_CURRENT_CONTEXT`,
`SDL_GL_MakeCurrent`). El standalone Vulkan crea un **device aparte** → no puede compartir
texturas con el motor (`RegisterExternalTexture` es por-device).

Falta un 3er modo: **device prestado + swapchain propio**. Es lo que necesita una 2ª
ventana (p.ej. el editor de tiles) que quiere reusar las texturas del motor (atlas/sprites).

**Idea clave**: el flag `sharedMode` mezcla dos cosas → (a) "no soy dueño del device" y
(b) "no manejo el frame (grabo en cmd externo, sin present)". `ownsDevice` ya está aparte.
El modo nuevo quiere **(a) sí** (no own device) pero **(b) no** (sí manejar el frame).

## Cambios

### 1) `include/core/RenderBackend.h` — struct `VulkanSharedContext`
```cpp
bool ownSwapchain = false;   // device compartido pero el backend posee surface+swapchain+present de `window`
```

### 2) `VulkanBackend::Init` (rama shared, ~líneas 90-150)
Si `shared->ownSwapchain`:
- Toma prestados los handles (instance/physicalDevice/device/graphicsQueue/queueFamilyIndex),
  `window = (SDL_Window*)windowHandle`, `ownsDevice = false`, **`sharedMode = false`**
  (para que `BeginFrame/EndFrame` usen el camino que maneja el frame), `useDynamicRendering = false`
  (usa su propio render pass).
- Crea el surface con el instance prestado: `SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)`.
- Corre los pasos del standalone **menos `CreateInstanceAndDevice`**, en este orden
  (importa: `CreateSwapchain` fija `colorFormat` antes de `CreatePipelines`):
  `CreateSwapchain()` → `CreateShaderModules()` → `CreatePipelines()` →
  `CreateDynamicBuffers()` → `CreateSamplerAndDescriptorInfra()` → `CreateSyncAndCommands()` →
  crear `uploadPool`.

Recomendado: extraer `CreateSurface()` y `CreatePresentResources()` (los pasos de arriba) y
reusarlos en standalone (`CreateInstanceAndDevice` + `CreateSurface` + `CreatePresentResources`)
y en el modo nuevo (borrow + `CreateSurface` + `CreatePresentResources`).

### 3) `Shutdown` (líneas 1322-1343)
Hoy destruye swapchain/sync/commandPool/**surface** solo `if (ownsDevice)`. En el modo nuevo
`ownsDevice == false` pero **sí** sos dueño de eso → cambiar esos gates a **`if (!sharedMode)`**
(cubre standalone + modo nuevo), y **mover `vkDestroySurfaceKHR`** del bloque de device (1339)
a ese bloque (antes del instance). El device/instance siguen gated en `ownsDevice`.

### 4) `BeginFrame`/`EndFrame`/`SetViewport`
Nada que tocar: ya ramifican por `sharedMode`. Con `sharedMode=false` el modo nuevo usa el
camino acquire/render-pass/submit/present (1049-1133) y el resize por `SetViewport` (1139).
El host **no** debe llamar `SetFrameCommandBuffer` en este modo.

## Uso resultante (host)
```cpp
SDL_Window* win = SDL_CreateWindow(title, w, h, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
FluentUI::VulkanSharedContext sc{};
sc.instance = eng.instance(); sc.physicalDevice = eng.physicalDevice();
sc.device = eng.device(); sc.graphicsQueue = eng.graphicsQueue(); sc.queueFamilyIndex = eng.graphicsFamily();
sc.ownSwapchain = true;
FluentUI::SetPreferredBackend(FluentUI::RenderBackendType::Vulkan);
FluentUI::UIContext* c = FluentUI::CreateContext(win, FluentUI::RenderBackendType::Vulkan, &sc);
// por frame: SetCurrentContext(c); c->input.Update(win)/ProcessEvent; NewFrame(dt); buildUI(); Render();  // presenta solo
// cerrar:    SetCurrentContext(c); DestroyContext(); SDL_DestroyWindow(win);
```

Tras esto, `Editor/ToolWindow.cpp` se reduce a: crear ventana, `SetCurrentContext` + ruteo de
eventos, `NewFrame/build/Render`, cerrar. Toda la parte Vulkan (surface/swapchain/sync/present)
vive en `VulkanBackend`. Y como comparte el device, el editor de tiles puede `RegisterExternalTexture`
las `VkImageView` del motor (atlas/sprites).

## Estado actual
Mientras esto no se aplique, `ToolWindow` implementa el modo nuevo a mano (shared device +
swapchain/present propios) y funciona igual. Al aplicar el cambio, adaptar `ToolWindow` a la
API nueva (queda como wrapper).
