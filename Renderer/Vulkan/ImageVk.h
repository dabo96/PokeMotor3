// Renderer/Vulkan/ImageVk.h — wrapper de VkImage + VkImageView + VMA.
// Adaptado del backend anterior (probado). Move-only.
#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace pk {

// Cómo se muestrea una textura (por asset, no global): Pixel = nearest sin mips
// (pixel-art / tiles, look crujiente); Smooth = lineal + mipmaps (arte HD detallado,
// p.ej. sprites de Pokémon). Lo elige quien carga la textura (ver AssetManager).
enum class FilterMode { Pixel, Smooth };

struct ImageCreateParams {
    uint32_t              width   = 1;
    uint32_t              height  = 1;
    VkFormat              format  = VK_FORMAT_R8G8B8A8_SRGB;
    VkImageUsageFlags     usage   = 0;
    VkImageAspectFlags    aspect  = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t              mipLevels   = 1;
    uint32_t              arrayLayers = 1;
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

    VkImage     Handle()    const { return m_image;     }
    VkImageView View()      const { return m_view;      }
    VkFormat    Format()    const { return m_format;    }
    uint32_t    Width()     const { return m_width;     }
    uint32_t    Height()    const { return m_height;    }
    uint32_t    MipLevels() const { return m_mipLevels; }

    void CmdTransition(VkCommandBuffer cmd,
                       VkImageLayout oldLayout, VkImageLayout newLayout,
                       VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) const;

    void CmdCopyFromBuffer(VkCommandBuffer cmd, VkBuffer src) const;

    // Genera la cadena de mipmaps por blits a partir del mip 0 (que el caller ya
    // subió y dejó en TRANSFER_DST_OPTIMAL). Al terminar, TODOS los niveles quedan
    // en SHADER_READ_ONLY_OPTIMAL. Requiere que la imagen se creara con mipLevels>1 y
    // usage TRANSFER_SRC|TRANSFER_DST, y que el formato soporte blit lineal. No-op si
    // m_mipLevels<=1 (solo hace la transición final del único nivel).
    void CmdGenerateMipmaps(VkCommandBuffer cmd) const;

private:
    VkDevice           m_device      = VK_NULL_HANDLE;
    VmaAllocator       m_allocator   = VK_NULL_HANDLE;
    VkImage            m_image       = VK_NULL_HANDLE;
    VmaAllocation      m_allocation  = VK_NULL_HANDLE;
    VkImageView        m_view        = VK_NULL_HANDLE;
    VkFormat           m_format      = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags m_aspect      = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t           m_width       = 0;
    uint32_t           m_height      = 0;
    uint32_t           m_mipLevels   = 1;
    uint32_t           m_arrayLayers = 1;
};

}  // namespace pk
