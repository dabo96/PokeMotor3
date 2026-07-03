#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pokemotor::vk {

// Opaque handle into RenderGraph's resource table. Stable for the lifetime of
// one frame (graph is Reset() each frame).
using GraphResource = uint32_t;
constexpr GraphResource kInvalidResource = static_cast<GraphResource>(-1);

// Per-pass declared usage of an image resource. The graph compares the
// requested state against the resource's current tracked state and inserts a
// vkCmdPipelineBarrier2 when they differ.
struct ImageAccess {
    GraphResource           resource = kInvalidResource;
    VkImageLayout           layout   = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2   stage    = 0;
    VkAccessFlags2          access   = 0;
};

struct GraphPass {
    std::string                                name;
    std::vector<ImageAccess>                   reads;
    std::vector<ImageAccess>                   writes;
    // Called between barrier insertion and the next pass. Opaque to the graph —
    // callers do vkCmdBeginRendering / vkCmdBindPipeline / draw / etc. inside.
    // Pass nullptr for "barrier-only" passes (e.g. swapchain layout finalize).
    std::function<void(VkCommandBuffer)>       execute;
};

// Linear render graph: passes execute in submission order. The graph keeps a
// per-resource (layout, stage, access) snapshot and emits one barrier per
// resource whose state differs from what the upcoming pass declares.
//
// What it does NOT do (FASE 6 scope):
// - No DAG / reordering / culling.
// - No transient resource creation.
// - No buffer tracking (only images for now — extend later if needed).
class RenderGraph {
public:
    // Register an externally-owned image. Returns a handle valid until Reset().
    //
    // If the same VkImage was used in a previous frame, the graph picks up
    // (layout, stage, access) from the last barrier that touched it — so the
    // first transition of the new frame chains correctly against the prior
    // frame's final use. The currentLayout / initialStage / initialAccess
    // arguments are used only on the *first* registration of a given VkImage
    // (e.g. for swapchain images, pass COLOR_ATTACHMENT_OUTPUT_BIT to match
    // the acquire semaphore's wait stage).
    GraphResource ImportImage(std::string name,
                              VkImage image,
                              VkImageAspectFlags aspect,
                              VkImageLayout currentLayout,
                              VkPipelineStageFlags2 initialStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                              VkAccessFlags2 initialAccess = 0);

    // Forget any persisted state for an image (e.g. before swapchain
    // recreation invalidates the handles).
    void ForgetImage(VkImage image);

    // Drop ALL persisted per-image state. Call after swapchain recreation
    // invalidates VkImage handles, so cached entries can't collide if VMA
    // happens to reuse them.
    void ClearPersistedState();

    void AddPass(GraphPass pass);

    // Walks passes in order, inserts required barriers, runs each pass's execute().
    void Execute(VkCommandBuffer cmd);

    // Clear per-frame state (resources + passes). Persistent per-VkImage
    // state is preserved across calls so cross-frame barriers can chain.
    void Reset();

private:
    struct ResourceState {
        std::string           name;
        VkImage               image;
        VkImageAspectFlags    aspect;
        VkImageLayout         layout;
        VkPipelineStageFlags2 stage;
        VkAccessFlags2        access;
    };

    struct PersistedState {
        VkImageLayout         layout;
        VkPipelineStageFlags2 stage;
        VkAccessFlags2        access;
    };

    void transitionImage(VkCommandBuffer cmd, ResourceState& res,
                         VkImageLayout newLayout,
                         VkPipelineStageFlags2 newStage,
                         VkAccessFlags2 newAccess);

    std::vector<ResourceState>                    m_resources;
    std::vector<GraphPass>                        m_passes;
    std::unordered_map<VkImage, PersistedState>   m_persisted;
};

}  // namespace pokemotor::vk
