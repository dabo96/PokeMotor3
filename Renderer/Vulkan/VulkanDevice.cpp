#include "VulkanDevice.h"

#include "VulkanInstance.h"

#include <cstdio>

namespace pk {

bool VulkanDevice::Initialize(const VulkanInstance& instance, VkSurfaceKHR surface) {
    VkPhysicalDeviceVulkan13Features feat13{};
    feat13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    feat13.dynamicRendering = VK_TRUE;
    feat13.synchronization2 = VK_TRUE;

    vkb::PhysicalDeviceSelector selector{ instance.Bootstrap() };
    auto picked = selector
        .set_minimum_version(1, 3)
        .set_required_features_13(feat13)
        .set_surface(surface)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
        .select();
    if (!picked) {
        std::fprintf(stderr, "[Vulkan] Physical device select failed: %s\n",
                     picked.error().message().c_str());
        return false;
    }
    m_physical = picked.value();

    vkb::DeviceBuilder devBuilder{ m_physical };
    auto dev = devBuilder.build();
    if (!dev) {
        std::fprintf(stderr, "[Vulkan] Logical device build failed: %s\n",
                     dev.error().message().c_str());
        return false;
    }
    m_device = dev.value();

    auto gq = m_device.get_queue(vkb::QueueType::graphics);
    auto pq = m_device.get_queue(vkb::QueueType::present);
    auto gi = m_device.get_queue_index(vkb::QueueType::graphics);
    auto pi = m_device.get_queue_index(vkb::QueueType::present);
    if (!gq || !pq || !gi || !pi) {
        std::fprintf(stderr, "[Vulkan] Failed to retrieve queues\n");
        return false;
    }
    m_graphicsQueue  = gq.value();
    m_presentQueue   = pq.value();
    m_graphicsFamily = gi.value();
    m_presentFamily  = pi.value();

    m_initialized = true;
    return true;
}

void VulkanDevice::Shutdown() {
    if (!m_initialized) return;
    vkb::destroy_device(m_device);
    m_device   = {};
    m_physical = {};
    m_initialized = false;
}

}  // namespace pk
