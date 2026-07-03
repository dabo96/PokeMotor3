// Renderer/Vulkan/BufferVk.h — wrapper RAII fino sobre un VkBuffer + VMA.
// Adaptado del backend del motor anterior (probado). Move-only; el caller debe
// llamar Destroy() antes de destruir el allocator.
#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstddef>

namespace pk {

class BufferVk {
public:
    BufferVk() = default;
    ~BufferVk();
    BufferVk(const BufferVk&) = delete;
    BufferVk& operator=(const BufferVk&) = delete;
    BufferVk(BufferVk&& other) noexcept;
    BufferVk& operator=(BufferVk&& other) noexcept;

    // Buffer HOST-visible como origen de staging CPU→GPU. Auto-mapeado (Mapped()).
    bool CreateStaging(VmaAllocator allocator, VkDeviceSize size,
                       VkBufferUsageFlags extraUsage = 0);

    // Buffer DEVICE-local. usage debe incluir el tipo final (VERTEX/INDEX/...).
    // TRANSFER_DST_BIT se añade siempre.
    bool CreateDeviceLocal(VmaAllocator allocator, VkDeviceSize size,
                           VkBufferUsageFlags usage);

    // Buffer HOST-coherente persistentemente mapeado (típico para UBOs por frame).
    bool CreateHostCoherent(VmaAllocator allocator, VkDeviceSize size,
                            VkBufferUsageFlags usage);

    void Destroy();

    VkBuffer     Handle() const { return m_buffer; }
    VkDeviceSize Size() const { return m_size; }
    void*        Mapped() const { return m_mapped; }

    static void CmdCopy(VkCommandBuffer cmd, const BufferVk& src, const BufferVk& dst,
                        VkDeviceSize size, VkDeviceSize srcOffset = 0,
                        VkDeviceSize dstOffset = 0);

private:
    VmaAllocator  m_allocator  = VK_NULL_HANDLE;
    VkBuffer      m_buffer     = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VkDeviceSize  m_size       = 0;
    void*         m_mapped     = nullptr;

    bool create(VmaAllocator allocator, VkDeviceSize size,
                VkBufferUsageFlags usage, VmaAllocationCreateFlags vmaFlags,
                VmaMemoryUsage memUsage);
};

}  // namespace pk
