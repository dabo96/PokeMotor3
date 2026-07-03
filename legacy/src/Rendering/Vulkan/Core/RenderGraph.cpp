#include "RenderGraph.h"

namespace pokemotor::vk {

GraphResource RenderGraph::ImportImage(std::string name,
                                       VkImage image,
                                       VkImageAspectFlags aspect,
                                       VkImageLayout currentLayout,
                                       VkPipelineStageFlags2 initialStage,
                                       VkAccessFlags2 initialAccess) {
    ResourceState r;
    r.name   = std::move(name);
    r.image  = image;
    r.aspect = aspect;

    // Reuse the last-known (layout, stage, access) for this VkImage if we've
    // seen it before — that's what lets cross-frame barriers chain against
    // the previous frame's storeOp / shader writes instead of starting from
    // UNDEFINED + TOP_OF_PIPE (which sync-validation flags as a hazard).
    auto it = m_persisted.find(image);
    if (it != m_persisted.end()) {
        r.layout = it->second.layout;
        r.stage  = it->second.stage;
        r.access = it->second.access;
    } else {
        r.layout = currentLayout;
        r.stage  = initialStage;
        r.access = initialAccess;
    }

    GraphResource handle = static_cast<GraphResource>(m_resources.size());
    m_resources.push_back(std::move(r));
    return handle;
}

void RenderGraph::ForgetImage(VkImage image) {
    m_persisted.erase(image);
}

void RenderGraph::ClearPersistedState() {
    m_persisted.clear();
}

void RenderGraph::AddPass(GraphPass pass) {
    m_passes.push_back(std::move(pass));
}

void RenderGraph::Reset() {
    // Snapshot every imported image's final state so the next frame can
    // resume from where this one left off. m_persisted survives Reset().
    for (const auto& r : m_resources) {
        m_persisted[r.image] = { r.layout, r.stage, r.access };
    }
    m_resources.clear();
    m_passes.clear();
}

void RenderGraph::transitionImage(VkCommandBuffer cmd, ResourceState& res,
                                  VkImageLayout newLayout,
                                  VkPipelineStageFlags2 newStage,
                                  VkAccessFlags2 newAccess) {
    // Skip the no-op case: same layout + identical r/w access on the same stage
    // would still legally need a barrier if writes were involved, but for
    // back-to-back reads with no layout change we can drop it. Keep the simple
    // rule: barrier whenever any of (layout, stage, access) changes.
    if (res.layout == newLayout && res.stage == newStage && res.access == newAccess) {
        return;
    }

    VkImageMemoryBarrier2 b{};
    b.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask  = res.stage;
    b.srcAccessMask = res.access;
    b.dstStageMask  = newStage;
    b.dstAccessMask = newAccess;
    b.oldLayout     = res.layout;
    b.newLayout     = newLayout;
    b.image         = res.image;
    b.subresourceRange = { res.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };

    VkDependencyInfo dep{};
    dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount  = 1;
    dep.pImageMemoryBarriers     = &b;
    vkCmdPipelineBarrier2(cmd, &dep);

    res.layout = newLayout;
    res.stage  = newStage;
    res.access = newAccess;
}

void RenderGraph::Execute(VkCommandBuffer cmd) {
    for (const auto& pass : m_passes) {
        // Reads + writes are both layout/stage/access requirements. We treat
        // them uniformly for barrier purposes — the read/write distinction
        // matters for future DAG analysis, not for this linear graph.
        for (const auto& a : pass.reads) {
            if (a.resource == kInvalidResource) continue;
            transitionImage(cmd, m_resources[a.resource], a.layout, a.stage, a.access);
        }
        for (const auto& a : pass.writes) {
            if (a.resource == kInvalidResource) continue;
            transitionImage(cmd, m_resources[a.resource], a.layout, a.stage, a.access);
        }
        if (pass.execute) pass.execute(cmd);
    }
}

}  // namespace pokemotor::vk
