// VMA implementation lives in this translation unit; do NOT include VMA
// elsewhere with VMA_IMPLEMENTATION defined or the linker will complain.
#define VMA_IMPLEMENTATION
#include "VulkanAllocator.h"

#include "VulkanDevice.h"
#include "VulkanInstance.h"

#include <cstdio>

namespace pokemotor::vk {

bool VulkanAllocator::Initialize(const VulkanInstance& instance, const VulkanDevice& device) {
    VmaAllocatorCreateInfo info{};
    info.flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    info.physicalDevice   = device.PhysicalHandle();
    info.device           = device.Handle();
    info.instance         = instance.Handle();
    info.vulkanApiVersion = VK_API_VERSION_1_3;

    if (vmaCreateAllocator(&info, &m_allocator) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] vmaCreateAllocator failed\n");
        m_allocator = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void VulkanAllocator::Shutdown() {
    if (m_allocator) {
        vmaDestroyAllocator(m_allocator);
        m_allocator = VK_NULL_HANDLE;
    }
}

}  // namespace pokemotor::vk
