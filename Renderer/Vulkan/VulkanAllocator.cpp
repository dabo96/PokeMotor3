// La implementación de VMA vive en esta única unidad de traducción; no incluyas
// VMA con VMA_IMPLEMENTATION en ningún otro sitio o el linker se quejará.
#define VMA_IMPLEMENTATION
#include "VulkanAllocator.h"

#include "VulkanDevice.h"
#include "VulkanInstance.h"

#include <cstdio>

namespace pk {

bool VulkanAllocator::Initialize(const VulkanInstance& instance, const VulkanDevice& device) {
    VmaAllocatorCreateInfo info{};
    info.physicalDevice   = device.PhysicalHandle();
    info.device           = device.Handle();
    info.instance         = instance.Handle();
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    // Sin BUFFER_DEVICE_ADDRESS por ahora (no se usa hasta fases posteriores),
    // así no exigimos esa feature al device.

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

}  // namespace pk
