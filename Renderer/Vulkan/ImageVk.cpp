#include "ImageVk.h"

#include <cstdio>

namespace pk {

ImageVk::~ImageVk() { Destroy(); }

ImageVk::ImageVk(ImageVk&& o) noexcept
    : m_device(o.m_device), m_allocator(o.m_allocator), m_image(o.m_image),
      m_allocation(o.m_allocation), m_view(o.m_view), m_format(o.m_format),
      m_aspect(o.m_aspect), m_width(o.m_width), m_height(o.m_height),
      m_mipLevels(o.m_mipLevels), m_arrayLayers(o.m_arrayLayers) {
    o.m_device = VK_NULL_HANDLE; o.m_allocator = VK_NULL_HANDLE; o.m_image = VK_NULL_HANDLE;
    o.m_allocation = VK_NULL_HANDLE; o.m_view = VK_NULL_HANDLE;
}

ImageVk& ImageVk::operator=(ImageVk&& o) noexcept {
    if (this != &o) {
        Destroy();
        m_device = o.m_device; m_allocator = o.m_allocator; m_image = o.m_image;
        m_allocation = o.m_allocation; m_view = o.m_view; m_format = o.m_format;
        m_aspect = o.m_aspect; m_width = o.m_width; m_height = o.m_height;
        m_mipLevels = o.m_mipLevels; m_arrayLayers = o.m_arrayLayers;
        o.m_device = VK_NULL_HANDLE; o.m_allocator = VK_NULL_HANDLE; o.m_image = VK_NULL_HANDLE;
        o.m_allocation = VK_NULL_HANDLE; o.m_view = VK_NULL_HANDLE;
    }
    return *this;
}

bool ImageVk::Create(VkDevice device, VmaAllocator allocator, const ImageCreateParams& p) {
    Destroy();

    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
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

    if (vmaCreateImage(allocator, &info, &allocInfo, &m_image, &m_allocation, nullptr) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] vmaCreateImage falló (%ux%u)\n", p.width, p.height);
        return false;
    }

    VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image    = m_image;
    viewInfo.viewType = (p.arrayLayers > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format   = p.format;
    viewInfo.subresourceRange.aspectMask     = p.aspect;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = p.mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = p.arrayLayers;
    if (vkCreateImageView(device, &viewInfo, nullptr, &m_view) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] vkCreateImageView falló\n");
        vmaDestroyImage(allocator, m_image, m_allocation);
        m_image = VK_NULL_HANDLE; m_allocation = VK_NULL_HANDLE;
        return false;
    }

    m_device = device; m_allocator = allocator; m_format = p.format; m_aspect = p.aspect;
    m_width = p.width; m_height = p.height; m_mipLevels = p.mipLevels; m_arrayLayers = p.arrayLayers;
    return true;
}

void ImageVk::Destroy() {
    if (m_view && m_device) vkDestroyImageView(m_device, m_view, nullptr);
    if (m_image && m_allocator) vmaDestroyImage(m_allocator, m_image, m_allocation);
    m_view = VK_NULL_HANDLE; m_image = VK_NULL_HANDLE; m_allocation = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE; m_allocator = VK_NULL_HANDLE;
}

void ImageVk::CmdTransition(VkCommandBuffer cmd,
                            VkImageLayout oldLayout, VkImageLayout newLayout,
                            VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) const {
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout; b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = m_image;
    b.subresourceRange = { m_aspect, 0, m_mipLevels, 0, m_arrayLayers };

    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void ImageVk::CmdCopyFromBuffer(VkCommandBuffer cmd, VkBuffer src) const {
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask     = m_aspect;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageExtent = { m_width, m_height, 1 };
    vkCmdCopyBufferToImage(cmd, src, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void ImageVk::CmdGenerateMipmaps(VkCommandBuffer cmd) const {
    // Pre: el mip 0 está en TRANSFER_DST_OPTIMAL (recién copiado del staging).
    // Para cada nivel i>0: pasamos i-1 a TRANSFER_SRC, blit lineal i-1→i (mitad de
    // tamaño, clamp a 1), y dejamos i-1 en SHADER_READ_ONLY. El último nivel se
    // transiciona aparte al final.
    auto barrier = [&](uint32_t mip, VkImageLayout oldL, VkImageLayout newL,
                       VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_image;
        b.subresourceRange = { m_aspect, mip, 1, 0, m_arrayLayers };
        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    };

    if (m_mipLevels <= 1) {
        // Sin cadena de mips: solo el mip 0 (TRANSFER_DST → SHADER_READ_ONLY).
        barrier(0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        return;
    }

    int32_t mw = static_cast<int32_t>(m_width);
    int32_t mh = static_cast<int32_t>(m_height);
    for (uint32_t i = 1; i < m_mipLevels; ++i) {
        // El nivel origen (i-1) pasa a TRANSFER_SRC para poder leerlo en el blit. El
        // origen lo escribió el copy (i==1) o el blit anterior (i>1); ALL_TRANSFER cubre
        // ambas etapas (COPY y BLIT son distintas en sync2 y deben sincronizarse bien).
        barrier(i - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

        const int32_t dw = mw > 1 ? mw / 2 : 1;
        const int32_t dh = mh > 1 ? mh / 2 : 1;
        VkImageBlit blit{};
        blit.srcOffsets[1] = { mw, mh, 1 };
        blit.srcSubresource = { m_aspect, i - 1, 0, m_arrayLayers };
        blit.dstOffsets[1] = { dw, dh, 1 };
        blit.dstSubresource = { m_aspect, i, 0, m_arrayLayers };
        vkCmdBlitImage(cmd,
            m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        // El nivel origen (i-1), ya consumido, queda listo para muestrear.
        barrier(i - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        mw = dw; mh = dh;
    }
    // El último nivel sigue en TRANSFER_DST (fue destino del último blit): a SHADER_READ.
    barrier(m_mipLevels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

}  // namespace pk
