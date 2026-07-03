#include "VulkanBackend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace pokemotor::vk {

// F16.C agrega resources reales: buffers dinámicos para vertex/index
// data + textures sampleables + sampler. Los stubs F16.B siguen para los
// métodos que aún no se implementan (DrawBatch / DrawLines llegan en
// F16.E cuando los pipelines existan).

#define VKB_TRACE 0
#if VKB_TRACE
    #define VKB_LOG(...) std::fprintf(stderr, "[VulkanBackend] " __VA_ARGS__)
#else
    #define VKB_LOG(...) (void)0
#endif

namespace {
std::vector<char> readBinaryFile(const char* path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) return {};
    size_t size = static_cast<size_t>(f.tellg());
    std::vector<char> buf(size);
    f.seekg(0);
    f.read(buf.data(), static_cast<std::streamsize>(size));
    return buf;
}

VkShaderModule loadSpv(VkDevice device, const char* path) {
    auto bytes = readBinaryFile(path);
    if (bytes.empty()) {
        std::fprintf(stderr, "[VulkanBackend] failed to load SPV: %s\n", path);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(bytes.data());
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &info, nullptr, &m);
    return m;
}

// Helper for one-shot copy commands when uploading texture data.
class OneShotCmd {
public:
    OneShotCmd(VkDevice device, uint32_t queueFamily, VkQueue queue)
        : m_device(device), m_queue(queue) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = queueFamily;
        vkCreateCommandPool(device, &poolInfo, nullptr, &m_pool);

        VkCommandBufferAllocateInfo alloc{};
        alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool        = m_pool;
        alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &alloc, &m_cmd);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_cmd, &begin);
    }
    ~OneShotCmd() {
        vkEndCommandBuffer(m_cmd);
        VkSubmitInfo submit{};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &m_cmd;
        vkQueueSubmit(m_queue, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_queue);
        vkDestroyCommandPool(m_device, m_pool, nullptr);
    }
    VkCommandBuffer cmd() const { return m_cmd; }

private:
    VkDevice        m_device = VK_NULL_HANDLE;
    VkQueue         m_queue  = VK_NULL_HANDLE;
    VkCommandPool   m_pool   = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd    = VK_NULL_HANDLE;
};
}  // namespace

VulkanBackend::VulkanBackend(const InitInfo& info) : m_info(info) {
    VKB_LOG("ctor — device=%p, queue=%p, fmt=%d\n",
            static_cast<void*>(m_info.device),
            static_cast<void*>(m_info.queue),
            int(m_info.colorFormat));
}

VulkanBackend::~VulkanBackend() {
    Shutdown();
}

void VulkanBackend::SetFrameContext(VkCommandBuffer cmd, VkExtent2D extent) {
    m_currentCmd    = cmd;
    m_currentExtent = extent;
}

// ===== RenderBackend overrides ============================================

bool VulkanBackend::Init(void* windowHandle, void* existingGLContext) {
    (void)windowHandle;
    (void)existingGLContext;

    // Idempotent: ctx.CreateUIBackend() Inits once, then FluentUI::CreateContext
    // calls backend->Init again with (window, glCtx) as part of its normal flow.
    // Without this guard the second call would re-create pipelines / pool /
    // buffers / white texture and leak the first set on shutdown.
    if (m_pipeBasic != VK_NULL_HANDLE) return true;

    // ---- F16.C dynamic vertex/index buffers (per frame in flight) -----
    // Start with modest capacity that covers typical UI frames (~32 KB
    // each), grow on demand if a single DrawBatch overshoots.
    constexpr VkDeviceSize kInitialVB = 32 * 1024;
    constexpr VkDeviceSize kInitialIB = 16 * 1024;
    for (auto& f : m_frame) {
        if (!f.vertex.CreateHostCoherent(m_info.allocator, kInitialVB,
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            std::fprintf(stderr, "[VulkanBackend] vertex buffer create failed\n");
            return false;
        }
        if (!f.index.CreateHostCoherent(m_info.allocator, kInitialIB,
                                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) {
            std::fprintf(stderr, "[VulkanBackend] index buffer create failed\n");
            return false;
        }
        f.vertexBytes = kInitialVB;
        f.indexBytes  = kInitialIB;
    }

    // ---- UI sampler: LINEAR + CLAMP_TO_EDGE (FluentUI atlases are
    //      non-tiling and we never want wrap artefacts on the edge). ----
    if (m_uiSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo s{};
        s.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        s.magFilter    = VK_FILTER_LINEAR;
        s.minFilter    = VK_FILTER_LINEAR;
        s.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        s.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.minLod       = 0.0f;
        s.maxLod       = 0.0f;
        if (vkCreateSampler(m_info.device, &s, nullptr, &m_uiSampler) != VK_SUCCESS) {
            std::fprintf(stderr, "[VulkanBackend] UI sampler create failed\n");
            return false;
        }
    }

    if (!createPipelines()) return false;

    // 1×1 white default texture for the Basic shader's null-texture path.
    // Frontend-supplied `null textureHandle` in DrawBatch falls back to this
    // so the pipeline layout always has a bound descriptor set.
    const uint8_t white[4] = { 255, 255, 255, 255 };
    void* whiteHandle = CreateTexture(1, 1, white, /*alphaOnly=*/false);
    if (whiteHandle) {
        m_whiteTexture = m_textures[reinterpret_cast<uintptr_t>(whiteHandle)].get();
    }

    VKB_LOG("Init OK\n");
    return true;
}

void VulkanBackend::Shutdown() {
    if (m_info.device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_info.device);

    m_whiteTexture = nullptr;
    m_textures.clear();          // unique_ptr destroys each UITexture's ImageVk
    // vkDeviceWaitIdle above ensured the GPU is fully idle — anything still
    // parked in the deferred queue is safe to release immediately.
    m_pendingDeletes.clear();

    destroyPipelines();

    for (auto& f : m_frame) {
        f.vertex.Destroy();
        f.index.Destroy();
        f.vertexBytes = f.indexBytes = 0;
    }

    if (m_uiSampler) {
        vkDestroySampler(m_info.device, m_uiSampler, nullptr);
        m_uiSampler = VK_NULL_HANDLE;
    }
    m_info = {};   // null out so a second Shutdown() is a no-op
    VKB_LOG("Shutdown\n");
}

void VulkanBackend::BeginFrame(const FluentUI::Color& clearColor) {
    (void)clearColor;
    m_clipStack.clear();

    // Tick the deferred-destruction queue. Anything that reaches 0 has
    // outlived every in-flight cmd buffer that could still reference it
    // (kFramesInFlight + 1 frames of grace) and is safe to destroy now.
    for (auto it = m_pendingDeletes.begin(); it != m_pendingDeletes.end(); ) {
        if (--it->framesRemaining <= 0) {
            it = m_pendingDeletes.erase(it);
        } else {
            ++it;
        }
    }

    // Reset bump cursors for the current frame's buffers.
    m_frame[m_frameIdx].vertexOffset = 0;
    m_frame[m_frameIdx].indexOffset  = 0;

    // [DIAG] Frame boundary marker. Lets us correlate which events fire on
    // a given frame when bisecting the yellow-patches bug. Remove after diag.
    static uint64_t s_frameCounter = 0;
    std::fprintf(stderr, "\n--- [VKB] BeginFrame #%llu (slot=%u, pendingDel=%zu) ---\n",
                 (unsigned long long)s_frameCounter++, m_frameIdx, m_pendingDeletes.size());
}

void VulkanBackend::EndFrame() {
    m_currentCmd    = VK_NULL_HANDLE;
    m_currentExtent = { 0, 0 };
    m_frameIdx      = (m_frameIdx + 1) % kFramesInFlight;
}

void VulkanBackend::SetViewport(int width, int height) {
    m_viewportW = width;
    m_viewportH = height;
}

void VulkanBackend::PushClipRect(int x, int y, int width, int height) {
    m_clipStack.push_back({ x, y, width, height });
}

void VulkanBackend::PopClipRect() {
    if (!m_clipStack.empty()) m_clipStack.pop_back();
}

// ---- Textures ------------------------------------------------------------

void* VulkanBackend::CreateTexture(int width, int height, const void* data, bool alphaOnly) {
    if (width <= 0 || height <= 0) return nullptr;

    auto tex = std::make_unique<UITexture>();
    tex->alphaOnly = alphaOnly;

    // FluentUI hands us 8-bit RED for alphaOnly atlases (bitmap text) and
    // 32-bit RGBA otherwise. Match those exact formats so the shader can
    // .r-swizzle for text or sample the full color for images.
    const VkFormat fmt = alphaOnly ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize bytes =
        static_cast<VkDeviceSize>(width) * height * (alphaOnly ? 1 : 4);

    ImageCreateParams p{};
    p.width  = static_cast<uint32_t>(width);
    p.height = static_cast<uint32_t>(height);
    p.format = fmt;
    p.usage  = VK_IMAGE_USAGE_TRANSFER_DST_BIT
             | VK_IMAGE_USAGE_TRANSFER_SRC_BIT  // CopyTexture support
             | VK_IMAGE_USAGE_SAMPLED_BIT;
    p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (!tex->image.Create(m_info.device, m_info.allocator, p)) {
        std::fprintf(stderr, "[VulkanBackend] CreateTexture image alloc failed (%dx%d)\n",
                     width, height);
        return nullptr;
    }

    // Upload + transition. If data is null we still leave the image in
    // SHADER_READ_ONLY_OPTIMAL so subsequent UpdateTexture calls have a
    // well-defined source layout.
    BufferVk staging;
    if (data && bytes > 0) {
        if (!staging.CreateStaging(m_info.allocator, bytes)) {
            std::fprintf(stderr, "[VulkanBackend] staging alloc failed\n");
            return nullptr;
        }
        std::memcpy(staging.Mapped(), data, static_cast<size_t>(bytes));
    }

    {
        OneShotCmd one(m_info.device, m_info.queueFamily, m_info.queue);
        // Sync2 splits TRANSFER into COPY/CLEAR/BLIT/RESOLVE. The clear path
        // and the copy path live in different stages, so the surrounding
        // layout-transition barriers have to match whichever one we use.
        const VkPipelineStageFlags2 transferStage =
            (data && bytes > 0) ? VK_PIPELINE_STAGE_2_COPY_BIT
                                : VK_PIPELINE_STAGE_2_CLEAR_BIT;
        tex->image.CmdTransition(one.cmd(),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            transferStage, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (data && bytes > 0) {
            tex->image.CmdCopyFromBuffer(one.cmd(), staging.Handle());
        } else {
            // No initial data — clear to zero so unsampled atlas regions
            // don't return uninitialized VRAM (which manifests as random
            // yellow/coloured splotches when MSDF / bitmap atlas slots
            // get sampled before any UpdateTexture has written there).
            VkClearColorValue clear{};
            clear.float32[0] = 0.0f;
            clear.float32[1] = 0.0f;
            clear.float32[2] = 0.0f;
            clear.float32[3] = 0.0f;
            VkImageSubresourceRange range{};
            range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            range.baseMipLevel   = 0;
            range.levelCount     = 1;
            range.baseArrayLayer = 0;
            range.layerCount     = 1;
            vkCmdClearColorImage(one.cmd(), tex->image.Handle(),
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &clear, 1, &range);
        }
        tex->image.CmdTransition(one.cmd(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            transferStage, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    if (!writeTextureDescriptor(*tex)) {
        std::fprintf(stderr, "[VulkanBackend] CreateTexture descriptor write failed\n");
        return nullptr;
    }

    const uintptr_t id = m_nextTextureId++;
    m_textures.emplace(id, std::move(tex));
    std::fprintf(stderr, "[VKB] CreateTexture id=%llu %dx%d alphaOnly=%d hasData=%d\n",
                 (unsigned long long)id, width, height, int(alphaOnly), int(data != nullptr));
    return reinterpret_cast<void*>(id);
}

void VulkanBackend::UpdateTexture(void* textureHandle, int x, int y,
                                  int width, int height, const void* data) {
    if (!textureHandle || !data || width <= 0 || height <= 0) return;
    auto it = m_textures.find(reinterpret_cast<uintptr_t>(textureHandle));
    if (it == m_textures.end()) return;
    UITexture& tex = *it->second;

    std::fprintf(stderr, "[VKB] UpdateTexture id=%llu rect=(%d,%d,%d,%d)\n",
                 (unsigned long long)reinterpret_cast<uintptr_t>(textureHandle),
                 x, y, width, height);

    const VkDeviceSize bytes =
        static_cast<VkDeviceSize>(width) * height * (tex.alphaOnly ? 1 : 4);
    BufferVk staging;
    if (!staging.CreateStaging(m_info.allocator, bytes)) return;
    std::memcpy(staging.Mapped(), data, static_cast<size_t>(bytes));

    OneShotCmd one(m_info.device, m_info.queueFamily, m_info.queue);

    tex.image.CmdTransition(one.cmd(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

    // Sub-rect copy (the helper only does full-image copies; do it inline).
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset = { x, y, 0 };
    region.imageExtent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    vkCmdCopyBufferToImage(one.cmd(), staging.Handle(), tex.image.Handle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    tex.image.CmdTransition(one.cmd(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void VulkanBackend::DeleteTexture(void* textureHandle) {
    if (!textureHandle) return;
    auto it = m_textures.find(reinterpret_cast<uintptr_t>(textureHandle));
    if (it == m_textures.end()) return;

    std::fprintf(stderr, "[VKB] DeleteTexture id=%llu (deferred %d frames)\n",
                 (unsigned long long)reinterpret_cast<uintptr_t>(textureHandle),
                 int(kFramesInFlight) + 1);

    // Defer the actual VkImage/VkImageView/descSet release: the current cmd
    // buffer may have already recorded vkCmdBindDescriptorSets pointing at
    // this texture's descSet earlier in the same frame, and that cmd buffer
    // hasn't been submitted yet — a vkDeviceWaitIdle here would NOT wait for
    // it. Park the UITexture on the deferred queue; BeginFrame ticks it down
    // and the destructor runs once kFramesInFlight + 1 frames have passed.
    m_pendingDeletes.push_back({ std::move(it->second), int(kFramesInFlight) + 1 });
    m_textures.erase(it);
}

// ---- Buffer growth helper -----------------------------------------------

bool VulkanBackend::growBuffer(BufferVk& out, VkDeviceSize& outCapacity,
                               VkDeviceSize required, VkBufferUsageFlags usage) {
    VkDeviceSize newCap = std::max<VkDeviceSize>(outCapacity * 2u, required);
    // Round to 4 KB.
    newCap = ((newCap + 4095u) / 4096u) * 4096u;
    vkDeviceWaitIdle(m_info.device);
    out.Destroy();
    if (!out.CreateHostCoherent(m_info.allocator, newCap, usage)) return false;
    outCapacity = newCap;
    return true;
}

// ---- Draw stubs (real implementation in F16.E) --------------------------

void VulkanBackend::DrawBatch(FluentUI::ShaderType type,
                              const FluentUI::RenderVertex* vertices, size_t vertexCount,
                              const unsigned int* indices, size_t indexCount,
                              void* textureHandle, const float* projectionMatrix,
                              const FluentUI::Color& textColor) {
    if (!m_currentCmd || vertexCount == 0 || indexCount == 0) return;

    FrameBuffers& f = m_frame[m_frameIdx];
    const VkDeviceSize vBytes = vertexCount * sizeof(FluentUI::RenderVertex);
    const VkDeviceSize iBytes = indexCount  * sizeof(unsigned int);

    // Overflow guard. We don't grow mid-frame because the previously
    // recorded draws still reference the OLD buffer handle; growing would
    // invalidate them. Capacity is initially generous; if a frame really
    // overshoots we just drop the offending batch and log.
    if (f.vertexOffset + vBytes > f.vertexBytes) {
        std::fprintf(stderr,
                     "[VulkanBackend] vertex buffer overflow (%llu + %llu > %llu) — skipping batch\n",
                     (unsigned long long)f.vertexOffset,
                     (unsigned long long)vBytes,
                     (unsigned long long)f.vertexBytes);
        return;
    }
    if (f.indexOffset + iBytes > f.indexBytes) {
        std::fprintf(stderr, "[VulkanBackend] index buffer overflow — skipping batch\n");
        return;
    }

    std::memcpy(static_cast<char*>(f.vertex.Mapped()) + f.vertexOffset, vertices, static_cast<size_t>(vBytes));
    std::memcpy(static_cast<char*>(f.index.Mapped())  + f.indexOffset,  indices,  static_cast<size_t>(iBytes));

    const VkDeviceSize vOffset = f.vertexOffset;
    const VkDeviceSize iOffset = f.indexOffset;
    f.vertexOffset += vBytes;
    f.indexOffset  += iBytes;

    // [DIAG] Per-batch record. Shows shader type, batch size, texture handle,
    // and how close we are to the buffer cap. Spammy but the only way to see
    // overflow / odd texture lookups when the flash fires. Remove after diag.
    std::fprintf(stderr, "[VKB] DrawBatch type=%d v=%zu i=%zu tex=%p vCap=%llu/%llu iCap=%llu/%llu\n",
                 int(type), vertexCount, indexCount, textureHandle,
                 (unsigned long long)f.vertexOffset, (unsigned long long)f.vertexBytes,
                 (unsigned long long)f.indexOffset,  (unsigned long long)f.indexBytes);

    // Pick pipeline.
    VkPipeline pipe = m_pipeBasic;
    switch (type) {
        case FluentUI::ShaderType::Basic: pipe = m_pipeBasic; break;
        case FluentUI::ShaderType::Text:  pipe = m_pipeText;  break;
        case FluentUI::ShaderType::MSDF:  pipe = m_pipeMsdf;  break;
        case FluentUI::ShaderType::Image: pipe = m_pipeImage; break;
    }

    // Pick descriptor set — falls back to the 1×1 white texture so the
    // Basic shader never has an unbound set.
    UITexture* tex = m_whiteTexture;
    if (textureHandle) {
        auto it = m_textures.find(reinterpret_cast<uintptr_t>(textureHandle));
        if (it != m_textures.end()) tex = it->second.get();
    }
    if (!tex || tex->descSet == VK_NULL_HANDLE) return;

    vkCmdBindPipeline(m_currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(m_currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_uiPipeLayout, 0, 1, &tex->descSet, 0, nullptr);

    PushConstants pc{};
    std::memcpy(pc.projection, projectionMatrix, sizeof(pc.projection));
    pc.textColor[0] = textColor.r;
    pc.textColor[1] = textColor.g;
    pc.textColor[2] = textColor.b;
    pc.textColor[3] = textColor.a;
    pc.pxRange      = 4.0f;  // sensible default; MSDF atlases typically use 2-6
    vkCmdPushConstants(m_currentCmd, m_uiPipeLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pc);

    // Viewport with negative height — Vulkan 1.1+ flips Y at rasterization,
    // matching FluentUI's OpenGL-style Y-up ortho without touching the
    // host-provided projection matrix. Origin moves to the bottom-left to
    // stay inside the framebuffer.
    VkViewport vp{};
    vp.x        = 0.0f;
    vp.y        = static_cast<float>(m_viewportH);
    vp.width    = static_cast<float>(m_viewportW);
    vp.height   = -static_cast<float>(m_viewportH);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(m_currentCmd, 0, 1, &vp);

    VkRect2D scissor{};
    if (m_clipStack.empty()) {
        scissor = { {0, 0}, { static_cast<uint32_t>(m_viewportW), static_cast<uint32_t>(m_viewportH) } };
    } else {
        const auto& c = m_clipStack.back();
        scissor.offset.x      = std::max(0, c.x);
        scissor.offset.y      = std::max(0, c.y);
        scissor.extent.width  = static_cast<uint32_t>(std::max(0, c.w));
        scissor.extent.height = static_cast<uint32_t>(std::max(0, c.h));
    }
    vkCmdSetScissor(m_currentCmd, 0, 1, &scissor);

    VkBuffer vb = f.vertex.Handle();
    vkCmdBindVertexBuffers(m_currentCmd, 0, 1, &vb, &vOffset);
    vkCmdBindIndexBuffer(m_currentCmd, f.index.Handle(), iOffset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(m_currentCmd, static_cast<uint32_t>(indexCount), 1, 0, 0, 0);
}

void VulkanBackend::DrawLines(const FluentUI::RenderVertex* vertices, size_t vertexCount,
                              float width, const float* projectionMatrix) {
    if (!m_currentCmd || vertexCount == 0) return;

    // [DIAG flash hunt] Dump every DrawLines call: vertex count, width, and
    // sanity-check the first 4 vertex positions for NaN/inf or absurd values.
    // If FluentUI is feeding us a degenerate endpoint during hover, the
    // sub-pixel-wide line stretches across the whole framebuffer — that is
    // the exact "diagonal lines across the screen" the user is reporting.
    {
        bool anyBad = false;
        float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
        for (size_t i = 0; i < vertexCount; ++i) {
            float x = vertices[i].x, y = vertices[i].y;
            if (!std::isfinite(x) || !std::isfinite(y)) { anyBad = true; }
            if (x < minX) minX = x;  if (x > maxX) maxX = x;
            if (y < minY) minY = y;  if (y > maxY) maxY = y;
        }
        std::fprintf(stderr,
                     "[VKB] DrawLines v=%zu width=%.2f x=[%.1f..%.1f] y=[%.1f..%.1f]%s vp=%dx%d\n",
                     vertexCount, width, minX, maxX, minY, maxY,
                     anyBad ? " NaN/INF!" : "",
                     m_viewportW, m_viewportH);
        size_t dumpN = vertexCount < 4 ? vertexCount : 4;
        for (size_t i = 0; i < dumpN; ++i) {
            std::fprintf(stderr, "  v[%zu] pos=(%.2f, %.2f) uv=(%.3f, %.3f) rgba=(%.2f, %.2f, %.2f, %.2f)\n",
                         i, vertices[i].x, vertices[i].y,
                         vertices[i].u, vertices[i].v,
                         vertices[i].r, vertices[i].g, vertices[i].b, vertices[i].a);
        }
    }

    FrameBuffers& f = m_frame[m_frameIdx];
    const VkDeviceSize vBytes = vertexCount * sizeof(FluentUI::RenderVertex);
    if (f.vertexOffset + vBytes > f.vertexBytes) {
        std::fprintf(stderr, "[VulkanBackend] vertex overflow in DrawLines — skipping\n");
        return;
    }
    std::memcpy(static_cast<char*>(f.vertex.Mapped()) + f.vertexOffset, vertices, static_cast<size_t>(vBytes));
    const VkDeviceSize vOffset = f.vertexOffset;
    f.vertexOffset += vBytes;

    vkCmdBindPipeline(m_currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeLines);
    // The pipeline layout REQUIRES set 0 (combined image sampler). If the white
    // texture failed to allocate we'd record a draw with NO descriptor bound,
    // which is undefined behaviour and could produce arbitrary garbage. Log
    // loudly if we ever hit that path.
    if (m_whiteTexture && m_whiteTexture->descSet) {
        vkCmdBindDescriptorSets(m_currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_uiPipeLayout, 0, 1, &m_whiteTexture->descSet, 0, nullptr);
    } else {
        std::fprintf(stderr, "[VKB] !!! DrawLines: m_whiteTexture is null — skipping descriptor bind (UB)\n");
    }

    PushConstants pc{};
    std::memcpy(pc.projection, projectionMatrix, sizeof(pc.projection));
    pc.textColor[0] = pc.textColor[1] = pc.textColor[2] = pc.textColor[3] = 1.0f;
    pc.pxRange = 0.0f;
    vkCmdPushConstants(m_currentCmd, m_uiPipeLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pc);

    // Same Y-flip as DrawBatch.
    VkViewport vp{};
    vp.x      = 0.0f;
    vp.y      = static_cast<float>(m_viewportH);
    vp.width  = static_cast<float>(m_viewportW);
    vp.height = -static_cast<float>(m_viewportH);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(m_currentCmd, 0, 1, &vp);

    VkRect2D scissor{ {0, 0}, { static_cast<uint32_t>(m_viewportW), static_cast<uint32_t>(m_viewportH) } };
    vkCmdSetScissor(m_currentCmd, 0, 1, &scissor);
    // Hardware may clamp line width to {1.0} unless wideLines feature is enabled —
    // for the smoke test we accept the clamp.
    vkCmdSetLineWidth(m_currentCmd, std::max(width, 1.0f));

    VkBuffer vb = f.vertex.Handle();
    vkCmdBindVertexBuffers(m_currentCmd, 0, 1, &vb, &vOffset);
    vkCmdDraw(m_currentCmd, static_cast<uint32_t>(vertexCount), 1, 0, 0);
}

void* VulkanBackend::CreateRenderTarget(int width, int height) {
    std::fprintf(stderr, "[VKB] !!! CreateRenderTarget(%dx%d) — STUB returning nullptr\n", width, height);
    return nullptr;
}

void VulkanBackend::SetRenderTarget(void* target) {
    std::fprintf(stderr, "[VKB] !!! SetRenderTarget(target=%p) — STUB no-op\n", target);
}
void* VulkanBackend::GetRenderTargetTexture(void* target) {
    std::fprintf(stderr, "[VKB] !!! GetRenderTargetTexture(target=%p) — STUB returning input\n", target);
    return target;
}
void VulkanBackend::DeleteRenderTarget(void* target) {
    std::fprintf(stderr, "[VKB] !!! DeleteRenderTarget(target=%p) — STUB no-op\n", target);
}
void VulkanBackend::CopyTexture(void* src, void* dst, int w, int h) {
    std::fprintf(stderr, "[VKB] !!! CopyTexture(src=%p dst=%p %dx%d) — STUB no-op\n", src, dst, w, h);
}

FluentUI::Color VulkanBackend::ReadPixel(int x, int y) {
    (void)x; (void)y;
    return FluentUI::Color(0, 0, 0, 0);
}

// ===== F16.D pipelines + descriptor infrastructure ========================

bool VulkanBackend::createPipelines() {
    // ---- Descriptor pool: 1 combined image sampler per UI texture ------
    constexpr uint32_t kMaxUITextures = 1024;
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = kMaxUITextures;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = kMaxUITextures;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    if (vkCreateDescriptorPool(m_info.device, &poolInfo, nullptr,
                               &m_uiDescPool) != VK_SUCCESS) {
        std::fprintf(stderr, "[VulkanBackend] UI desc pool create failed\n");
        return false;
    }

    // ---- Descriptor set layout: binding 0 = combined image sampler -----
    VkDescriptorSetLayoutBinding b{};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo slInfo{};
    slInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    slInfo.bindingCount = 1;
    slInfo.pBindings    = &b;
    if (vkCreateDescriptorSetLayout(m_info.device, &slInfo, nullptr,
                                    &m_uiSetLayout) != VK_SUCCESS) return false;

    // ---- Pipeline layout: 1 set + 96 B push constants ------------------
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &m_uiSetLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(m_info.device, &plInfo, nullptr,
                               &m_uiPipeLayout) != VK_SUCCESS) return false;

    // ---- Vertex input shape: matches FluentUI::RenderVertex (32 B) -----
    VkVertexInputBindingDescription vbind{};
    vbind.binding = 0;
    vbind.stride  = sizeof(FluentUI::RenderVertex);
    vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription vattr[3]{};
    vattr[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(FluentUI::RenderVertex, x) };
    vattr[1] = { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(FluentUI::RenderVertex, r) };
    vattr[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(FluentUI::RenderVertex, u) };

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &vbind;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions    = vattr;

    // ---- Shared graphics state -----------------------------------------
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    // Premultiplied-style alpha blend matching the OpenGL backend.
    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable         = VK_TRUE;
    att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.colorBlendOp        = VK_BLEND_OP_ADD;
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.alphaBlendOp        = VK_BLEND_OP_ADD;
    att.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkDynamicState dyn[3] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_LINE_WIDTH,
    };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 3;
    dynState.pDynamicStates    = dyn;

    VkPipelineRenderingCreateInfo render{};
    render.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    render.colorAttachmentCount    = 1;
    render.pColorAttachmentFormats = &m_info.colorFormat;

    // ---- Vertex shader (shared) ----------------------------------------
    VkShaderModule vs = loadSpv(m_info.device, "Shaders/SPV/ui.vert.spv");
    if (!vs) return false;

    auto makePipeline = [&](const char* fragSpv, VkPrimitiveTopology topo,
                            VkPipeline& out) -> bool {
        VkShaderModule fs = loadSpv(m_info.device, fragSpv);
        if (!fs) return false;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = topo;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName  = "main";

        VkGraphicsPipelineCreateInfo info{};
        info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext               = &render;
        info.stageCount          = 2;
        info.pStages             = stages;
        info.pVertexInputState   = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState      = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState   = &ms;
        info.pDepthStencilState  = &ds;
        info.pColorBlendState    = &cb;
        info.pDynamicState       = &dynState;
        info.layout              = m_uiPipeLayout;
        VkResult r = vkCreateGraphicsPipelines(m_info.device, m_info.pipelineCache, 1,
                                               &info, nullptr, &out);
        vkDestroyShaderModule(m_info.device, fs, nullptr);
        return r == VK_SUCCESS;
    };

    bool ok = true;
    ok &= makePipeline("Shaders/SPV/ui_basic.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, m_pipeBasic);
    ok &= makePipeline("Shaders/SPV/ui_text.frag.spv",  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, m_pipeText);
    ok &= makePipeline("Shaders/SPV/ui_msdf.frag.spv",  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, m_pipeMsdf);
    ok &= makePipeline("Shaders/SPV/ui_image.frag.spv", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, m_pipeImage);
    // Lines reuse the Basic fragment but with line-list topology + dynamic line width.
    ok &= makePipeline("Shaders/SPV/ui_basic.frag.spv", VK_PRIMITIVE_TOPOLOGY_LINE_LIST,    m_pipeLines);
    vkDestroyShaderModule(m_info.device, vs, nullptr);
    return ok;
}

void VulkanBackend::destroyPipelines() {
    auto kill = [&](VkPipeline& p) {
        if (p) { vkDestroyPipeline(m_info.device, p, nullptr); p = VK_NULL_HANDLE; }
    };
    kill(m_pipeBasic);
    kill(m_pipeText);
    kill(m_pipeMsdf);
    kill(m_pipeImage);
    kill(m_pipeLines);
    if (m_uiPipeLayout) {
        vkDestroyPipelineLayout(m_info.device, m_uiPipeLayout, nullptr);
        m_uiPipeLayout = VK_NULL_HANDLE;
    }
    if (m_uiSetLayout) {
        vkDestroyDescriptorSetLayout(m_info.device, m_uiSetLayout, nullptr);
        m_uiSetLayout = VK_NULL_HANDLE;
    }
    if (m_uiDescPool) {
        vkDestroyDescriptorPool(m_info.device, m_uiDescPool, nullptr);
        m_uiDescPool = VK_NULL_HANDLE;
    }
}

void* VulkanBackend::RegisterExternalTexture(VkImageView view) {
    if (view == VK_NULL_HANDLE) return nullptr;

    auto tex = std::make_unique<UITexture>();
    // image stays default-constructed (no owned VkImage). Destroy is a
    // no-op for that state, so m_textures.clear() on Shutdown won't try
    // to free the caller's resources.

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = m_uiDescPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts        = &m_uiSetLayout;
    if (vkAllocateDescriptorSets(m_info.device, &alloc, &tex->descSet) != VK_SUCCESS) {
        std::fprintf(stderr, "[VulkanBackend] RegisterExternalTexture descriptor alloc failed\n");
        return nullptr;
    }

    VkDescriptorImageInfo info{};
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.imageView   = view;
    info.sampler     = m_uiSampler;
    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = tex->descSet;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &info;
    vkUpdateDescriptorSets(m_info.device, 1, &w, 0, nullptr);

    const uintptr_t id = m_nextTextureId++;
    m_textures.emplace(id, std::move(tex));
    VKB_LOG("RegisterExternalTexture id=%llu view=%p\n",
            (unsigned long long)id, (void*)view);
    return reinterpret_cast<void*>(id);
}

bool VulkanBackend::writeTextureDescriptor(UITexture& tex) {
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = m_uiDescPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts        = &m_uiSetLayout;
    if (vkAllocateDescriptorSets(m_info.device, &alloc, &tex.descSet) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorImageInfo info{};
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.imageView   = tex.image.View();
    info.sampler     = m_uiSampler;
    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = tex.descSet;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &info;
    vkUpdateDescriptorSets(m_info.device, 1, &w, 0, nullptr);
    return true;
}

}  // namespace pokemotor::vk
