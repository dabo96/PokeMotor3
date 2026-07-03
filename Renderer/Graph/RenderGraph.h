// Renderer/Graph/RenderGraph.h — render graph mínimo (Fase 2).
// Diseño: MotorGrafico_RenderGraph.md. Declarar passes (qué leen/escriben) →
// compile() deduce el orden por dependencias (orden topológico) e inserta las
// barreras/transiciones de layout automáticamente → execute() graba.
//
// Alcance Fase 2 (como pide el índice): orden + barreras. SIN culling, SIN
// aliasing, SIN recursos transitorios todavía (llegan en fases posteriores).
// Por ahora solo rastrea imágenes; los buffers se añadirán cuando hagan falta.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pk {

using GraphResource = uint32_t;
constexpr GraphResource kInvalidResource = static_cast<GraphResource>(-1);

// Uso declarado de un recurso-imagen por un pass.
struct ImageAccess {
    GraphResource         resource = kInvalidResource;
    VkImageLayout         layout   = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage    = 0;
    VkAccessFlags2        access   = 0;
};

class RenderGraph;

// Fase de declaración: el setup de cada pass declara qué lee/escribe.
class RenderGraphBuilder {
public:
    void read(GraphResource r, VkImageLayout layout,
              VkPipelineStageFlags2 stage, VkAccessFlags2 access);
    void write(GraphResource r, VkImageLayout layout,
               VkPipelineStageFlags2 stage, VkAccessFlags2 access);

    // Atajos comunes.
    void writeColorAttachment(GraphResource r);   // COLOR_ATTACHMENT_OPTIMAL
    void writeDepthAttachment(GraphResource r);   // DEPTH_ATTACHMENT_OPTIMAL
    void readSampled(GraphResource r);            // SHADER_READ_ONLY_OPTIMAL (frag)

private:
    friend class RenderGraph;
    std::vector<ImageAccess> m_reads;
    std::vector<ImageAccess> m_writes;
};

// Fase de ejecución: lo que recibe el callback de cada pass.
struct RenderPassContext {
    VkCommandBuffer cmd   = VK_NULL_HANDLE;
    RenderGraph*    graph = nullptr;

    VkImageView view(GraphResource r) const;
    VkImage     image(GraphResource r) const;
    VkExtent2D  extent(GraphResource r) const;
};

class RenderGraph {
public:
    using ExecuteFn = std::function<void(RenderPassContext&)>;
    using SetupFn   = std::function<ExecuteFn(RenderGraphBuilder&)>;

    // Reconstruir cada frame (recomendado por el diseño): reset → declarar →
    // compile → execute.
    void reset();

    // Importa una imagen externa (p.ej. la del swapchain). initialStage/Access
    // describen en qué estado llega (para el swapchain, la etapa en que se espera
    // el semáforo del acquire). finalLayout es a la que el grafo la deja al final
    // (p.ej. PRESENT_SRC).
    GraphResource importImage(const char* name, VkImage image, VkImageView view,
                              VkExtent2D extent, VkImageAspectFlags aspect,
                              VkImageLayout initialLayout,
                              VkPipelineStageFlags2 initialStage,
                              VkAccessFlags2 initialAccess,
                              VkImageLayout finalLayout);

    // El setup declara dependencias y RETORNA el callback execute (diseño).
    void addPass(const char* name, SetupFn setup);

    void compile();                      // orden topológico por dependencias
    void execute(VkCommandBuffer cmd);   // barreras automáticas + ejecuta passes

    // Usados por RenderPassContext.
    VkImageView viewOf(GraphResource r) const;
    VkImage     imageOf(GraphResource r) const;
    VkExtent2D  extentOf(GraphResource r) const;

private:
    struct Resource {
        std::string           name;
        VkImage               image  = VK_NULL_HANDLE;
        VkImageView           view   = VK_NULL_HANDLE;
        VkExtent2D            extent = { 0, 0 };
        VkImageAspectFlags    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        // Estado actual rastreado.
        VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkPipelineStageFlags2 stage  = 0;
        VkAccessFlags2        access = 0;
        bool                  lastWasWrite = false;
        // Estado final deseado (imágenes importadas).
        VkImageLayout         finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool                  hasFinal = false;
    };
    struct Pass {
        std::string              name;
        std::vector<ImageAccess> reads;
        std::vector<ImageAccess> writes;
        ExecuteFn                execute;
    };

    void barrier(VkCommandBuffer cmd, Resource& res,
                 VkImageLayout layout, VkPipelineStageFlags2 stage,
                 VkAccessFlags2 access, bool isWrite);

    std::vector<Resource> m_resources;
    std::vector<Pass>     m_passes;
    std::vector<int>      m_order;
};

}  // namespace pk
