#include "BufferVk.h"

#include <cstdio>

namespace pk {

BufferVk::~BufferVk() { Destroy(); }

BufferVk::BufferVk(BufferVk&& other) noexcept
    : m_allocator(other.m_allocator), m_buffer(other.m_buffer),
      m_allocation(other.m_allocation), m_size(other.m_size), m_mapped(other.m_mapped) {
    other.m_allocator = VK_NULL_HANDLE; other.m_buffer = VK_NULL_HANDLE;
    other.m_allocation = VK_NULL_HANDLE; other.m_size = 0; other.m_mapped = nullptr;
}

BufferVk& BufferVk::operator=(BufferVk&& other) noexcept {
    if (this != &other) {
        Destroy();
        m_allocator = other.m_allocator; m_buffer = other.m_buffer;
        m_allocation = other.m_allocation; m_size = other.m_size; m_mapped = other.m_mapped;
        other.m_allocator = VK_NULL_HANDLE; other.m_buffer = VK_NULL_HANDLE;
        other.m_allocation = VK_NULL_HANDLE; other.m_size = 0; other.m_mapped = nullptr;
    }
    return *this;
}

bool BufferVk::create(VmaAllocator allocator, VkDeviceSize size,
                      VkBufferUsageFlags usage, VmaAllocationCreateFlags vmaFlags,
                      VmaMemoryUsage memUsage) {
    Destroy();

    VkBufferCreateInfo bufInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufInfo.size        = size;
    bufInfo.usage       = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = memUsage;
    allocInfo.flags = vmaFlags;

    VmaAllocationInfo result{};
    if (vmaCreateBuffer(allocator, &bufInfo, &allocInfo,
                        &m_buffer, &m_allocation, &result) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] vmaCreateBuffer falló (size=%llu)\n",
                     static_cast<unsigned long long>(size));
        return false;
    }
    m_allocator = allocator;
    m_size      = size;
    m_mapped    = result.pMappedData;
    return true;
}

bool BufferVk::CreateStaging(VmaAllocator allocator, VkDeviceSize size,
                             VkBufferUsageFlags extraUsage) {
    return create(allocator, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | extraUsage,
                  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT,
                  VMA_MEMORY_USAGE_AUTO);
}

bool BufferVk::CreateDeviceLocal(VmaAllocator allocator, VkDeviceSize size,
                                 VkBufferUsageFlags usage) {
    return create(allocator, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  0, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
}

bool BufferVk::CreateHostCoherent(VmaAllocator allocator, VkDeviceSize size,
                                  VkBufferUsageFlags usage) {
    return create(allocator, size, usage,
                  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT,
                  VMA_MEMORY_USAGE_AUTO);
}

void BufferVk::Destroy() {
    if (m_buffer && m_allocator) vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
    m_buffer = VK_NULL_HANDLE; m_allocation = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE; m_size = 0; m_mapped = nullptr;
}

void BufferVk::CmdCopy(VkCommandBuffer cmd, const BufferVk& src, const BufferVk& dst,
                       VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset) {
    VkBufferCopy region{};
    region.srcOffset = srcOffset;
    region.dstOffset = dstOffset;
    region.size      = size;
    vkCmdCopyBuffer(cmd, src.Handle(), dst.Handle(), 1, &region);
}

}  // namespace pk
