# FluentUI — Cambios para multi-contexto real (backend por-contexto)

> Pendiente de portar al repo original de FluentUI/WinUI3.
> Fecha: 2026-06-20. Motivo: cerrar una 2ª ventana destruía el contexto/backend
> del editor principal (FluentUI era de contexto único global). Solución de raíz:
> el backend deja de ser un global aparte y pasa a vivir **en el contexto**, de
> modo que `GetBackend()`/`SetCurrentContext()` siguen al contexto activo y varias
> ventanas (cada una con su propio contexto) ya no comparten un único backend.

Solo se tocan **2 archivos** de la librería:

- `include/core/Context.h`
- `src/Core/Context.cpp`

(Los cambios en `Editor/ToolWindow.*`, `Editor/EditorUI.*` y `Platform/Window.cpp`
son del motor que consume la librería; ver al final "Cómo se consume".)

---

## 1) `include/core/Context.h`

### 1.1 Añadir campo `backend` a `UIContext`

```cpp
struct UIContext {
  Renderer renderer;
+ // Backend que dibuja ESTE contexto. Lo crea Create*Context y lo libera
+ // Destroy*Context. GetBackend() devuelve el backend del contexto ACTUAL, para que
+ // varias ventanas (cada una con su propio contexto) no compartan un backend global.
+ RenderBackend* backend = nullptr;
  InputState input;
  ...
```

### 1.2 Nueva sobrecarga de `CreateStandaloneContext` (con `existingContext`)

```cpp
// Create a standalone context (not the global singleton) for secondary windows
// outBackend receives the created backend pointer (for cleanup)
UIContext* CreateStandaloneContext(SDL_Window* window, RenderBackend** outBackend = nullptr);
+// Overload: pick the backend and pass its matching existingContext, so secondary
+// windows can use Vulkan shared mode (existingContext = VulkanSharedContext*) or
+// OpenGL (existingContext = SDL_GLContext / nullptr). Mirrors CreateContext().
+UIContext* CreateStandaloneContext(SDL_Window* window, RenderBackendType backend,
+                                   void* existingContext, RenderBackend** outBackend = nullptr);
```

---

## 2) `src/Core/Context.cpp`

### 2.1 Eliminar el global `g_backend`

```cpp
- static UIContext* g_ctx = nullptr;
- static RenderBackend* g_backend = nullptr;
+ // Contexto ACTUAL. El backend ya no es un global aparte: vive en g_ctx->backend,
+ // de modo que GetBackend() y SetCurrentContext() siguen al contexto activo y
+ // varias ventanas (cada una su contexto) no comparten un único backend.
+ static UIContext* g_ctx = nullptr;
```

### 2.2 `CreateContext` (singleton): backend local guardado en `g_ctx->backend`

```cpp
        g_ctx = new UIContext();
        g_ctx->window = window;

-       g_backend = CreateBackendInstance();
-       if (!g_backend->Init(window, existingGLContext)) {
+       RenderBackend* backend = CreateBackendInstance();
+       if (!backend->Init(window, existingGLContext)) {
            Log(LogLevel::Error, "Failed to initialize render backend");
            if (g_preferredBackend == RenderBackendType::OpenGL) {
                Log(LogLevel::Error, "Hint: if this is a Vulkan window, call "
                    "SetPreferredBackend(RenderBackendType::Vulkan) before CreateContext().");
            }
-           delete g_backend;
-           delete g_ctx;
-           g_backend = nullptr;
-           g_ctx = nullptr;
+           delete backend;
+           delete g_ctx;
+           g_ctx = nullptr;
            return nullptr;
        }

-       if (!g_ctx->renderer.Init(g_backend)) {
+       if (!g_ctx->renderer.Init(backend)) {
            Log(LogLevel::Error, "Failed to initialize Renderer");
-           g_backend->Shutdown();
-           delete g_backend;
-           delete g_ctx;
-           g_backend = nullptr;
-           g_ctx = nullptr;
+           backend->Shutdown();
+           delete backend;
+           delete g_ctx;
+           g_ctx = nullptr;
            return nullptr;
        }

+       g_ctx->backend = backend;
        g_ctx->style = GetDarkFluentStyle();
        ...
```

### 2.3 `GetBackend` / `RegisterExternalTexture` / `DestroyExternalTexture` → backend del contexto actual

```cpp
    RenderBackend* GetBackend() {
-       return g_backend;
+       return g_ctx ? g_ctx->backend : nullptr;
    }

    void* RegisterExternalTexture(void* nativeView, void* sampler, int layout) {
-       if (!g_backend) {
-           Log(LogLevel::Error, "RegisterExternalTexture: no active backend (call CreateContext first)");
-           return nullptr;
-       }
-       return g_backend->RegisterExternalTexture(nativeView, sampler, layout);
+       RenderBackend* be = g_ctx ? g_ctx->backend : nullptr;
+       if (!be) {
+           Log(LogLevel::Error, "RegisterExternalTexture: no active backend (call CreateContext / SetCurrentContext first)");
+           return nullptr;
+       }
+       return be->RegisterExternalTexture(nativeView, sampler, layout);
    }

    void DestroyExternalTexture(void* handle) {
-       if (g_backend && handle) g_backend->DeleteTexture(handle);
+       RenderBackend* be = g_ctx ? g_ctx->backend : nullptr;
+       if (be && handle) be->DeleteTexture(handle);
    }
```

### 2.4 `CreateStandaloneContext`: nueva sobrecarga (4 args) + compat (2 args)

Reemplaza la implementación previa de `CreateStandaloneContext(window, outBackend)` por:

```cpp
    UIContext* CreateStandaloneContext(SDL_Window* window, RenderBackendType backendType,
                                       void* existingContext, RenderBackend** outBackend) {
        if (!window) {
            Log(LogLevel::Error, "Window handle is NULL");
            return nullptr;
        }

        SetPreferredBackend(backendType);   // CreateBackendInstance() lee el preferido

        auto* ctx = new UIContext();
        ctx->window = window;

        auto* backend = CreateBackendInstance();
-       if (!backend->Init(window)) {
+       if (!backend->Init(window, existingContext)) {
            Log(LogLevel::Error, "Failed to initialize render backend for secondary window");
            delete backend;
            delete ctx;
            return nullptr;
        }

        if (!ctx->renderer.Init(backend)) {
            Log(LogLevel::Error, "Failed to initialize Renderer for secondary window");
            backend->Shutdown();
            delete backend;
            delete ctx;
            return nullptr;
        }

+       ctx->backend = backend;
        ctx->style = GetDarkFluentStyle();
        InitCursors(ctx);
        ctx->initialized = true;

        if (outBackend) *outBackend = backend;

        return ctx;
    }

+   // Compat: conserva el backend preferido actual y modo standalone (sin shared).
+   UIContext* CreateStandaloneContext(SDL_Window* window, RenderBackend** outBackend) {
+       return CreateStandaloneContext(window, GetPreferredBackend(), nullptr, outBackend);
+   }
```

> Nota: la firma de 2 args (la que usa `FluentApp`) se conserva intacta delegando
> en la de 4 args con `GetPreferredBackend()` + `existingContext = nullptr`, por lo
> que su comportamiento es idéntico al anterior.

### 2.5 `DestroyStandaloneContext`: no dejar el global colgando

```cpp
    void DestroyStandaloneContext(UIContext* ctx, RenderBackend* backend) {
        if (!ctx) return;
+       if (g_ctx == ctx) g_ctx = nullptr;   // no dejar el contexto global colgando
        DestroyCursors(ctx);
        ctx->renderer.Shutdown();
        if (backend) {
            backend->Shutdown();
            delete backend;
        }
        delete ctx;
    }
```

### 2.6 `DestroyContext` (singleton): destruir el backend del propio contexto

```cpp
    void DestroyContext() {
        if (!g_ctx) return;
        DestroyCursors(g_ctx);
        g_ctx->renderer.Shutdown();
-       if (g_backend) {
-           g_backend->Shutdown();
-           delete g_backend;
-           g_backend = nullptr;
-       }
+       if (g_ctx->backend) {
+           g_ctx->backend->Shutdown();
+           delete g_ctx->backend;
+           g_ctx->backend = nullptr;
+       }
        delete g_ctx;
        g_ctx = nullptr;
    }
```

---

## Resumen del contrato nuevo

- El backend pasa a ser **propiedad del contexto** (`UIContext::backend`).
- `GetBackend()`, `RegisterExternalTexture()`, `DestroyExternalTexture()` operan sobre
  el **contexto actual** (`g_ctx`). Requisito: llamar `SetCurrentContext(ctx)` antes
  (el editor y las ventanas de herramientas ya lo hacen en cada punto de entrada).
- Ventanas secundarias deben usar `CreateStandaloneContext(...)` /
  `DestroyStandaloneContext(...)`, **no** el singleton `CreateContext` /
  `DestroyContext`. La nueva sobrecarga permite además el **modo Vulkan compartido**
  pasando un `VulkanSharedContext*` como `existingContext`.

### Compatibilidad / riesgos
- API pública: solo se **añade** una sobrecarga; nada se rompe. `FluentApp`
  (que usa `CreateStandaloneContext(window, &backend)`) sigue igual.
- Quien llame a `GetBackend()`/`RegisterExternalTexture()` sin un contexto actual
  válido ahora obtiene `nullptr` (antes obtenía el último `g_backend`). En la práctica
  todos los call sites fijan el contexto antes; verificar en el repo original si hay
  alguno que dependiera del global.

---

## Cómo se consume desde el motor (referencia, NO va en la librería)

- `Editor/ToolWindow.cpp::open()` → `CreateStandaloneContext(win, Vulkan, &shared, &be)`,
  guarda `m_uictx` y `m_backend`.
- `Editor/ToolWindow.cpp::renderFrame()` → usa `m_backend->SetFrameCommandBuffer(cmd)`.
- `Editor/ToolWindow.cpp::close()` → `DestroyStandaloneContext(m_uictx, m_backend)`.
- `Editor/EditorUI.*` → sigue con el singleton `CreateContext`/`DestroyContext` (ventana principal).
- `Platform/Window.cpp::drainEvents()` → maneja `SDL_EVENT_WINDOW_CLOSE_REQUESTED` por
  `windowID` (la principal termina el motor; las herramientas cierran su propia ventana).
