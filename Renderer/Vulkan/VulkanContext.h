// Renderer/Vulkan/VulkanContext.h — posee instance, surface, device, allocator
// y swapchain. Diseño: MotorGrafico_RenderGraph.md (el VulkanContext provee
// device/allocator/swapchain; el Renderer hace la sync). Deliberadamente
// pequeño: NO es el god-object del motor anterior.
#pragma once

#include "VulkanAllocator.h"
#include "VulkanDevice.h"
#include "VulkanInstance.h"
#include "VulkanSwapchain.h"

#include <cstdint>
#include <functional>

struct SDL_Window;

namespace pk {

class VulkanContext {
public:
    bool init(SDL_Window* window, bool enableValidation);
    void shutdown();

    // Destruye y reconstruye la swapchain al nuevo tamaño. Llama vkDeviceWaitIdle
    // antes (el caller debe recrear lo que dependa del nº de imágenes).
    bool recreateSwapchain(uint32_t width, uint32_t height);

    // Tamaño del framebuffer en píxeles (no en coords lógicas).
    void drawableSize(uint32_t& width, uint32_t& height) const;

    // Graba y ejecuta un bloque de comandos de una sola vez (carga de texturas,
    // subidas a GPU): crea un command buffer transitorio, lo envía y ESPERA.
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& fn);

    // --- Accesores ---
    VkDevice         device()         const { return m_device.Handle(); }
    VkPhysicalDevice physicalDevice() const { return m_device.PhysicalHandle(); }
    VkQueue          graphicsQueue()  const { return m_device.GraphicsQueue(); }
    VkQueue          presentQueue()   const { return m_device.PresentQueue(); }
    uint32_t         graphicsFamily() const { return m_device.GraphicsQueueFamily(); }
    VmaAllocator     allocator()      const { return m_allocator.Handle(); }
    VkInstance       instance()       const { return m_instance.Handle(); }
    SDL_Window*      window()         const { return m_window; }

    VulkanSwapchain&       swapchain()       { return m_swapchain; }
    const VulkanSwapchain& swapchain() const { return m_swapchain; }

    // Para crear swapchains adicionales (p.ej. una segunda ventana del editor).
    const VulkanDevice& deviceObj() const { return m_device; }

private:
    SDL_Window*     m_window  = nullptr;
    VkSurfaceKHR    m_surface = VK_NULL_HANDLE;
    VulkanInstance  m_instance;
    VulkanDevice    m_device;
    VulkanAllocator m_allocator;
    VulkanSwapchain m_swapchain;
    bool            m_initialized = false;
};

}  // namespace pk
