#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstddef>

namespace pokemotor::vk {

// Thin RAII-ish wrapper. Caller is responsible for invoking Destroy() before
// the allocator is torn down. Move-only.
class BufferVk {
public:
    BufferVk() = default;
    ~BufferVk();
    BufferVk(const BufferVk&) = delete;
    BufferVk& operator=(const BufferVk&) = delete;
    BufferVk(BufferVk&& other) noexcept;
    BufferVk& operator=(BufferVk&& other) noexcept;

    // Creates a HOST-visible buffer suitable as a CPU→GPU staging source.
    // Auto-mapped — use Mapped() to write.
    bool CreateStaging(VmaAllocator allocator, VkDeviceSize size,
                       VkBufferUsageFlags extraUsage = 0);

    // Creates a DEVICE-local buffer. usage must include the eventual binding
    // type (VERTEX_BUFFER_BIT / INDEX_BUFFER_BIT / UNIFORM_BUFFER_BIT / ...).
    // TRANSFER_DST_BIT is always added.
    bool CreateDeviceLocal(VmaAllocator allocator, VkDeviceSize size,
                           VkBufferUsageFlags usage);

    // Creates a HOST-coherent persistently-mapped buffer (typical for per-frame
    // UBOs). usage typically VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT.
    bool CreateHostCoherent(VmaAllocator allocator, VkDeviceSize size,
                            VkBufferUsageFlags usage);

    // Device-local storage buffer (SSBO). TRANSFER_DST + STORAGE_BUFFER are
    // added automatically; pass extraUsage for INDIRECT_BUFFER_BIT etc.
    bool CreateStorage(VmaAllocator allocator, VkDeviceSize size,
                       VkBufferUsageFlags extraUsage = 0);

    // Records a vkCmdFillBuffer that clears the whole buffer to a uint pattern.
    // Caller manages barriers around it. Typical use: reset ray-count / bin
    // counters to 0 at the start of each frame's SSR sort.
    void CmdFill(VkCommandBuffer cmd, uint32_t pattern,
                 VkDeviceSize size = VK_WHOLE_SIZE, VkDeviceSize offset = 0) const;

    void Destroy();

    VkBuffer Handle() const { return m_buffer; }
    VkDeviceSize Size() const { return m_size; }
    void* Mapped() const { return m_mapped; }

    // Records a buffer→buffer copy. Caller manages barriers around it.
    static void CmdCopy(VkCommandBuffer cmd, const BufferVk& src, const BufferVk& dst,
                        VkDeviceSize size, VkDeviceSize srcOffset = 0,
                        VkDeviceSize dstOffset = 0);

private:
    VmaAllocator   m_allocator  = VK_NULL_HANDLE;
    VkBuffer       m_buffer     = VK_NULL_HANDLE;
    VmaAllocation  m_allocation = VK_NULL_HANDLE;
    VkDeviceSize   m_size       = 0;
    void*          m_mapped     = nullptr;

    bool create(VmaAllocator allocator, VkDeviceSize size,
                VkBufferUsageFlags usage, VmaAllocationCreateFlags vmaFlags,
                VmaMemoryUsage memUsage);
};

}  // namespace pokemotor::vk
