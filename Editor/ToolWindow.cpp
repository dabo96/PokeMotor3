// Editor/ToolWindow.cpp — ventana OS secundaria con 2º contexto FluentUI en modo
// "device compartido + swapchain propio" (CreateStandaloneContext(shareFrom),
// ownSwapchain). La librería posee surface/swapchain/sync y hace acquire/submit/
// present dentro de BeginFrame/EndFrame: aquí NO hay Vulkan a mano.
#include "Editor/ToolWindow.h"

#include "Core/Log.h"
#include "Editor/EditorTheme.h"

#include <SDL3/SDL.h>

#include "FluentGUI.h"
#include "core/RenderBackend.h"
#include "core/SDLPlatform.h"   // ProcessSDLEvent: traduce SDL_Event → UIEvent

namespace pk {

bool ToolWindow::open(void* shareFromCtx, const char* title, int width, int height) {
    if (m_open) return true;

    m_window = SDL_CreateWindow(title, width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!m_window) { LOG_ERROR("ToolWindow: SDL_CreateWindow falló: %s", SDL_GetError()); return false; }
    m_windowId = SDL_GetWindowID(m_window);

    // 2º contexto sobre el device + resource-pool del editor principal. El backend crea
    // SU surface/swapchain/render pass/sync con el instance prestado (ownSwapchain) y
    // adopta shaders/layouts/pipelines + atlas de fuentes del padre.
    FluentUI::SetPreferredBackend(FluentUI::RenderBackendType::Vulkan);
    FluentUI::RenderBackend* be = nullptr;
    FluentUI::UIContext* c = FluentUI::CreateStandaloneContext(
        m_window, static_cast<FluentUI::UIContext*>(shareFromCtx), &be);
    if (!c) {
        LOG_ERROR("ToolWindow: CreateStandaloneContext falló");
        SDL_DestroyWindow(m_window); m_window = nullptr; m_windowId = 0;
        return false;
    }
    c->style = winuiEditorStyle();
    c->renderer.LoadIconFont("assets/fonts/lucide.ttf", 16);
    m_uictx   = c;
    m_backend = be;

    // SDL3 entrega los eventos de texto POR VENTANA. CreateStandaloneContext (ventana
    // externa) no arranca la entrada de texto (sí lo hace FluentApp, que esta ventana no
    // usa), así que la habilitamos a mano para que TextInput reciba teclas aquí.
    SDL_StartTextInput(m_window);

    m_open = true;
    m_shouldClose = false;
    LOG_INFO("Ventana de herramientas abierta: %s", title);
    return true;
}

void ToolWindow::beginInputFrame() {
    if (!m_open) return;
    auto* c = static_cast<FluentUI::UIContext*>(m_uictx);
    FluentUI::SetCurrentContext(c);
    if (c) c->input.Update(m_window);
}

void ToolWindow::processEvent(const SDL_Event& e) {
    if (!m_open) return;
    if (sdlEventWindowId(e) != m_windowId) return;   // solo eventos de ESTA ventana

    if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) { m_shouldClose = true; return; }
    // El resize lo maneja el backend: SetViewport (por frame) marca el swapchain sucio y
    // BeginFrame lo recrea. No hace falta seguir el estado aquí.

    auto* c = static_cast<FluentUI::UIContext*>(m_uictx);
    FluentUI::SetCurrentContext(c);
    // El core ya no consume SDL_Event; ProcessSDLEvent traduce a UIEvent internamente.
    if (c) FluentUI::ProcessSDLEvent(c->input, e);
}

void ToolWindow::renderFrame(float dt, const std::function<void(int, int)>& buildUI) {
    if (!m_open) return;

    auto* c = static_cast<FluentUI::UIContext*>(m_uictx);
    FluentUI::SetCurrentContext(c);

    // En PÍXELES, no en coordenadas lógicas: este tamaño es el que el backend usa para
    // recrear SU swapchain y para fijar viewport/scissor. Con SDL_GetWindowSize (lógico)
    // en un display escalado, la UI se maquetaría en un rectángulo menor que la imagen.
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(m_window, &w, &h);
    if (w <= 0 || h <= 0) return;    // minimizada
    c->renderer.SetViewport(w, h);   // + detecta resize → recrea swapchain en BeginFrame

    FluentUI::NewFrame(dt);          // acquire + begin cmd/render pass (backend propio)
    if (buildUI) buildUI(w, h);
    FluentUI::RenderDeferredDropdowns();
    FluentUI::Render();              // flush + end cmd/render pass + submit + present
    c->renderer.Present();           // Vulkan: no-op (ya presentó EndFrame); contrato multi-ventana
}

void ToolWindow::close() {
    if (!m_window && !m_uictx) return;   // idempotente

    // Contexto propio de esta ventana: DestroyStandaloneContext hace vkDeviceWaitIdle y
    // destruye su swapchain/sync/surface + backend, SIN tocar el device prestado ni el
    // contexto del editor principal.
    if (m_uictx) {
        FluentUI::DestroyStandaloneContext(static_cast<FluentUI::UIContext*>(m_uictx),
                                           static_cast<FluentUI::RenderBackend*>(m_backend));
        m_uictx   = nullptr;
        m_backend = nullptr;
    }
    if (m_window) { SDL_StopTextInput(m_window); SDL_DestroyWindow(m_window); m_window = nullptr; }

    m_open = false;
    m_shouldClose = false;
    m_windowId = 0;
}

}  // namespace pk
