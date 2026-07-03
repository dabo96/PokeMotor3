#include "Renderer/Graph/RenderGraph.h"

#include <unordered_map>
#include <utility>

namespace pk {

// --- RenderGraphBuilder ---

void RenderGraphBuilder::read(GraphResource r, VkImageLayout layout,
                              VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
    m_reads.push_back({ r, layout, stage, access });
}

void RenderGraphBuilder::write(GraphResource r, VkImageLayout layout,
                               VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
    m_writes.push_back({ r, layout, stage, access });
}

void RenderGraphBuilder::writeColorAttachment(GraphResource r) {
    write(r, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

void RenderGraphBuilder::writeDepthAttachment(GraphResource r) {
    write(r, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
}

void RenderGraphBuilder::readSampled(GraphResource r) {
    read(r, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

// --- RenderPassContext ---

VkImageView RenderPassContext::view(GraphResource r) const { return graph->viewOf(r); }
VkImage     RenderPassContext::image(GraphResource r) const { return graph->imageOf(r); }
VkExtent2D  RenderPassContext::extent(GraphResource r) const { return graph->extentOf(r); }

// --- RenderGraph ---

void RenderGraph::reset() {
    m_resources.clear();
    m_passes.clear();
    m_order.clear();
}

GraphResource RenderGraph::importImage(const char* name, VkImage image, VkImageView view,
                                       VkExtent2D extent, VkImageAspectFlags aspect,
                                       VkImageLayout initialLayout,
                                       VkPipelineStageFlags2 initialStage,
                                       VkAccessFlags2 initialAccess,
                                       VkImageLayout finalLayout) {
    Resource res;
    res.name        = name;
    res.image       = image;
    res.view        = view;
    res.extent      = extent;
    res.aspect      = aspect;
    res.layout      = initialLayout;
    res.stage       = initialStage;
    res.access      = initialAccess;
    res.lastWasWrite = false;
    res.finalLayout = finalLayout;
    // finalLayout == UNDEFINED → la imagen no necesita transición final (p.ej. un
    // depth buffer que se limpia cada frame).
    res.hasFinal    = (finalLayout != VK_IMAGE_LAYOUT_UNDEFINED);
    m_resources.push_back(res);
    return static_cast<GraphResource>(m_resources.size() - 1);
}

void RenderGraph::addPass(const char* name, SetupFn setup) {
    RenderGraphBuilder builder;
    ExecuteFn exec = setup(builder);   // declara reads/writes y devuelve el execute
    Pass pass;
    pass.name    = name;
    pass.reads   = std::move(builder.m_reads);
    pass.writes  = std::move(builder.m_writes);
    pass.execute = std::move(exec);
    m_passes.push_back(std::move(pass));
}

void RenderGraph::compile() {
    const int n = static_cast<int>(m_passes.size());
    std::vector<std::vector<int>> adj(n);
    std::vector<int> indeg(n, 0);

    auto addEdge = [&](int from, int to) {
        if (from == to) return;
        adj[from].push_back(to);
        ++indeg[to];
    };

    // Aristas por hazards de recurso (en orden de inserción):
    //  - lectura depende de escrituras previas (RAW)
    //  - escritura depende de lecturas y escrituras previas (WAR / WAW)
    // Recursos distintos no generan aristas → pueden reordenarse (es un DAG).
    std::unordered_map<GraphResource, std::vector<std::pair<int, bool>>> hist;  // resource -> (pass, isWrite)
    for (int i = 0; i < n; ++i) {
        for (const auto& a : m_passes[i].reads) {
            for (const auto& [pj, isW] : hist[a.resource]) if (isW) addEdge(pj, i);
        }
        for (const auto& a : m_passes[i].writes) {
            for (const auto& [pj, isW] : hist[a.resource]) addEdge(pj, i);
        }
        for (const auto& a : m_passes[i].reads)  hist[a.resource].push_back({ i, false });
        for (const auto& a : m_passes[i].writes) hist[a.resource].push_back({ i, true });
    }

    // Orden topológico (Kahn) con desempate determinista por índice de inserción.
    m_order.clear();
    m_order.reserve(n);
    std::vector<int>  deg(indeg);
    std::vector<bool> done(n, false);
    for (int count = 0; count < n; ++count) {
        int pick = -1;
        for (int i = 0; i < n; ++i)
            if (!done[i] && deg[i] == 0) { pick = i; break; }
        if (pick < 0)  // ciclo inesperado: continúa en orden de inserción
            for (int i = 0; i < n; ++i)
                if (!done[i]) { pick = i; break; }
        done[pick] = true;
        m_order.push_back(pick);
        for (int v : adj[pick])
            if (--deg[v] < 0) deg[v] = 0;
    }
}

void RenderGraph::barrier(VkCommandBuffer cmd, Resource& res,
                          VkImageLayout layout, VkPipelineStageFlags2 stage,
                          VkAccessFlags2 access, bool isWrite) {
    const bool layoutChange = res.layout != layout;
    // Hace falta barrera si: cambia el layout, escribimos (WAR/WAW), o el acceso
    // previo fue escritura (RAW). Lectura-tras-lectura con mismo layout se salta.
    if (!layoutChange && !isWrite && !res.lastWasWrite)
        return;

    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask        = res.stage ? res.stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    b.srcAccessMask       = res.access;
    b.dstStageMask        = stage;
    b.dstAccessMask       = access;
    b.oldLayout           = res.layout;
    b.newLayout           = layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = res.image;
    b.subresourceRange    = { res.aspect, 0, 1, 0, 1 };

    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &dep);

    res.layout       = layout;
    res.stage        = stage;
    res.access       = access;
    res.lastWasWrite = isWrite;
}

void RenderGraph::execute(VkCommandBuffer cmd) {
    for (int idx : m_order) {
        Pass& p = m_passes[idx];
        for (const auto& a : p.reads)
            barrier(cmd, m_resources[a.resource], a.layout, a.stage, a.access, /*isWrite=*/false);
        for (const auto& a : p.writes)
            barrier(cmd, m_resources[a.resource], a.layout, a.stage, a.access, /*isWrite=*/true);

        RenderPassContext ctx{ cmd, this };
        if (p.execute) p.execute(ctx);
    }

    // Transición final de las imágenes importadas (p.ej. swapchain → PRESENT_SRC).
    for (auto& res : m_resources) {
        if (res.hasFinal && res.layout != res.finalLayout) {
            barrier(cmd, res, res.finalLayout,
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0, /*isWrite=*/false);
        }
    }
}

VkImageView RenderGraph::viewOf(GraphResource r) const   { return m_resources[r].view; }
VkImage     RenderGraph::imageOf(GraphResource r) const  { return m_resources[r].image; }
VkExtent2D  RenderGraph::extentOf(GraphResource r) const { return m_resources[r].extent; }

}  // namespace pk
