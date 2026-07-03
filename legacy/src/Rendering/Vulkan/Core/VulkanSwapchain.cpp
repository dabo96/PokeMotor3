#include "VulkanSwapchain.h"

#include "VulkanDevice.h"

#include <cstdio>

namespace pokemotor::vk {

bool VulkanSwapchain::Initialize(const VulkanDevice& device, VkSurfaceKHR surface,
                                 uint32_t width, uint32_t height) {
    vkb::SwapchainBuilder builder{device.Bootstrap(), surface};
    auto built = builder
        .use_default_format_selection()
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        .set_desired_extent(width, height)
        .add_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                               | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        .build();
    if (!built) {
        std::fprintf(stderr, "[Vulkan] Swapchain build failed: %s\n",
                     built.error().message().c_str());
        return false;
    }
    m_swapchain = built.value();

    auto images = m_swapchain.get_images();
    auto views  = m_swapchain.get_image_views();
    if (!images || !views) {
        std::fprintf(stderr, "[Vulkan] Swapchain images/views retrieval failed\n");
        return false;
    }
    m_images     = images.value();
    m_imageViews = views.value();
    m_initialized = true;
    return true;
}

void VulkanSwapchain::Shutdown(const VulkanDevice& /*device*/) {
    if (!m_initialized) return;
    m_swapchain.destroy_image_views(m_imageViews);
    vkb::destroy_swapchain(m_swapchain);
    m_imageViews.clear();
    m_images.clear();
    m_swapchain = {};
    m_initialized = false;
}

}  // namespace pokemotor::vk
