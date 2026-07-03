// Editor/ToolWindow.h — ventana OS secundaria (p.ej. el editor de tiles).
// Crea su propio SDL_Window + surface + swapchain + sync sobre el MISMO VkDevice
// del motor, y un segundo contexto FluentUI en modo Vulkan compartido del que
// ESTA clase hace acquire/record/submit/present. T3 del plan del editor de tiles.
#pragma once

#include "Renderer/Vulkan/VulkanSwapchain.h"

#include <vulkan/vulkan.h>
#include <SDL3/SDL_events.h>

#include <cstdint>
#include <functional>
#include <vector>

struct SDL_Window;

namespace pk {

class VulkanContext;

// windowID del evento SDL (0 si no aplica) — para rutear eventos por ventana.
inline uint32_t sdlEventWindowId(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION:      return e.motion.windowID;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:   return e.button.windowID;
        case SDL_EVENT_MOUSE_WHEEL:       return e.wheel.windowID;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:            return e.key.windowID;
        case SDL_EVENT_TEXT_INPUT:        return e.text.windowID;
        case SDL_EVENT_TEXT_EDITING:      return e.edit.windowID;
        default:
            if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST)
                return e.window.windowID;
            return 0;
    }
}

class ToolWindow {
public:
    bool open(VulkanContext& ctx, const char* title, int width, int height);
    void close();                              // destruye contexto/swapchain/ventana
    bool isOpen() const { return m_open; }
    bool wantsClose() const { return m_shouldClose; }
    uint32_t windowId() const { return m_windowId; }
    SDL_Window* window() const { return m_window; }   // para parentar diálogos nativos

    void beginInputFrame();                    // SetCurrentContext + input.Update
    void processEvent(const SDL_Event& e);     // rutea/alimenta el input de su contexto
    // Construye la UI (callback con w,h del cliente) y la presenta en esta ventana.
    void renderFrame(float dt, const std::function<void(int w, int h)>& buildUI);

private:
    bool createSync();
    void destroySync();
    bool recreateSwapchain();
    void makeCurrent();

    VulkanContext*  m_ctx       = nullptr;
    SDL_Window*     m_window    = nullptr;
    uint32_t        m_windowId  = 0;
    VkSurfaceKHR    m_surface   = VK_NULL_HANDLE;
    VulkanSwapchain m_swapchain;
    void*           m_uictx     = nullptr;     // FluentUI::UIContext* (propio)
    void*           m_backend   = nullptr;     // FluentUI::RenderBackend* (propio)

    VkCommandPool                m_cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer              m_cmd     = VK_NULL_HANDLE;
    VkSemaphore                  m_imageAvailable = VK_NULL_HANDLE;
    VkFence                      m_inFlight = VK_NULL_HANDLE;
    std::vector<VkSemaphore>     m_renderFinished;   // uno por imagen del swapchain

    bool m_open          = false;
    bool m_shouldClose   = false;
    bool m_swapchainDirty = false;
};

}  // namespace pk
