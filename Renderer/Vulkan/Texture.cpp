#include "Renderer/Vulkan/Texture.h"

#include "Core/Log.h"
#include "Renderer/Vulkan/BufferVk.h"
#include "Renderer/Vulkan/VulkanContext.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pk {

bool Texture::loadFromFile(VulkanContext& ctx, const char* path, FilterMode filter, bool srgb) {
    m_filter = filter;
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path, &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels) {
        LOG_ERROR("Texture: no se pudo cargar '%s' (%s)", path, stbi_failure_reason());
        return false;
    }
    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;

    // Tratamiento HD (premultiplicado + mips) SOLO para color suave: filter==Smooth Y
    // srgb (color). Las texturas de DATOS (srgb=false: atlas MSDF, normal maps) nunca se
    // premultiplican ni mipean aunque entren con filter Smooth: corromperían los datos y
    // el MSDF se muestrea con su propio sampler lineal aparte.
    const bool hd = (filter == FilterMode::Smooth) && srgb;

    // Alpha premultiplicado: va emparejado con el blend premultiplicado del pipeline
    // sprite2d (evita los halos oscuros que el filtrado lineal saca en los bordes
    // anti-aliados). Las Pixel / datos se quedan con alpha straight.
    if (hd) {
        const size_t count = static_cast<size_t>(w) * h;
        for (size_t i = 0; i < count; ++i) {
            stbi_uc* px = pixels + i * 4;
            const unsigned a = px[3];   // alpha sin tocar; r/g/b *= a/255
            px[0] = static_cast<stbi_uc>(px[0] * a / 255);
            px[1] = static_cast<stbi_uc>(px[1] * a / 255);
            px[2] = static_cast<stbi_uc>(px[2] * a / 255);
        }
    }

    BufferVk staging;
    if (!staging.CreateStaging(ctx.allocator(), size)) {
        stbi_image_free(pixels);
        return false;
    }
    std::memcpy(staging.Mapped(), pixels, static_cast<size_t>(size));
    stbi_image_free(pixels);

    const VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

    // Mipmaps solo para Smooth (y si el formato soporta blit lineal: lo necesita el
    // vkCmdBlitImage con VK_FILTER_LINEAR). Si no, caemos a 1 nivel y avisamos.
    uint32_t mipLevels = 1;
    if (hd) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice(), fmt, &fp);
        const bool canBlitLinear =
            (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0 &&
            (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0 &&
            (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
        if (canBlitLinear) {
            const uint32_t maxDim = static_cast<uint32_t>(std::max(w, h));
            mipLevels = static_cast<uint32_t>(std::floor(std::log2(maxDim))) + 1;
        } else {
            LOG_WARN("Texture: '%s' sin mipmaps (formato no soporta blit lineal)", path);
        }
    }

    ImageCreateParams p;
    p.width     = static_cast<uint32_t>(w);
    p.height    = static_cast<uint32_t>(h);
    p.format    = fmt;
    p.mipLevels = mipLevels;
    p.usage     = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mipLevels > 1) p.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // blit i-1 → i
    if (!m_image.Create(ctx.device(), ctx.allocator(), p)) {
        staging.Destroy();
        return false;
    }

    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        m_image.CmdTransition(cmd,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            // ALL_TRANSFER: tras esto vienen un copy (mip 0) y blits (mips 1..n); ambas
            // etapas deben quedar sincronizadas con la transición de layout (sync2).
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        m_image.CmdCopyFromBuffer(cmd, staging.Handle());
        // Genera la cadena de mips (o, si mipLevels==1, solo transiciona el mip 0) y
        // deja TODOS los niveles en SHADER_READ_ONLY_OPTIMAL.
        m_image.CmdGenerateMipmaps(cmd);
    });

    staging.Destroy();  // immediateSubmit ya esperó a que el copy/blits terminen
    LOG_INFO("Texture cargada: %s (%dx%d, %u mips)", path, w, h, mipLevels);
    return true;
}

bool Texture::createSolid(VulkanContext& ctx, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t px[4] = { r, g, b, a };

    BufferVk staging;
    if (!staging.CreateStaging(ctx.allocator(), 4)) return false;
    std::memcpy(staging.Mapped(), px, 4);

    ImageCreateParams p;
    p.width  = 1;
    p.height = 1;
    p.format = VK_FORMAT_R8G8B8A8_SRGB;
    p.usage  = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!m_image.Create(ctx.device(), ctx.allocator(), p)) { staging.Destroy(); return false; }

    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        m_image.CmdTransition(cmd,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        m_image.CmdCopyFromBuffer(cmd, staging.Handle());
        m_image.CmdTransition(cmd,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    staging.Destroy();
    return true;
}

}  // namespace pk
