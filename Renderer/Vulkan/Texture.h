// Renderer/Vulkan/Texture.h — textura 2D cargada de archivo (PNG vía stb_image).
// Diseño: MotorGrafico_IndiceMaestro.md (Fase 3: carga de texturas imagen→VkImage
// vía VMA). El sampler es aparte (lo comparte el material/renderer).
#pragma once

#include "Renderer/Vulkan/ImageVk.h"

namespace pk {

class VulkanContext;

class Texture {
public:
    // Carga RGBA8. Sube vía staging + immediateSubmit y deja la imagen en
    // SHADER_READ_ONLY_OPTIMAL. Devuelve false si el archivo no se pudo leer.
    // srgb=true para color (aplica gamma al muestrear); srgb=false para texturas de
    // DATOS (MSDF, normal maps): los canales se leen lineales, sin conversión.
    // filter=Smooth (defecto): genera mipmaps y premultiplica el alpha (arte HD, va
    // con el sampler lineal + blend premultiplicado). filter=Pixel: sin mips, sin
    // premultiplicar (pixel-art / tiles, sampler nearest).
    bool loadFromFile(VulkanContext& ctx, const char* path,
                      FilterMode filter = FilterMode::Smooth, bool srgb = true);
    // Crea una textura 1×1 de color sólido (p.ej. blanco para materiales sin textura).
    bool createSolid(VulkanContext& ctx, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void destroy() { m_image.Destroy(); }

    VkImageView view()      const { return m_image.View(); }
    uint32_t    width()     const { return m_image.Width(); }
    uint32_t    height()    const { return m_image.Height(); }
    uint32_t    mipLevels() const { return m_image.MipLevels(); }
    FilterMode  filter()    const { return m_filter; }
    bool        valid()     const { return m_image.View() != VK_NULL_HANDLE; }

private:
    ImageVk    m_image;
    FilterMode m_filter = FilterMode::Smooth;
};

}  // namespace pk
