#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace pokemotor::vk {

struct ImageCreateParams {
    uint32_t              width  = 1;
    uint32_t              height = 1;
    VkFormat              format = VK_FORMAT_R8G8B8A8_SRGB;
    VkImageUsageFlags     usage  = 0;
    VkImageAspectFlags    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t              mipLevels   = 1;
    uint32_t              arrayLayers = 1;  // >1 makes the main view a 2D_ARRAY view
};

class ImageVk {
public:
    ImageVk() = default;
    ~ImageVk();
    ImageVk(const ImageVk&) = delete;
    ImageVk& operator=(const ImageVk&) = delete;
    ImageVk(ImageVk&& other) noexcept;
    ImageVk& operator=(ImageVk&& other) noexcept;

    bool Create(VkDevice device, VmaAllocator allocator, const ImageCreateParams& p);
    void Destroy();

    VkImage     Handle()    const { return m_image;  }
    VkImageView View()      const { return m_view;   }
    VkFormat    Format()    const { return m_format; }
    uint32_t    Width()     const { return m_width;  }
    uint32_t    Height()    const { return m_height; }

    // Records a sync2 layout transition on this image's full subresource range.
    void CmdTransition(VkCommandBuffer cmd,
                       VkImageLayout oldLayout, VkImageLayout newLayout,
                       VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) const;

    // Records a buffer→image copy for the full mip 0 layer 0.
    void CmdCopyFromBuffer(VkCommandBuffer cmd, VkBuffer src) const;

private:
    VkDevice            m_device     = VK_NULL_HANDLE;
    VmaAllocator        m_allocator  = VK_NULL_HANDLE;
    VkImage             m_image      = VK_NULL_HANDLE;
    VmaAllocation       m_allocation = VK_NULL_HANDLE;
    VkImageView         m_view       = VK_NULL_HANDLE;
    VkFormat            m_format     = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags  m_aspect     = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t            m_width        = 0;
    uint32_t            m_height       = 0;
    uint32_t            m_mipLevels    = 1;
    uint32_t            m_arrayLayers  = 1;
};

}  // namespace pokemotor::vk
