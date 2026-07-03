#pragma once

// FluentUI Vulkan backend (FASE 16). Implements the FluentUI::RenderBackend
// virtual interface on top of our Vulkan renderer. Unlike OpenGLBackend, this
// one does NOT own the swapchain or queue submit — the host VulkanContext
// passes the current frame's VkCommandBuffer via SetFrameContext() before
// any RenderBackend methods are invoked.
//
// Lifecycle inside one frame:
//   ctx.BeginFrame(...)           // VulkanContext sets up cmd buffer
//   ctx.SetFrameContext(cmd, ext)  // engine wires it to the backend
//   backend->BeginFrame(...)       // FluentUI clears its per-frame caches
//   backend->DrawBatch(...) × N
//   backend->EndFrame()
//   ctx.EndFrame()                 // submit + present

#include "core/RenderBackend.h"

#include "BufferVk.h"
#include "ImageVk.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace pokemotor::vk {

class VulkanBackend final : public FluentUI::RenderBackend {
public:
    // Vulkan resources the host owns and shares with the backend. Lifetimes
    // outlive the backend; do NOT destroy here.
    struct InitInfo {
        VkInstance        instance        = VK_NULL_HANDLE;
        VkPhysicalDevice  physicalDevice  = VK_NULL_HANDLE;
        VkDevice          device          = VK_NULL_HANDLE;
        VmaAllocator      allocator       = VK_NULL_HANDLE;
        uint32_t          queueFamily     = 0;
        VkQueue           queue           = VK_NULL_HANDLE;
        VkPipelineCache   pipelineCache   = VK_NULL_HANDLE;
        VkDescriptorPool  descriptorPool  = VK_NULL_HANDLE;
        // Color attachment format the engine renders the UI into — typically
        // the swapchain format (B8G8R8A8_SRGB by default).
        VkFormat          colorFormat     = VK_FORMAT_UNDEFINED;
    };

    explicit VulkanBackend(const InitInfo& info);
    ~VulkanBackend() override;

    VulkanBackend(const VulkanBackend&)            = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

    // ---- Engine ↔ backend wiring (NOT in RenderBackend) ------------------
    // Call once per frame BEFORE any RenderBackend method, after the engine
    // has begun its UI pass (vkCmdBeginRendering on the swapchain image).
    void SetFrameContext(VkCommandBuffer cmd, VkExtent2D extent);

    // Register an externally-owned VkImage (engine-side allocation) as a
    // FluentUI texture handle. The backend allocates and binds a descriptor
    // set to (m_uiSampler, view) but does NOT take ownership of the image —
    // caller keeps the VkImage/VkImageView alive at least until the next
    // backend Shutdown. Use for the editor viewport (m_viewportColor) so
    // FluentUI::Image() can sample it. Returns a handle usable as any
    // textureHandle param. The image must already be in
    // SHADER_READ_ONLY_OPTIMAL layout when DrawBatch samples it; the render
    // graph emits the right barrier when viewportColor is declared as a
    // pass read.
    void* RegisterExternalTexture(VkImageView view);

    // ---- RenderBackend overrides -----------------------------------------
    bool Init(void* windowHandle, void* existingGLContext = nullptr) override;
    void Shutdown() override;
    void BeginFrame(const FluentUI::Color& clearColor) override;
    void EndFrame() override;
    void SetViewport(int width, int height) override;

    void PushClipRect(int x, int y, int width, int height) override;
    void PopClipRect() override;

    void* CreateTexture(int width, int height, const void* data,
                        bool alphaOnly = false) override;
    void  UpdateTexture(void* textureHandle, int x, int y,
                        int width, int height, const void* data) override;
    void  DeleteTexture(void* textureHandle) override;

    void DrawBatch(FluentUI::ShaderType type,
                   const FluentUI::RenderVertex* vertices, size_t vertexCount,
                   const unsigned int* indices, size_t indexCount,
                   void* textureHandle, const float* projectionMatrix,
                   const FluentUI::Color& textColor = {1, 1, 1, 1}) override;

    void DrawLines(const FluentUI::RenderVertex* vertices, size_t vertexCount,
                   float width, const float* projectionMatrix) override;

    void* CreateRenderTarget(int width, int height) override;
    void  SetRenderTarget(void* target) override;
    void* GetRenderTargetTexture(void* target) override;
    void  DeleteRenderTarget(void* target) override;

    void CopyTexture(void* src, void* dst, int width, int height) override;

    // GL state save/restore is meaningless under Vulkan — no implicit global
    // state. Stubs remain harmless no-ops.
    void SaveState() override {}
    void RestoreState() override {}

    FluentUI::Color ReadPixel(int x, int y) override;

private:
    InitInfo m_info;

    // Per-frame transient state (cleared by SetFrameContext / EndFrame).
    VkCommandBuffer m_currentCmd    = VK_NULL_HANDLE;
    VkExtent2D      m_currentExtent = { 0, 0 };
    int             m_viewportW     = 0;
    int             m_viewportH     = 0;

    // Scissor stack — Vulkan replaces glScissor with vkCmdSetScissor; we
    // emulate the push/pop with a host-side stack and apply on each draw.
    struct ScissorRect { int x, y, w, h; };
    std::vector<ScissorRect> m_clipStack;

    // ---- F16.C resources --------------------------------------------------

    // Per-frame-in-flight dynamic vertex/index buffers. DrawBatch appends
    // its batch to the current frame's offsets and records the draw with
    // those offsets. Capacity grows on demand (CreateOrGrowBuffer).
    static constexpr uint32_t kFramesInFlight = 2;
    struct FrameBuffers {
        BufferVk     vertex;        // host-coherent, mapped
        BufferVk     index;         // host-coherent, mapped
        VkDeviceSize vertexBytes  = 0;   // capacity in bytes
        VkDeviceSize indexBytes   = 0;
        VkDeviceSize vertexOffset = 0;   // bump-allocator cursor for this frame
        VkDeviceSize indexOffset  = 0;
    };
    FrameBuffers m_frame[kFramesInFlight]{};
    uint32_t     m_frameIdx = 0;     // advances on EndFrame

    bool growBuffer(BufferVk& out, VkDeviceSize& outCapacity,
                    VkDeviceSize required, VkBufferUsageFlags usage);

    // ---- Textures ---------------------------------------------------------
    // Texture handle = stable uintptr_t key into m_textures. Each texture
    // owns a permanently-bound descriptor set so DrawBatch can just
    // vkCmdBindDescriptorSets without per-frame writes.
    struct UITexture {
        ImageVk         image;
        VkDescriptorSet descSet  = VK_NULL_HANDLE;
        bool            alphaOnly = false;
    };
    std::unordered_map<uintptr_t, std::unique_ptr<UITexture>> m_textures;
    uintptr_t m_nextTextureId = 1;  // 0 reserved for "null texture"

    // Deferred destruction queue. DeleteTexture can fire mid-frame (e.g. the
    // MSDF dynamic atlas grows when a new glyph/icon is first rendered) while
    // earlier DrawBatch calls in the SAME cmd buffer already bound the old
    // texture's descriptor set. Destroying the VkImage/VkImageView right then
    // leaves the descSet dangling, the shader samples freed memory, and MSDF
    // turns that garbage into sharp yellow triangular shards on screen for one
    // frame. We park doomed UITextures here with a frame counter and only
    // release them after kFramesInFlight + 1 frames — guaranteed safe because
    // by then any cmd buffer that referenced the old descSet has signalled its
    // fence.
    struct PendingDelete {
        std::unique_ptr<UITexture> tex;
        int                        framesRemaining;
    };
    std::vector<PendingDelete> m_pendingDeletes;

    VkSampler m_uiSampler = VK_NULL_HANDLE;  // LINEAR + CLAMP_TO_EDGE

    // ---- F16.D pipelines + descriptor infrastructure ----------------------
    // 4 pipelines (one per ShaderType) share a single layout. Layout: 1 set
    // (combined image sampler) + 96 B push constants (mat4 proj + vec4 tint
    // + float pxRange + 3×float pad).
    VkDescriptorPool      m_uiDescPool    = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_uiSetLayout   = VK_NULL_HANDLE;
    VkPipelineLayout      m_uiPipeLayout  = VK_NULL_HANDLE;
    VkPipeline            m_pipeBasic     = VK_NULL_HANDLE;
    VkPipeline            m_pipeText      = VK_NULL_HANDLE;
    VkPipeline            m_pipeMsdf      = VK_NULL_HANDLE;
    VkPipeline            m_pipeImage     = VK_NULL_HANDLE;
    VkPipeline            m_pipeLines     = VK_NULL_HANDLE;  // line topology + dynamic line width

    // A 1×1 white texture used for the Basic shader's `null texture handle`
    // case. CreateTexture allocates the pool entry; Basic shader doesn't
    // sample it but the descriptor set still needs SOMETHING bound.
    UITexture*  m_whiteTexture   = nullptr;

    bool createPipelines();
    void destroyPipelines();

    // Allocates a UI descriptor set and binds (uiSampler, view) into binding 0.
    bool writeTextureDescriptor(UITexture& tex);

    struct PushConstants {
        float projection[16];
        float textColor[4];
        float pxRange;
        float _pad0, _pad1, _pad2;
    };
    static_assert(sizeof(PushConstants) == 96, "UI PushConstants must match GLSL layout");
};

}  // namespace pokemotor::vk
