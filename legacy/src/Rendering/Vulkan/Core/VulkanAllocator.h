#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace pokemotor::vk {

class VulkanInstance;
class VulkanDevice;

class VulkanAllocator {
public:
    bool Initialize(const VulkanInstance& instance, const VulkanDevice& device);
    void Shutdown();

    VmaAllocator Handle() const { return m_allocator; }

private:
    VmaAllocator m_allocator = VK_NULL_HANDLE;
};

}  // namespace pokemotor::vk
