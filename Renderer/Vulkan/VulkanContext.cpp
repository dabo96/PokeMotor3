#include "VulkanContext.h"

#include "Core/Log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace pk {

bool VulkanContext::init(SDL_Window* window, bool enableValidation) {
    m_window = window;

    if (!m_instance.Initialize("PokeMotor", enableValidation))
        return false;

    // vk-bootstrap habilita las extensiones de superficie automáticamente; aquí
    // solo creamos la superficie SDL contra la instancia ya construida.
    if (!SDL_Vulkan_CreateSurface(window, m_instance.Handle(), nullptr, &m_surface)) {
        LOG_ERROR("SDL_Vulkan_CreateSurface falló: %s", SDL_GetError());
        return false;
    }

    if (!m_device.Initialize(m_instance, m_surface))    return false;
    if (!m_allocator.Initialize(m_instance, m_device))  return false;

    uint32_t w = 0, h = 0;
    drawableSize(w, h);
    if (!m_swapchain.Initialize(m_device, m_surface, w, h))
        return false;

    LOG_INFO("VulkanContext listo (%ux%u, %u imágenes de swapchain).",
             w, h, m_swapchain.ImageCount());
    m_initialized = true;
    return true;
}

void VulkanContext::drawableSize(uint32_t& width, uint32_t& height) const {
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
    width  = static_cast<uint32_t>(pw < 0 ? 0 : pw);
    height = static_cast<uint32_t>(ph < 0 ? 0 : ph);
}

void VulkanContext::immediateSubmit(const std::function<void(VkCommandBuffer)>& fn) {
    VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = m_device.GraphicsQueueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device(), &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        LOG_ERROR("immediateSubmit: vkCreateCommandPool falló");
        return;
    }

    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool        = pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device(), &ai, &cmd);

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    fn(cmd);
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cs{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cs.commandBuffer = cmd;
    VkSubmitInfo2 si{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos    = &cs;

    VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device(), &fi, nullptr, &fence);
    vkQueueSubmit2(graphicsQueue(), 1, &si, fence);
    vkWaitForFences(device(), 1, &fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(device(), fence, nullptr);
    vkDestroyCommandPool(device(), pool, nullptr);
}

bool VulkanContext::recreateSwapchain(uint32_t width, uint32_t height) {
    m_swapchain.Shutdown(m_device);
    return m_swapchain.Initialize(m_device, m_surface, width, height);
}

void VulkanContext::shutdown() {
    if (!m_initialized) return;
    m_swapchain.Shutdown(m_device);
    m_allocator.Shutdown();
    m_device.Shutdown();
    if (m_surface) {
        vkDestroySurfaceKHR(m_instance.Handle(), m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    m_instance.Shutdown();
    m_window = nullptr;
    m_initialized = false;
}

}  // namespace pk
