#include "VulkanDevice.h"

#include "VulkanInstance.h"

#include <cstdio>

namespace pokemotor::vk {

bool VulkanDevice::Initialize(const VulkanInstance& instance, VkSurfaceKHR surface) {
    VkPhysicalDeviceVulkan13Features feat13{};
    feat13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    feat13.dynamicRendering = VK_TRUE;
    feat13.synchronization2 = VK_TRUE;

    // 1.2 features used by FASE 5 bindless: sampler2D[] in set=1 with partial
    // binding + update-after-bind so we can keep adding textures at runtime.
    VkPhysicalDeviceVulkan12Features feat12{};
    feat12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    feat12.bufferDeviceAddress                          = VK_TRUE;
    feat12.descriptorIndexing                           = VK_TRUE;
    feat12.runtimeDescriptorArray                       = VK_TRUE;
    feat12.descriptorBindingPartiallyBound              = VK_TRUE;
    feat12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    feat12.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;

    vkb::PhysicalDeviceSelector selector{instance.Bootstrap()};
    auto picked = selector
        .set_minimum_version(1, 3)
        .set_required_features_13(feat13)
        .set_required_features_12(feat12)
        .set_surface(surface)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
        .select();
    if (!picked) {
        std::fprintf(stderr, "[Vulkan] Physical device select failed: %s\n",
                     picked.error().message().c_str());
        return false;
    }
    m_physical = picked.value();

    // wideLines lets the debug-line / gizmo pipeline draw axes with a lineWidth
    // > 1.0 (thicker, easier to grab). It's near-universal on desktop discrete
    // GPUs but OPTIONAL in Vulkan, so we don't require it for device selection —
    // a GPU without it should still boot. Enable it only if present and record
    // the result; the gizmo pipeline must fall back to lineWidth 1.0 otherwise,
    // since setting a static width > 1.0 without the feature is a spec violation.
    VkPhysicalDeviceFeatures feat10{};
    feat10.wideLines = VK_TRUE;
    m_wideLinesSupported = m_physical.enable_features_if_present(feat10);

    vkb::DeviceBuilder devBuilder{m_physical};
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
    m_device = {};
    m_physical = {};
    m_initialized = false;
}

}  // namespace pokemotor::vk
