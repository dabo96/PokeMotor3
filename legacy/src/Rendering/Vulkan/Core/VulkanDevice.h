#pragma once

#include <VkBootstrap.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace pokemotor::vk {

class VulkanInstance;

class VulkanDevice {
public:
    bool Initialize(const VulkanInstance& instance, VkSurfaceKHR surface);
    void Shutdown();

    VkPhysicalDevice PhysicalHandle() const { return m_physical.physical_device; }
    VkDevice Handle() const { return m_device.device; }
    VkQueue GraphicsQueue() const { return m_graphicsQueue; }
    VkQueue PresentQueue() const { return m_presentQueue; }
    uint32_t GraphicsQueueFamily() const { return m_graphicsFamily; }
    uint32_t PresentQueueFamily() const { return m_presentFamily; }
    const vkb::Device& Bootstrap() const { return m_device; }
    // True only if the optional wideLines feature was present and enabled.
    // Pipelines that want lineWidth > 1.0 must check this and fall back to 1.0.
    bool WideLinesSupported() const { return m_wideLinesSupported; }

private:
    vkb::PhysicalDevice m_physical{};
    vkb::Device m_device{};
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue  = VK_NULL_HANDLE;
    uint32_t m_graphicsFamily = 0;
    uint32_t m_presentFamily  = 0;
    bool m_wideLinesSupported = false;
    bool m_initialized = false;
};

}  // namespace pokemotor::vk
