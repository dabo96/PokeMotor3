// Renderer/Vulkan/VulkanSwapchain.h — swapchain vía vk-bootstrap.
// Diseño: Fase 1. FIFO (vsync). Se recrea ante resize / out-of-date.
#pragma once

#include <VkBootstrap.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace pk {

class VulkanDevice;

class VulkanSwapchain {
public:
    bool Initialize(const VulkanDevice& device, VkSurfaceKHR surface,
                    uint32_t width, uint32_t height);
    void Shutdown(const VulkanDevice& device);

    VkSwapchainKHR Handle() const { return m_swapchain.swapchain; }
    VkFormat       ImageFormat() const { return m_swapchain.image_format; }
    VkExtent2D     Extent() const { return m_swapchain.extent; }
    uint32_t       ImageCount() const { return static_cast<uint32_t>(m_images.size()); }
    VkImage        Image(uint32_t i) const { return m_images[i]; }
    VkImageView    ImageView(uint32_t i) const { return m_imageViews[i]; }

private:
    vkb::Swapchain           m_swapchain{};
    std::vector<VkImage>     m_images;
    std::vector<VkImageView> m_imageViews;
    bool                     m_initialized = false;
};

}  // namespace pk
