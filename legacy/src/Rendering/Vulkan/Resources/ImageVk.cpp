#include "ImageVk.h"

#include <cstdio>

namespace pokemotor::vk {

ImageVk::~ImageVk() {
    Destroy();
}

ImageVk::ImageVk(ImageVk&& other) noexcept
    : m_device(other.m_device)
    , m_allocator(other.m_allocator)
    , m_image(other.m_image)
    , m_allocation(other.m_allocation)
    , m_view(other.m_view)
    , m_format(other.m_format)
    , m_aspect(other.m_aspect)
    , m_width(other.m_width)
    , m_height(other.m_height)
    , m_mipLevels(other.m_mipLevels)
    , m_arrayLayers(other.m_arrayLayers) {
    other.m_device     = VK_NULL_HANDLE;
    other.m_allocator  = VK_NULL_HANDLE;
    other.m_image      = VK_NULL_HANDLE;
    other.m_allocation = VK_NULL_HANDLE;
    other.m_view       = VK_NULL_HANDLE;
}

ImageVk& ImageVk::operator=(ImageVk&& other) noexcept {
    if (this != &other) {
        Destroy();
        m_device      = other.m_device;
        m_allocator   = other.m_allocator;
        m_image       = other.m_image;
        m_allocation  = other.m_allocation;
        m_view        = other.m_view;
        m_format      = other.m_format;
        m_aspect      = other.m_aspect;
        m_width       = other.m_width;
        m_height      = other.m_height;
        m_mipLevels   = other.m_mipLevels;
        m_arrayLayers = other.m_arrayLayers;
        other.m_device     = VK_NULL_HANDLE;
        other.m_allocator  = VK_NULL_HANDLE;
        other.m_image      = VK_NULL_HANDLE;
        other.m_allocation = VK_NULL_HANDLE;
        other.m_view       = VK_NULL_HANDLE;
    }
    return *this;
}

bool ImageVk::Create(VkDevice device, VmaAllocator allocator, const ImageCreateParams& p) {
    Destroy();

    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.format        = p.format;
    info.extent        = { p.width, p.height, 1 };
    info.mipLevels     = p.mipLevels;
    info.arrayLayers   = p.arrayLayers;
    info.samples       = p.samples;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.usage         = p.usage;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocator, &info, &allocInfo,
                       &m_image, &m_allocation, nullptr) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] vmaCreateImage failed (%ux%u format=%d)\n",
                     p.width, p.height, p.format);
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image    = m_image;
    viewInfo.viewType = (p.arrayLayers > 1)
        ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
        : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format   = p.format;
    viewInfo.subresourceRange.aspectMask     = p.aspect;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = p.mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = p.arrayLayers;
    if (vkCreateImageView(device, &viewInfo, nullptr, &m_view) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] vkCreateImageView failed\n");
        vmaDestroyImage(allocator, m_image, m_allocation);
        m_image = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
        return false;
    }

    m_device      = device;
    m_allocator   = allocator;
    m_format      = p.format;
    m_aspect      = p.aspect;
    m_width       = p.width;
    m_height      = p.height;
    m_mipLevels   = p.mipLevels;
    m_arrayLayers = p.arrayLayers;
    return true;
}

void ImageVk::Destroy() {
    if (m_view && m_device) vkDestroyImageView(m_device, m_view, nullptr);
    if (m_image && m_allocator) vmaDestroyImage(m_allocator, m_image, m_allocation);
    m_view = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
    m_allocator = VK_NULL_HANDLE;
}

void ImageVk::CmdTransition(VkCommandBuffer cmd,
                            VkImageLayout oldLayout, VkImageLayout newLayout,
                            VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) const {
    VkImageMemoryBarrier2 b{};
    b.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask  = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask  = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout     = oldLayout;
    b.newLayout     = newLayout;
    b.image         = m_image;
    b.subresourceRange = { m_aspect, 0, m_mipLevels, 0, m_arrayLayers };

    VkDependencyInfo dep{};
    dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount  = 1;
    dep.pImageMemoryBarriers     = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void ImageVk::CmdCopyFromBuffer(VkCommandBuffer cmd, VkBuffer src) const {
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask     = m_aspect;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageExtent = { m_width, m_height, 1 };
    vkCmdCopyBufferToImage(cmd, src, m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

}  // namespace pokemotor::vk
