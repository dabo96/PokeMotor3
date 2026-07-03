#include "VulkanContext.h"

#include "GltfLoader.h"
#include "Rendering/Vulkan/UI/VulkanBackend.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

// stb_image implementation is emitted by FluentUI's Renderer.cpp (FluentUI
// is always linked into PokeMotorVk for the UI backend). Defining
// STB_IMAGE_IMPLEMENTATION here would duplicate the symbols at link time.
#include <stb_image.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace pokemotor::vk {

namespace {

struct CameraUBO {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 cameraPos;       // .xyz
    glm::vec4 lightDirection;  // .xyz (world-space, points away from light)
    glm::vec4 lightColor;      // .xyz
    glm::vec4 ambient;         // .xyz
    glm::mat4 lightSpaceMatrix[kCascadeCount];
    glm::vec4 cascadeSplits;   // .xyz = view-distance thresholds, .w = unused
    glm::mat4 prevViewProj;
    glm::vec4 jitter;          // .xy = current frame, .zw = previous frame
};

// Vertex layout for the debug-line overlay (position + per-vertex color).
struct DebugLineVertex {
    glm::vec3 pos;
    glm::vec3 color;
};
static_assert(sizeof(DebugLineVertex) == 24, "DebugLineVertex must be 24 bytes");

// Shared by every graphics pipeline — no blending, just write RGBA.
inline VkPipelineColorBlendAttachmentState makeOpaqueBlend() {
    VkPipelineColorBlendAttachmentState a{};
    a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    return a;
}

// Halton low-discrepancy sequence used for sub-pixel TAA jitter.
inline float haltonAt(uint32_t index, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= float(base);
        r += f * float(index % base);
        index /= base;
    }
    return r;
}

// Fixed cascade splits in world units. Frustum-fit + perspective-aware
// splits land later — these constants are good enough for the deferred demo.
// Near-weighted (pseudo-logarithmic) splits: more shadow-map resolution close
// to the camera where it matters, larger last cascade for distant geometry.
constexpr float kCascadeFarSplits[kCascadeCount] = { 6.0f, 20.0f, 80.0f };

struct PushConstants {
    glm::mat4 model;                       // offset 0   — vertex stage
    glm::vec4 baseColorFactor;             // offset 64  — fragment stage (vec4-aligned)
    uint32_t  materialIndex;               // offset 80  — fragment stage (baseColor bindless slot)
    uint32_t  metallicRoughnessIndex;      // offset 84  — fragment stage (MR bindless slot, 0 = fallback)
    float     metallicFactor;              // offset 88  — fragment stage
    float     roughnessFactor;             // offset 92  — fragment stage
    float     specularAA;                  // offset 96  — fragment stage (1 = geometric specular AA on)
};
// GLM is not force-aligned in this project (only DEPTH_ZERO_TO_ONE/RADIANS are
// set), so the struct packs to its scalar 4-byte alignment: 100 bytes, matching
// the std430 push_constant offsets. Well under the 128-byte guaranteed minimum.
static_assert(sizeof(PushConstants) == 100, "PushConstants size must match GLSL layout");

constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

std::vector<char> readBinaryFile(const char* path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) return {};
    size_t size = static_cast<size_t>(f.tellg());
    std::vector<char> buf(size);
    f.seekg(0);
    f.read(buf.data(), size);
    return buf;
}

// RAII one-shot command buffer for init-time upload/transition work.
// Owns its own transient pool, submits to graphics queue, waits idle, frees.
struct OneShotCmd {
    OneShotCmd(VkDevice dev, uint32_t queueFamily, VkQueue queue)
        : m_device(dev), m_queue(queue) {
        VkCommandPoolCreateInfo pi{};
        pi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = queueFamily;
        vkCreateCommandPool(dev, &pi, nullptr, &m_pool);

        VkCommandBufferAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool        = m_pool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &ai, &m_cmd);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_cmd, &bi);
    }

    ~OneShotCmd() {
        vkEndCommandBuffer(m_cmd);
        VkSubmitInfo s{};
        s.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        s.commandBufferCount = 1;
        s.pCommandBuffers    = &m_cmd;
        vkQueueSubmit(m_queue, 1, &s, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_queue);
        vkDestroyCommandPool(m_device, m_pool, nullptr);
    }

    VkCommandBuffer cmd() const { return m_cmd; }

    OneShotCmd(const OneShotCmd&) = delete;
    OneShotCmd& operator=(const OneShotCmd&) = delete;

private:
    VkDevice        m_device;
    VkQueue         m_queue;
    VkCommandPool   m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd  = VK_NULL_HANDLE;
};

}  // namespace

bool VulkanContext::Initialize(SDL_Window* window, bool enableValidation) {
    if (!m_instance.Initialize("PokeMotorVk", enableValidation)) return false;
    if (!createSurface(window)) return false;
    if (!m_device.Initialize(m_instance, m_surface)) return false;
    if (!m_allocator.Initialize(m_instance, m_device)) return false;

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!m_swapchain.Initialize(m_device, m_surface,
                                static_cast<uint32_t>(w),
                                static_cast<uint32_t>(h))) return false;

    if (!createDepthResources())                                  return false;
    if (!createGBuffer())                                         return false;
    if (!createHiZResources())                                    return false;
    if (!createSSRResources())                                    return false;
    if (!createShadowResources())                                 return false;
    if (!createIBLResources())                                    return false;
    if (!createTAAResources())                                    return false;
    if (!createPostResources())                                   return false;
    if (!createBloomResources())                                  return false;
    if (!createVolFogResources())                                 return false;
    if (!createParticleResources())                               return false;
    if (!createSpriteResources())                                 return false;
    if (!createLightResources())                                  return false;  // before G-Buffer descriptors
    if (!createSampler())                                         return false;
    if (!createGBufferSampler())                                  return false;
    if (!createFallbackTexture())                                 return false;
    if (!loadModel("Assets/Models/Demo/CoffeeCart_01_2k.gltf"))   return false;

    loadPipelineCache();
    if (!createDescriptors())          return false;
    if (!createIBLPipelines())         return false;
    if (!precomputeIBL())              return false;
    // The equirect HDRI was only needed to bake the env cubemap (precomputeIBL
    // submitted + waited), so free its ~128 MB now. Destroy() is idempotent, so
    // the one in destroyIBLResources at shutdown is harmless.
    m_envEquirect.Destroy();
    if (!createGBufferDescriptors())   return false;
    if (!createGeometryPipeline())     return false;
    if (!createLightingPipeline())     return false;
    if (!createHiZPipeline())          return false;
    if (!createShadowPipeline())       return false;
    if (!createTAAPipeline())          return false;
    if (!createTAADescriptors())       return false;
    if (!createSSRPipelines())         return false;
    if (!createSSRDescriptors())       return false;
    if (!createBloomPipelines())       return false;
    if (!createBloomDescriptors())     return false;
    if (!createVolFogPipeline())       return false;
    if (!createVolFogDescriptors())    return false;
    if (!createParticlePipelines())    return false;
    if (!createParticleDescriptors())  return false;
    if (!createSpritePipeline())       return false;
    if (!createSpriteShadowPipeline()) return false;  // needs m_spriteSsboSetLayout + m_bindlessSetLayout
    if (!createSpriteDescriptors())    return false;
    if (!createAutoExposureResources())   return false;  // m_exposureBuffer before tonemap descriptors
    if (!createAutoExposurePipelines())   return false;
    if (!createTonemapPipeline())      return false;
    if (!createTonemapDescriptors())   return false;
    if (!createCASPipeline())          return false;
    if (!createCASDescriptors())       return false;
    if (!createDebugLineResources())   return false;
    if (!createDebugLinePipeline())    return false;
    if (!createDebugTriPipeline())     return false;
    if (!createAutoExposureDescriptors()) return false;
    if (!createPerFrameResources())    return false;
    if (!createPerImageSemaphores())   return false;
    return true;
}

FluentUI::VulkanSharedContext VulkanContext::GetUISharedContext() const {
    FluentUI::VulkanSharedContext shared{};
    shared.instance         = m_instance.Handle();
    shared.physicalDevice   = m_device.PhysicalHandle();
    shared.device           = m_device.Handle();
    shared.graphicsQueue    = m_device.GraphicsQueue();
    shared.queueFamilyIndex = m_device.GraphicsQueueFamily();
    // The UI pass renders with vkCmdBeginRendering (no VkRenderPass object), so
    // FluentUI must build its pipelines with VkPipelineRenderingCreateInfo.
    shared.dynamicRendering = true;
    shared.renderPass       = 0;
    shared.colorFormat      = static_cast<uint32_t>(m_swapchain.ImageFormat());
    shared.sampleCount      = 1;   // UI pass targets the single-sample swapchain
    shared.depthFormat      = 0;   // UI binds no depth/stencil attachment
    shared.stencilFormat    = 0;
    return shared;
}

void VulkanContext::Shutdown() {
    if (m_device.Handle() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device.Handle());
    }

    savePipelineCache();

    destroyPerImageSemaphores();

    for (auto& f : m_frames) {
        f.ubo.Destroy();
        if (f.pool)            vkDestroyCommandPool(m_device.Handle(), f.pool, nullptr);
        if (f.imageAvailable)  vkDestroySemaphore(m_device.Handle(), f.imageAvailable, nullptr);
        if (f.inFlight)        vkDestroyFence(m_device.Handle(), f.inFlight, nullptr);
        f = {};
    }

    if (m_descriptorPool)      vkDestroyDescriptorPool(m_device.Handle(), m_descriptorPool, nullptr);
    if (m_frameSetLayout)      vkDestroyDescriptorSetLayout(m_device.Handle(), m_frameSetLayout, nullptr);
    if (m_bindlessSetLayout)   vkDestroyDescriptorSetLayout(m_device.Handle(), m_bindlessSetLayout, nullptr);
    if (m_gbufferSetLayout)    vkDestroyDescriptorSetLayout(m_device.Handle(), m_gbufferSetLayout, nullptr);
    if (m_hizSetLayout)        vkDestroyDescriptorSetLayout(m_device.Handle(), m_hizSetLayout, nullptr);
    if (m_envSetLayout)        vkDestroyDescriptorSetLayout(m_device.Handle(), m_envSetLayout, nullptr);
    if (m_irradianceSetLayout) vkDestroyDescriptorSetLayout(m_device.Handle(), m_irradianceSetLayout, nullptr);
    if (m_prefilterSetLayout)  vkDestroyDescriptorSetLayout(m_device.Handle(), m_prefilterSetLayout, nullptr);
    if (m_brdfLutSetLayout)    vkDestroyDescriptorSetLayout(m_device.Handle(), m_brdfLutSetLayout, nullptr);
    if (m_taaSetLayout)        vkDestroyDescriptorSetLayout(m_device.Handle(), m_taaSetLayout, nullptr);
    if (m_ssrSortSetLayout)     vkDestroyDescriptorSetLayout(m_device.Handle(), m_ssrSortSetLayout, nullptr);
    if (m_ssrPrefixSetLayout)   vkDestroyDescriptorSetLayout(m_device.Handle(), m_ssrPrefixSetLayout, nullptr);
    if (m_ssrTraceSetLayout)    vkDestroyDescriptorSetLayout(m_device.Handle(), m_ssrTraceSetLayout, nullptr);
    if (m_ssrTemporalSetLayout) vkDestroyDescriptorSetLayout(m_device.Handle(), m_ssrTemporalSetLayout, nullptr);
    if (m_ssrSpatialSetLayout)  vkDestroyDescriptorSetLayout(m_device.Handle(), m_ssrSpatialSetLayout, nullptr);
    if (m_ssrUpsampleSetLayout) vkDestroyDescriptorSetLayout(m_device.Handle(), m_ssrUpsampleSetLayout, nullptr);
    if (m_tonemapSetLayout)          vkDestroyDescriptorSetLayout(m_device.Handle(), m_tonemapSetLayout, nullptr);
    if (m_casSetLayout)              vkDestroyDescriptorSetLayout(m_device.Handle(), m_casSetLayout, nullptr);
    if (m_histBuildSetLayout)        vkDestroyDescriptorSetLayout(m_device.Handle(), m_histBuildSetLayout, nullptr);
    if (m_histAvgSetLayout)          vkDestroyDescriptorSetLayout(m_device.Handle(), m_histAvgSetLayout, nullptr);
    if (m_bloomPrefilterSetLayout)   vkDestroyDescriptorSetLayout(m_device.Handle(), m_bloomPrefilterSetLayout, nullptr);
    if (m_bloomDownsampleSetLayout)  vkDestroyDescriptorSetLayout(m_device.Handle(), m_bloomDownsampleSetLayout, nullptr);
    if (m_bloomUpsampleSetLayout)    vkDestroyDescriptorSetLayout(m_device.Handle(), m_bloomUpsampleSetLayout, nullptr);
    if (m_volFogSetLayout)           vkDestroyDescriptorSetLayout(m_device.Handle(), m_volFogSetLayout, nullptr);
    if (m_particleSsboSetLayout)     vkDestroyDescriptorSetLayout(m_device.Handle(), m_particleSsboSetLayout, nullptr);
    if (m_spriteSsboSetLayout)       vkDestroyDescriptorSetLayout(m_device.Handle(), m_spriteSsboSetLayout, nullptr);

    if (m_geometryPipeline)         vkDestroyPipeline(m_device.Handle(), m_geometryPipeline, nullptr);
    if (m_lightingPipeline)         vkDestroyPipeline(m_device.Handle(), m_lightingPipeline, nullptr);
    if (m_hizPipeline)              vkDestroyPipeline(m_device.Handle(), m_hizPipeline, nullptr);
    if (m_hizDownsamplePipeline)    vkDestroyPipeline(m_device.Handle(), m_hizDownsamplePipeline, nullptr);
    if (m_shadowPipeline)           vkDestroyPipeline(m_device.Handle(), m_shadowPipeline, nullptr);
    if (m_envPipeline)              vkDestroyPipeline(m_device.Handle(), m_envPipeline, nullptr);
    if (m_irradiancePipeline)       vkDestroyPipeline(m_device.Handle(), m_irradiancePipeline, nullptr);
    if (m_prefilterPipeline)        vkDestroyPipeline(m_device.Handle(), m_prefilterPipeline, nullptr);
    if (m_brdfLutPipeline)          vkDestroyPipeline(m_device.Handle(), m_brdfLutPipeline, nullptr);
    if (m_taaPipeline)              vkDestroyPipeline(m_device.Handle(), m_taaPipeline, nullptr);
    if (m_ssrSortPipeline)          vkDestroyPipeline(m_device.Handle(), m_ssrSortPipeline, nullptr);
    if (m_ssrPrefixPipeline)        vkDestroyPipeline(m_device.Handle(), m_ssrPrefixPipeline, nullptr);
    if (m_ssrTracePipeline)         vkDestroyPipeline(m_device.Handle(), m_ssrTracePipeline, nullptr);
    if (m_ssrTemporalPipeline)      vkDestroyPipeline(m_device.Handle(), m_ssrTemporalPipeline, nullptr);
    if (m_ssrSpatialPipeline)       vkDestroyPipeline(m_device.Handle(), m_ssrSpatialPipeline, nullptr);
    if (m_ssrUpsamplePipeline)      vkDestroyPipeline(m_device.Handle(), m_ssrUpsamplePipeline, nullptr);
    if (m_tonemapPipeline)          vkDestroyPipeline(m_device.Handle(), m_tonemapPipeline, nullptr);
    if (m_casPipeline)              vkDestroyPipeline(m_device.Handle(), m_casPipeline, nullptr);
    if (m_debugLinePipeline)        vkDestroyPipeline(m_device.Handle(), m_debugLinePipeline, nullptr);
    if (m_debugTriPipeline)         vkDestroyPipeline(m_device.Handle(), m_debugTriPipeline, nullptr);
    if (m_histBuildPipeline)        vkDestroyPipeline(m_device.Handle(), m_histBuildPipeline, nullptr);
    if (m_histAvgPipeline)          vkDestroyPipeline(m_device.Handle(), m_histAvgPipeline, nullptr);
    if (m_bloomPrefilterPipeline)   vkDestroyPipeline(m_device.Handle(), m_bloomPrefilterPipeline, nullptr);
    if (m_bloomDownsamplePipeline)  vkDestroyPipeline(m_device.Handle(), m_bloomDownsamplePipeline, nullptr);
    if (m_bloomUpsamplePipeline)    vkDestroyPipeline(m_device.Handle(), m_bloomUpsamplePipeline, nullptr);
    if (m_volFogPipeline)           vkDestroyPipeline(m_device.Handle(), m_volFogPipeline, nullptr);
    if (m_particleUpdatePipeline)   vkDestroyPipeline(m_device.Handle(), m_particleUpdatePipeline, nullptr);
    if (m_particleRenderPipeline)   vkDestroyPipeline(m_device.Handle(), m_particleRenderPipeline, nullptr);
    if (m_spritePipeline)           vkDestroyPipeline(m_device.Handle(), m_spritePipeline, nullptr);
    if (m_spriteShadowPipeline)     vkDestroyPipeline(m_device.Handle(), m_spriteShadowPipeline, nullptr);
    if (m_pipelineLayout)           vkDestroyPipelineLayout(m_device.Handle(), m_pipelineLayout, nullptr);
    if (m_hizPipelineLayout)        vkDestroyPipelineLayout(m_device.Handle(), m_hizPipelineLayout, nullptr);
    if (m_shadowPipelineLayout)     vkDestroyPipelineLayout(m_device.Handle(), m_shadowPipelineLayout, nullptr);
    if (m_envPipelineLayout)        vkDestroyPipelineLayout(m_device.Handle(), m_envPipelineLayout, nullptr);
    if (m_irradiancePipelineLayout) vkDestroyPipelineLayout(m_device.Handle(), m_irradiancePipelineLayout, nullptr);
    if (m_prefilterPipelineLayout)  vkDestroyPipelineLayout(m_device.Handle(), m_prefilterPipelineLayout, nullptr);
    if (m_brdfLutPipelineLayout)    vkDestroyPipelineLayout(m_device.Handle(), m_brdfLutPipelineLayout, nullptr);
    if (m_taaPipelineLayout)        vkDestroyPipelineLayout(m_device.Handle(), m_taaPipelineLayout, nullptr);
    if (m_ssrSortPipelineLayout)     vkDestroyPipelineLayout(m_device.Handle(), m_ssrSortPipelineLayout, nullptr);
    if (m_ssrPrefixPipelineLayout)   vkDestroyPipelineLayout(m_device.Handle(), m_ssrPrefixPipelineLayout, nullptr);
    if (m_ssrTracePipelineLayout)    vkDestroyPipelineLayout(m_device.Handle(), m_ssrTracePipelineLayout, nullptr);
    if (m_ssrTemporalPipelineLayout) vkDestroyPipelineLayout(m_device.Handle(), m_ssrTemporalPipelineLayout, nullptr);
    if (m_ssrSpatialPipelineLayout)  vkDestroyPipelineLayout(m_device.Handle(), m_ssrSpatialPipelineLayout, nullptr);
    if (m_ssrUpsamplePipelineLayout) vkDestroyPipelineLayout(m_device.Handle(), m_ssrUpsamplePipelineLayout, nullptr);
    if (m_tonemapPipelineLayout)          vkDestroyPipelineLayout(m_device.Handle(), m_tonemapPipelineLayout, nullptr);
    if (m_casPipelineLayout)              vkDestroyPipelineLayout(m_device.Handle(), m_casPipelineLayout, nullptr);
    if (m_debugLinePipelineLayout)        vkDestroyPipelineLayout(m_device.Handle(), m_debugLinePipelineLayout, nullptr);
    if (m_histBuildPipelineLayout)        vkDestroyPipelineLayout(m_device.Handle(), m_histBuildPipelineLayout, nullptr);
    if (m_histAvgPipelineLayout)          vkDestroyPipelineLayout(m_device.Handle(), m_histAvgPipelineLayout, nullptr);
    if (m_bloomPrefilterPipelineLayout)   vkDestroyPipelineLayout(m_device.Handle(), m_bloomPrefilterPipelineLayout, nullptr);
    if (m_bloomDownsamplePipelineLayout)  vkDestroyPipelineLayout(m_device.Handle(), m_bloomDownsamplePipelineLayout, nullptr);
    if (m_bloomUpsamplePipelineLayout)    vkDestroyPipelineLayout(m_device.Handle(), m_bloomUpsamplePipelineLayout, nullptr);
    if (m_volFogPipelineLayout)           vkDestroyPipelineLayout(m_device.Handle(), m_volFogPipelineLayout, nullptr);
    if (m_particleUpdatePipelineLayout)   vkDestroyPipelineLayout(m_device.Handle(), m_particleUpdatePipelineLayout, nullptr);
    if (m_particleRenderPipelineLayout)   vkDestroyPipelineLayout(m_device.Handle(), m_particleRenderPipelineLayout, nullptr);
    if (m_spritePipelineLayout)           vkDestroyPipelineLayout(m_device.Handle(), m_spritePipelineLayout, nullptr);
    if (m_spriteShadowPipelineLayout)     vkDestroyPipelineLayout(m_device.Handle(), m_spriteShadowPipelineLayout, nullptr);
    if (m_pipelineCache)            vkDestroyPipelineCache(m_device.Handle(), m_pipelineCache, nullptr);

    for (auto& mesh : m_meshes) mesh.Destroy();
    m_meshes.clear();

    for (auto& tex : m_textures) tex.Destroy();
    m_textures.clear();
    m_bindlessSet = VK_NULL_HANDLE;
    for (auto& s : m_gbufferSet) s = VK_NULL_HANDLE;  // sets freed with the pool; just null the handles
    m_texturePathToIndex.clear();

    if (m_sampler)        vkDestroySampler(m_device.Handle(), m_sampler, nullptr);
    if (m_gbufferSampler) vkDestroySampler(m_device.Handle(), m_gbufferSampler, nullptr);
    if (m_depthSampler)   vkDestroySampler(m_device.Handle(), m_depthSampler, nullptr);
    if (m_hiZSampler)     vkDestroySampler(m_device.Handle(), m_hiZSampler, nullptr);
    if (m_shadowSampler)  vkDestroySampler(m_device.Handle(), m_shadowSampler, nullptr);
    if (m_shadowDepthSampler) vkDestroySampler(m_device.Handle(), m_shadowDepthSampler, nullptr);
    if (m_iblSampler)     vkDestroySampler(m_device.Handle(), m_iblSampler, nullptr);
    if (m_taaSampler)     vkDestroySampler(m_device.Handle(), m_taaSampler, nullptr);
    // m_uiBackend is owned by FluentUI::Context — destroyed by its
    // DestroyContext() call from the host main before ctx.Shutdown().
    m_uiBackend = nullptr;
    destroySpriteResources();
    destroyDebugLineResources();
    destroyParticleResources();
    destroyLightResources();
    destroyVolFogResources();
    destroyBloomResources();
    destroyPostResources();
    // Auto-exposure SSBOs are resolution-independent (not in recreateSwapchain),
    // so they are destroyed once here.
    m_histogramBuffer.Destroy();
    m_exposureBuffer.Destroy();
    destroySSRResources();
    destroyTAAResources();
    destroyIBLResources();
    destroyShadowResources();
    destroyHiZResources();
    destroyGBuffer();
    destroyDepthResources();

    m_swapchain.Shutdown(m_device);
    m_allocator.Shutdown();

    if (m_surface && m_instance.Handle()) {
        vkDestroySurfaceKHR(m_instance.Handle(), m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    m_device.Shutdown();
    m_instance.Shutdown();
}

void VulkanContext::WaitIdle() {
    if (m_device.Handle()) vkDeviceWaitIdle(m_device.Handle());
}

float VulkanContext::AspectRatio() const {
    auto e = m_swapchain.Extent();
    if (e.height == 0) return 1.0f;
    return static_cast<float>(e.width) / static_cast<float>(e.height);
}

bool VulkanContext::MeshAABB(uint32_t i, glm::vec3& outMin, glm::vec3& outMax) const {
    if (i >= m_meshes.size()) return false;
    glm::vec3 lo, hi;
    if (!m_meshes[i].LocalAABB(lo, hi)) return false;
    const glm::mat4& lt = m_meshes[i].LocalTransform();
    glm::vec3 mn( std::numeric_limits<float>::max());
    glm::vec3 mx(-std::numeric_limits<float>::max());
    for (int c = 0; c < 8; ++c) {
        glm::vec3 corner((c & 1) ? hi.x : lo.x,
                         (c & 2) ? hi.y : lo.y,
                         (c & 4) ? hi.z : lo.z);
        glm::vec3 w = glm::vec3(lt * glm::vec4(corner, 1.0f));
        mn = glm::min(mn, w); mx = glm::max(mx, w);
    }
    outMin = mn; outMax = mx;
    return true;
}

bool VulkanContext::RaycastMesh(uint32_t i, const glm::mat4& world,
                                const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                                float& tHit) const {
    if (i >= m_meshes.size()) return false;
    return m_meshes[i].Raycast(world, rayOrigin, rayDir, tHit);
}

bool VulkanContext::createSurface(SDL_Window* window) {
    if (!SDL_Vulkan_CreateSurface(window, m_instance.Handle(), nullptr, &m_surface)) {
        std::fprintf(stderr, "[Vulkan] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

bool VulkanContext::createDepthResources() {
    ImageCreateParams p{};
    p.width  = m_swapchain.Extent().width;
    p.height = m_swapchain.Extent().height;
    p.format = kDepthFormat;
    // SAMPLED so the Hi-Z compute pass can read depth as sampler2D.
    p.usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    p.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    return m_depth.Create(m_device.Handle(), m_allocator.Handle(), p);
}

void VulkanContext::destroyDepthResources() {
    m_depth.Destroy();
}

bool VulkanContext::createGBuffer() {
    const uint32_t w = m_swapchain.Extent().width;
    const uint32_t h = m_swapchain.Extent().height;

    auto make = [&](ImageVk& out, VkFormat format) {
        ImageCreateParams p{};
        p.width  = w;
        p.height = h;
        p.format = format;
        // SAMPLED so the lighting pass can read it; TRANSFER_SRC reserved for
        // future blit/copy uses (HiZ, debug viewers).
        p.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                 | VK_IMAGE_USAGE_SAMPLED_BIT;
        p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        return out.Create(m_device.Handle(), m_allocator.Handle(), p);
    };

    if (!make(m_gPosition, VK_FORMAT_R16G16B16A16_SFLOAT)) return false;
    if (!make(m_gNormal,   VK_FORMAT_R16G16B16A16_SFLOAT)) return false;
    if (!make(m_gAlbedo,   VK_FORMAT_R8G8B8A8_UNORM))      return false;
    if (!make(m_gEmissive, VK_FORMAT_R16G16B16A16_SFLOAT)) return false;
    if (!make(m_gMotion,   VK_FORMAT_R16G16_SFLOAT))       return false;
    return true;
}

void VulkanContext::destroyGBuffer() {
    m_gPosition.Destroy();
    m_gNormal.Destroy();
    m_gAlbedo.Destroy();
    m_gEmissive.Destroy();
    m_gMotion.Destroy();
}

bool VulkanContext::createGBufferSampler() {
    // Point + clamp-to-edge — we sample exact texels, no filtering needed since
    // the G-Buffer is screen-resolution and the lighting pass uses fullscreen UVs.
    VkSamplerCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter    = VK_FILTER_NEAREST;
    info.minFilter    = VK_FILTER_NEAREST;
    info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_device.Handle(), &info, nullptr, &m_gbufferSampler) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] vkCreateSampler (G-Buffer) failed\n");
        return false;
    }
    return true;
}

bool VulkanContext::createHiZResources() {
    const uint32_t w = m_swapchain.Extent().width;
    const uint32_t h = m_swapchain.Extent().height;

    // Mip count: floor(log2(max(w,h))) + 1. Each mip halves the dimensions
    // (clamped to 1). SSR uses the chain to take large early steps where
    // geometry coverage allows.
    uint32_t maxDim = (w > h) ? w : h;
    uint32_t mips = 1u;
    while ((maxDim >> mips) > 0u) ++mips;
    // Clamp to the max for which descriptor sets were pre-allocated. Going
    // past 16 would happen at 32K+ resolutions; we'd waste a couple low
    // mips of SSR coverage there, never an issue in practice.
    if (mips > 16u) mips = 16u;
    m_hiZMipCount = mips;

    ImageCreateParams p{};
    p.width     = w;
    p.height    = h;
    p.format    = VK_FORMAT_R32_SFLOAT;
    p.usage     = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    p.aspect    = VK_IMAGE_ASPECT_COLOR_BIT;
    p.mipLevels = m_hiZMipCount;
    if (!m_hiZ.Create(m_device.Handle(), m_allocator.Handle(), p)) return false;

    // Per-mip single-level storage views. The image's default view already
    // covers all mips for SSR sampling (textureLod); these per-mip views are
    // bound as `writeonly image2D` targets by the seed and downsample shaders.
    m_hiZMipViews.assign(m_hiZMipCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < m_hiZMipCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image    = m_hiZ.Handle();
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format   = VK_FORMAT_R32_SFLOAT;
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel   = i;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(m_device.Handle(), &viewInfo, nullptr,
                              &m_hiZMipViews[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] Hi-Z per-mip view %u failed\n", i);
            return false;
        }
    }

    // Sampler for reading depth (mip 0 of m_depth). NEAREST is mandatory —
    // we treat each texel as the depth at that pixel.
    if (m_depthSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter    = VK_FILTER_NEAREST;
        info.minFilter    = VK_FILTER_NEAREST;
        info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_device.Handle(), &info, nullptr, &m_depthSampler) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] depth sampler create failed\n");
            return false;
        }
    }

    // Sampler for the Hi-Z chain itself: LINEAR within-mip filtering keeps
    // the per-step textureLod fetch smooth without crossing mip levels (the
    // mipLevel is explicit in the SSR trace shader). NEAREST mipmap mode →
    // no inter-mip blending.
    if (m_hiZSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter    = VK_FILTER_LINEAR;
        info.minFilter    = VK_FILTER_LINEAR;
        info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.minLod       = 0.0f;
        info.maxLod       = static_cast<float>(m_hiZMipCount);
        if (vkCreateSampler(m_device.Handle(), &info, nullptr, &m_hiZSampler) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] Hi-Z sampler create failed\n");
            return false;
        }
    }
    return true;
}

void VulkanContext::destroyHiZResources() {
    for (VkImageView v : m_hiZMipViews) {
        if (v) vkDestroyImageView(m_device.Handle(), v, nullptr);
    }
    m_hiZMipViews.clear();
    m_hiZMipCount = 0;
    m_hiZ.Destroy();
}

bool VulkanContext::createHiZPipeline() {
    // ---- Set layout: combined image sampler + storage image -------------
    // Reused for both the seed (depth → mip 0) and each downsample
    // (mip N → mip N+1). Different VkDescriptorSet instances, same layout.
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(m_device.Handle(), &layoutInfo, nullptr,
                                    &m_hizSetLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] Hi-Z DescriptorSetLayout failed\n");
        return false;
    }

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts    = &m_hizSetLayout;
    if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                               &m_hizPipelineLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] Hi-Z pipelineLayout failed\n");
        return false;
    }

    // ---- Seed pipeline (hiz_generate.comp) ------------------------------
    {
        VkShaderModule cs = loadShaderModule("Shaders/SPV/hiz_generate.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = cs;
        stage.pName  = "main";
        VkComputePipelineCreateInfo info{};
        info.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage  = stage;
        info.layout = m_hizPipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                              &info, nullptr, &m_hizPipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] Hi-Z seed pipeline create failed: %d\n", r);
            return false;
        }
    }

    // ---- Downsample pipeline (hiz_downsample.comp) ---------------------
    {
        VkShaderModule cs = loadShaderModule("Shaders/SPV/hiz_downsample.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = cs;
        stage.pName  = "main";
        VkComputePipelineCreateInfo info{};
        info.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.stage  = stage;
        info.layout = m_hizPipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                              &info, nullptr, &m_hizDownsamplePipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] Hi-Z downsample pipeline create failed: %d\n", r);
            return false;
        }
    }

    // ---- Descriptor sets ----------------------------------------------
    // Allocate (kMaxHiZMips - 1) downsample sets so a future window resize
    // that bumps mipCount won't index past the array. Only the first
    // (m_hiZMipCount - 1) get written below — the resize path
    // (recreateSwapchain) re-writes the relevant range each time.
    constexpr uint32_t kMaxHiZMipsLocal = 16;
    const uint32_t allocCount = kMaxHiZMipsLocal - 1u;
    const uint32_t downCount  = (m_hiZMipCount > 0u) ? (m_hiZMipCount - 1u) : 0u;
    std::vector<VkDescriptorSetLayout> layouts(1 + allocCount, m_hizSetLayout);

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = m_descriptorPool;
    alloc.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    alloc.pSetLayouts        = layouts.data();

    std::vector<VkDescriptorSet> sets(layouts.size(), VK_NULL_HANDLE);
    if (vkAllocateDescriptorSets(m_device.Handle(), &alloc, sets.data()) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] alloc Hi-Z descriptor sets failed (count=%zu)\n",
                     layouts.size());
        return false;
    }
    m_hizSet = sets[0];
    m_hizDownsampleSets.assign(sets.begin() + 1, sets.end());

    // Seed set: reads depth (sampler), writes mip 0 of hiZ.
    {
        VkDescriptorImageInfo depthInfo{};
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depthInfo.imageView   = m_depth.View();
        depthInfo.sampler     = m_depthSampler;

        VkDescriptorImageInfo hizInfo{};
        hizInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        hizInfo.imageView   = m_hiZMipViews[0];

        VkWriteDescriptorSet w[2]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = m_hizSet;
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &depthInfo;
        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[1].dstSet = m_hizSet;
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &hizInfo;
        vkUpdateDescriptorSets(m_device.Handle(), 2, w, 0, nullptr);
    }

    // Downsample sets: [i] reads mip i (sampler), writes mip i+1 (storage).
    // Per-write VkDescriptorImageInfo must outlive the vkUpdateDescriptorSets
    // call → flatten into vectors first.
    std::vector<VkDescriptorImageInfo> srcInfos(downCount);
    std::vector<VkDescriptorImageInfo> dstInfos(downCount);
    std::vector<VkWriteDescriptorSet>  dsWrites(downCount * 2);
    for (uint32_t i = 0; i < downCount; ++i) {
        srcInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;  // we keep the chain in GENERAL
        srcInfos[i].imageView   = m_hiZMipViews[i];
        srcInfos[i].sampler     = m_depthSampler;           // texelFetch ignores sampler config

        dstInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        dstInfos[i].imageView   = m_hiZMipViews[i + 1];

        dsWrites[i * 2 + 0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        dsWrites[i * 2 + 0].dstSet = m_hizDownsampleSets[i];
        dsWrites[i * 2 + 0].dstBinding = 0;
        dsWrites[i * 2 + 0].descriptorCount = 1;
        dsWrites[i * 2 + 0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        dsWrites[i * 2 + 0].pImageInfo = &srcInfos[i];

        dsWrites[i * 2 + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        dsWrites[i * 2 + 1].dstSet = m_hizDownsampleSets[i];
        dsWrites[i * 2 + 1].dstBinding = 1;
        dsWrites[i * 2 + 1].descriptorCount = 1;
        dsWrites[i * 2 + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        dsWrites[i * 2 + 1].pImageInfo = &dstInfos[i];
    }
    if (!dsWrites.empty()) {
        vkUpdateDescriptorSets(m_device.Handle(),
                               static_cast<uint32_t>(dsWrites.size()),
                               dsWrites.data(), 0, nullptr);
    }
    return true;
}

// ===== SSR (FASE 13) =====================================================

// Push constant payloads. Sizes match the GLSL push_constant blocks. Vulkan
// guarantees ≥128 B; both fit (32 and 176 respectively, well under 256).
namespace {
struct SSRSortPC {
    glm::vec4 cameraPos;  // .xyz, .w padding
    int32_t   width;
    int32_t   height;
    int32_t   mode;       // 0 = count, 1 = scatter
    int32_t   _pad0;
};
static_assert(sizeof(SSRSortPC) == 32, "SSRSortPC must match GLSL layout");

struct SSRTracePC {
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 cameraPos;
    int32_t   width;
    int32_t   height;
    int32_t   hiZMipLevels;
    float     maxDistance;
    float     thickness;
    int32_t   frameId;
    int32_t   fullWidth;
    int32_t   fullHeight;
};
static_assert(sizeof(SSRTracePC) == 176, "SSRTracePC must match GLSL layout");

struct SSRUpsamplePC {
    int32_t fullWidth;
    int32_t fullHeight;
    int32_t halfWidth;
    int32_t halfHeight;
};
static_assert(sizeof(SSRUpsamplePC) == 16, "SSRUpsamplePC must match GLSL layout");

struct HistBuildPC {
    int32_t width;
    int32_t height;
    float   logMin;
    float   invLogRange;
};
static_assert(sizeof(HistBuildPC) == 16, "HistBuildPC must match GLSL");

struct HistAvgPC {
    float    logMin;
    float    logRange;
    float    dt;
    float    tau;
    uint32_t pixelCount;
    float    minLum;
    float    maxLum;
    float    _pad;
};
static_assert(sizeof(HistAvgPC) == 32, "HistAvgPC must match GLSL");
}  // namespace

bool VulkanContext::createSSRResources() {
    const uint32_t w = m_swapchain.Extent().width;
    const uint32_t h = m_swapchain.Extent().height;

    // ---- Output images: raw trace + SVGF history/scratch ----------------
    auto makeStorageSampled = [&](ImageVk& out) {
        ImageCreateParams p{};
        p.width  = w;
        p.height = h;
        p.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        p.usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        return out.Create(m_device.Handle(), m_allocator.Handle(), p);
    };

    if (!makeStorageSampled(m_ssrResult))     return false;
    if (!makeStorageSampled(m_ssrColor[0]))   return false;
    if (!makeStorageSampled(m_ssrColor[1]))   return false;
    if (!makeStorageSampled(m_ssrColorTemp))  return false;
    if (!makeStorageSampled(m_ssrMoments[0])) return false;
    if (!makeStorageSampled(m_ssrMoments[1])) return false;

    // Half-res reflection target written by the packet trace when half-res SSR
    // is enabled; reconstructed back to m_ssrResult by the upsample pass.
    {
        ImageCreateParams p{};
        p.width  = (w + 1) / 2;
        p.height = (h + 1) / 2;
        p.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        p.usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        if (!m_ssrResultHalf.Create(m_device.Handle(), m_allocator.Handle(), p)) return false;
    }

    // First frame after (re)creation has no valid SVGF history — the
    // temporal pass uses the "historyValid=0" branch for one frame.
    m_svgfFrameCounter = 0;

    // ---- SSBOs ----------------------------------------------------------
    const VmaAllocator alloc = m_allocator.Handle();

    if (!m_ssrRayCount.CreateStorage(alloc, sizeof(uint32_t))) return false;
    if (!m_ssrBinCounters.CreateStorage(alloc, sizeof(uint32_t) * kSSRBins)) return false;
    if (!m_ssrBinOffsets.CreateStorage(alloc, sizeof(uint32_t) * kSSRBins)) return false;
    if (!m_ssrScatterCounters.CreateStorage(alloc, sizeof(uint32_t) * kSSRBins)) return false;
    // Worst case: every pixel produces a ray. Reserve sizeof(uint) × w × h.
    if (!m_ssrSortedRays.CreateStorage(alloc, sizeof(uint32_t) * w * h)) return false;
    // numGroupsX/Y/Z written by prefix shader → indirect dispatch source.
    if (!m_ssrIndirectArgs.CreateStorage(alloc, sizeof(uint32_t) * 3,
                                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)) return false;
    return true;
}

void VulkanContext::destroySSRResources() {
    m_ssrResult.Destroy();
    m_ssrResultHalf.Destroy();
    m_ssrColor[0].Destroy();
    m_ssrColor[1].Destroy();
    m_ssrColorTemp.Destroy();
    m_ssrMoments[0].Destroy();
    m_ssrMoments[1].Destroy();
    m_ssrRayCount.Destroy();
    m_ssrBinCounters.Destroy();
    m_ssrBinOffsets.Destroy();
    m_ssrScatterCounters.Destroy();
    m_ssrSortedRays.Destroy();
    m_ssrIndirectArgs.Destroy();
}

bool VulkanContext::createSSRPipelines() {
    // ---- Sort set layout (shared by sort count + scatter dispatches) ----
    // bindings 0..1 = sampler2D (gPosition, gNormal); 2..6 = SSBOs.
    {
        VkDescriptorSetLayoutBinding b[7]{};
        for (int i = 0; i < 7; ++i) {
            b[i].binding         = static_cast<uint32_t>(i);
            b[i].descriptorCount = 1;
            b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 7;
        info.pBindings    = b;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                        &m_ssrSortSetLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SSR sort set layout failed\n");
            return false;
        }
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset     = 0;
        pcRange.size       = sizeof(SSRSortPC);
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &m_ssrSortSetLayout;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &pcRange;
        if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                                   &m_ssrSortPipelineLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SSR sort pipeline layout failed\n");
            return false;
        }
        VkShaderModule cs = loadShaderModule("Shaders/SPV/ssr_ray_sort.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = cs;
        stage.pName  = "main";
        VkComputePipelineCreateInfo cpi{};
        cpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage  = stage;
        cpi.layout = m_ssrSortPipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                              &cpi, nullptr, &m_ssrSortPipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SSR sort pipeline failed: %d\n", r);
            return false;
        }
    }

    // ---- Prefix set layout (only SSBOs) --------------------------------
    {
        VkDescriptorSetLayoutBinding b[4]{};
        for (int i = 0; i < 4; ++i) {
            b[i].binding         = static_cast<uint32_t>(i);
            b[i].descriptorCount = 1;
            b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            b[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 4;
        info.pBindings    = b;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                        &m_ssrPrefixSetLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SSR prefix set layout failed\n");
            return false;
        }
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount = 1;
        plInfo.pSetLayouts    = &m_ssrPrefixSetLayout;
        if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                                   &m_ssrPrefixPipelineLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SSR prefix pipeline layout failed\n");
            return false;
        }
        VkShaderModule cs = loadShaderModule("Shaders/SPV/ssr_ray_prefix.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = cs;
        stage.pName  = "main";
        VkComputePipelineCreateInfo cpi{};
        cpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage  = stage;
        cpi.layout = m_ssrPrefixPipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                              &cpi, nullptr, &m_ssrPrefixPipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SSR prefix pipeline failed: %d\n", r);
            return false;
        }
    }

    // ---- Trace set layout: 5 samplers + 2 SSBOs + 1 storage image -------
    {
        VkDescriptorSetLayoutBinding b[8]{};
        for (int i = 0; i < 8; ++i) {
            b[i].binding         = static_cast<uint32_t>(i);
            b[i].descriptorCount = 1;
            b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        // 0..4 = gPosition, gNormal, gAlbedo, hiZ chain, prevFrame
        for (int i = 0; i < 5; ++i) b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        // 5..6 = sortedRays, totalRayCount
        b[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        // 7 = ssrResult storage image
        b[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 8;
        info.pBindings    = b;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                        &m_ssrTraceSetLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SSR trace set layout failed\n");
            return false;
        }
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset     = 0;
        pcRange.size       = sizeof(SSRTracePC);
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &m_ssrTraceSetLayout;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &pcRange;
        if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                                   &m_ssrTracePipelineLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SSR trace pipeline layout failed\n");
            return false;
        }
        VkShaderModule cs = loadShaderModule("Shaders/SPV/ssr_packet_trace.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = cs;
        stage.pName  = "main";
        VkComputePipelineCreateInfo cpi{};
        cpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage  = stage;
        cpi.layout = m_ssrTracePipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                              &cpi, nullptr, &m_ssrTracePipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SSR trace pipeline failed: %d\n", r);
            return false;
        }
    }

    // ---- Upsample set layout: 3 samplers + 1 storage image --------------
    // 0..2 = gPosition, gNormal, ssrResultHalf; 3 = ssrResult storage image.
    {
        VkDescriptorSetLayoutBinding b[4]{};
        for (int i = 0; i < 4; ++i) {
            b[i].binding         = static_cast<uint32_t>(i);
            b[i].descriptorCount = 1;
            b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 4;
        info.pBindings    = b;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                        &m_ssrUpsampleSetLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SSR upsample set layout failed\n");
            return false;
        }
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset     = 0;
        pcRange.size       = sizeof(SSRUpsamplePC);
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &m_ssrUpsampleSetLayout;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &pcRange;
        if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                                   &m_ssrUpsamplePipelineLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SSR upsample pipeline layout failed\n");
            return false;
        }
        VkShaderModule cs = loadShaderModule("Shaders/SPV/ssr_upsample.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = cs;
        stage.pName  = "main";
        VkComputePipelineCreateInfo cpi{};
        cpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage  = stage;
        cpi.layout = m_ssrUpsamplePipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                              &cpi, nullptr, &m_ssrUpsamplePipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SSR upsample pipeline failed: %d\n", r);
            return false;
        }
    }

    // ---- SVGF Temporal pipeline (F13.E) ---------------------------------
    {
        VkDescriptorSetLayoutBinding b[8]{};
        for (int i = 0; i < 8; ++i) {
            b[i].binding         = static_cast<uint32_t>(i);
            b[i].descriptorCount = 1;
            b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        for (int i = 0; i < 6; ++i) b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 8;
        info.pBindings    = b;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                        &m_ssrTemporalSetLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SVGF temporal set layout failed\n");
            return false;
        }

        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset     = 0;
        pcRange.size       = 16;  // 4 ints
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &m_ssrTemporalSetLayout;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &pcRange;
        if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                                   &m_ssrTemporalPipelineLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SVGF temporal pipeline layout failed\n");
            return false;
        }

        VkShaderModule cs = loadShaderModule("Shaders/SPV/ssr_svgf_temporal.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = cs;
        stage.pName  = "main";
        VkComputePipelineCreateInfo cpi{};
        cpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage  = stage;
        cpi.layout = m_ssrTemporalPipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                              &cpi, nullptr, &m_ssrTemporalPipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SVGF temporal pipeline failed: %d\n", r);
            return false;
        }
    }

    // ---- SVGF Spatial pipeline (F13.E) ----------------------------------
    {
        VkDescriptorSetLayoutBinding b[5]{};
        for (int i = 0; i < 5; ++i) {
            b[i].binding         = static_cast<uint32_t>(i);
            b[i].descriptorCount = 1;
            b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        for (int i = 0; i < 4; ++i) b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 5;
        info.pBindings    = b;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                        &m_ssrSpatialSetLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SVGF spatial set layout failed\n");
            return false;
        }

        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset     = 0;
        pcRange.size       = 32;  // 4 ints + 3 floats + 1 pad = 32 B
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &m_ssrSpatialSetLayout;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &pcRange;
        if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                                   &m_ssrSpatialPipelineLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SVGF spatial pipeline layout failed\n");
            return false;
        }

        VkShaderModule cs = loadShaderModule("Shaders/SPV/ssr_svgf_spatial.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = cs;
        stage.pName  = "main";
        VkComputePipelineCreateInfo cpi{};
        cpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage  = stage;
        cpi.layout = m_ssrSpatialPipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                              &cpi, nullptr, &m_ssrSpatialPipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] SVGF spatial pipeline failed: %d\n", r);
            return false;
        }
    }

    return true;
}

bool VulkanContext::createSSRDescriptors() {
    // 1 sort + 1 prefix + 2 trace (per TAA slot) + 2 temporal (per curSlot)
    // + 4 spatial ([curSlot][direction]) + 1 upsample.
    VkDescriptorSetLayout layouts[11] = {
        m_ssrSortSetLayout, m_ssrPrefixSetLayout,
        m_ssrTraceSetLayout, m_ssrTraceSetLayout,
        m_ssrTemporalSetLayout, m_ssrTemporalSetLayout,
        m_ssrSpatialSetLayout, m_ssrSpatialSetLayout,
        m_ssrSpatialSetLayout, m_ssrSpatialSetLayout,
        m_ssrUpsampleSetLayout,
    };
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = m_descriptorPool;
    alloc.descriptorSetCount = 11;
    alloc.pSetLayouts        = layouts;
    VkDescriptorSet sets[11] = {};
    if (vkAllocateDescriptorSets(m_device.Handle(), &alloc, sets) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] alloc SSR descriptor sets failed\n");
        return false;
    }
    m_ssrSortSet              = sets[0];
    m_ssrPrefixSet            = sets[1];
    m_ssrTraceSets[0]         = sets[2];
    m_ssrTraceSets[1]         = sets[3];
    m_ssrTemporalSets[0]      = sets[4];
    m_ssrTemporalSets[1]      = sets[5];
    m_ssrSpatialSets[0][0]    = sets[6];
    m_ssrSpatialSets[0][1]    = sets[7];
    m_ssrSpatialSets[1][0]    = sets[8];
    m_ssrSpatialSets[1][1]    = sets[9];
    m_ssrUpsampleSet          = sets[10];

    // ---- Sort set: 2 G-Buffer samplers + 5 SSBOs -----------------------
    {
        VkDescriptorImageInfo posInfo{}, norInfo{};
        posInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        posInfo.imageView   = m_gPosition.View();
        posInfo.sampler     = m_gbufferSampler;
        norInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        norInfo.imageView   = m_gNormal.View();
        norInfo.sampler     = m_gbufferSampler;

        const BufferVk* ssbos[5] = {
            &m_ssrRayCount, &m_ssrBinCounters, &m_ssrBinOffsets,
            &m_ssrScatterCounters, &m_ssrSortedRays,
        };
        VkDescriptorBufferInfo bufInfos[5]{};
        for (int i = 0; i < 5; ++i) {
            bufInfos[i].buffer = ssbos[i]->Handle();
            bufInfos[i].range  = VK_WHOLE_SIZE;
        }
        VkWriteDescriptorSet w[7]{};
        w[0].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet=m_ssrSortSet;
        w[0].dstBinding=0; w[0].descriptorCount=1;
        w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo=&posInfo;
        w[1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet=m_ssrSortSet;
        w[1].dstBinding=1; w[1].descriptorCount=1;
        w[1].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo=&norInfo;
        for (int i = 0; i < 5; ++i) {
            w[2 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[2 + i].dstSet = m_ssrSortSet;
            w[2 + i].dstBinding = static_cast<uint32_t>(2 + i);
            w[2 + i].descriptorCount = 1;
            w[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[2 + i].pBufferInfo = &bufInfos[i];
        }
        vkUpdateDescriptorSets(m_device.Handle(), 7, w, 0, nullptr);
    }

    // ---- Prefix set: 4 SSBOs -------------------------------------------
    {
        const BufferVk* ssbos[4] = {
            &m_ssrBinCounters, &m_ssrBinOffsets,
            &m_ssrRayCount, &m_ssrIndirectArgs,
        };
        VkDescriptorBufferInfo bufInfos[4]{};
        VkWriteDescriptorSet   writes[4]{};
        for (int i = 0; i < 4; ++i) {
            bufInfos[i].buffer = ssbos[i]->Handle();
            bufInfos[i].range  = VK_WHOLE_SIZE;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = m_ssrPrefixSet;
            writes[i].dstBinding = static_cast<uint32_t>(i);
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &bufInfos[i];
        }
        vkUpdateDescriptorSets(m_device.Handle(), 4, writes, 0, nullptr);
    }

    // ---- Trace sets (2): see writeSSRTraceDescriptors ------------------
    writeSSRTraceDescriptors();

    // ---- SVGF Temporal sets (2 — one per "curSlot" direction) ----------
    // Temporal pass reads raw SSR + history color/moments, writes new color
    // (into the intra-frame scratch image) and new moments.
    for (uint32_t curSlot = 0; curSlot < 2; ++curSlot) {
        const uint32_t prevSlot = 1u - curSlot;

        VkDescriptorImageInfo curInfo{}, histColInfo{}, histMomInfo{};
        VkDescriptorImageInfo norInfo{}, posInfo{}, motInfo{};
        VkDescriptorImageInfo outColInfo{}, outMomInfo{};

        curInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        curInfo.imageView   = m_ssrResult.View();
        curInfo.sampler     = m_gbufferSampler;

        histColInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        histColInfo.imageView   = m_ssrColor[prevSlot].View();
        histColInfo.sampler     = m_gbufferSampler;

        histMomInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        histMomInfo.imageView   = m_ssrMoments[prevSlot].View();
        histMomInfo.sampler     = m_gbufferSampler;

        norInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        norInfo.imageView   = m_gNormal.View();   norInfo.sampler = m_gbufferSampler;
        posInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        posInfo.imageView   = m_gPosition.View(); posInfo.sampler = m_gbufferSampler;
        motInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        motInfo.imageView   = m_gMotion.View();   motInfo.sampler = m_gbufferSampler;

        outColInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        outColInfo.imageView   = m_ssrColorTemp.View();

        outMomInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        outMomInfo.imageView   = m_ssrMoments[curSlot].View();

        VkWriteDescriptorSet w[8]{};
        const VkDescriptorImageInfo* images[6] = {
            &curInfo, &histColInfo, &histMomInfo, &norInfo, &posInfo, &motInfo,
        };
        for (int i = 0; i < 6; ++i) {
            w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet          = m_ssrTemporalSets[curSlot];
            w[i].dstBinding      = static_cast<uint32_t>(i);
            w[i].descriptorCount = 1;
            w[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo      = images[i];
        }
        w[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[6].dstSet = m_ssrTemporalSets[curSlot];
        w[6].dstBinding = 6; w[6].descriptorCount = 1;
        w[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[6].pImageInfo = &outColInfo;
        w[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[7].dstSet = m_ssrTemporalSets[curSlot];
        w[7].dstBinding = 7; w[7].descriptorCount = 1;
        w[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[7].pImageInfo = &outMomInfo;
        vkUpdateDescriptorSets(m_device.Handle(), 8, w, 0, nullptr);
    }

    // ---- SVGF Spatial sets (4 — [curSlot][direction]) ------------------
    // Iterations alternate input/output between m_ssrColorTemp and
    // m_ssrColor[curSlot]:
    //   direction 0 — reads Temp, writes Color[curSlot] (iter1, iter3)
    //   direction 1 — reads Color[curSlot], writes Temp (iter2)
    // The moments image used for variance lookup is always m_ssrMoments[curSlot].
    for (uint32_t curSlot = 0; curSlot < 2; ++curSlot) {
        for (uint32_t dir = 0; dir < 2; ++dir) {
            VkDescriptorImageInfo inInfo{}, momInfo{}, norInfo{}, posInfo{}, outInfo{};

            const VkImageView inView  = (dir == 0) ? m_ssrColorTemp.View()
                                                   : m_ssrColor[curSlot].View();
            const VkImageView outView = (dir == 0) ? m_ssrColor[curSlot].View()
                                                   : m_ssrColorTemp.View();

            inInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            inInfo.imageView   = inView; inInfo.sampler = m_gbufferSampler;

            momInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            momInfo.imageView   = m_ssrMoments[curSlot].View();
            momInfo.sampler     = m_gbufferSampler;

            norInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            norInfo.imageView   = m_gNormal.View();   norInfo.sampler = m_gbufferSampler;
            posInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            posInfo.imageView   = m_gPosition.View(); posInfo.sampler = m_gbufferSampler;

            outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            outInfo.imageView   = outView;

            VkWriteDescriptorSet w[5]{};
            const VkDescriptorImageInfo* images[4] = {
                &inInfo, &momInfo, &norInfo, &posInfo,
            };
            for (int i = 0; i < 4; ++i) {
                w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet          = m_ssrSpatialSets[curSlot][dir];
                w[i].dstBinding      = static_cast<uint32_t>(i);
                w[i].descriptorCount = 1;
                w[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[i].pImageInfo      = images[i];
            }
            w[4].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[4].dstSet          = m_ssrSpatialSets[curSlot][dir];
            w[4].dstBinding      = 4;
            w[4].descriptorCount = 1;
            w[4].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[4].pImageInfo      = &outInfo;
            vkUpdateDescriptorSets(m_device.Handle(), 5, w, 0, nullptr);
        }
    }

    // ---- Upsample set: see writeSSRUpsampleDescriptors -----------------
    writeSSRUpsampleDescriptors();

    return true;
}

void VulkanContext::writeSSRUpsampleDescriptors() {
    // Joint-bilateral upsample: full-res G-Buffer position/normal + the
    // half-res reflection target (in GENERAL, where the trace leaves it),
    // writing into the full-res m_ssrResult storage image (also GENERAL).
    VkDescriptorImageInfo posInfo{}, norInfo{}, halfInfo{}, outInfo{};
    posInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    posInfo.imageView   = m_gPosition.View();    posInfo.sampler = m_gbufferSampler;
    norInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    norInfo.imageView   = m_gNormal.View();      norInfo.sampler = m_gbufferSampler;
    halfInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    halfInfo.imageView   = m_ssrResultHalf.View(); halfInfo.sampler = m_gbufferSampler;
    outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    outInfo.imageView   = m_ssrResult.View();

    VkWriteDescriptorSet w[4]{};
    const VkDescriptorImageInfo* images[3] = { &posInfo, &norInfo, &halfInfo };
    for (int i = 0; i < 3; ++i) {
        w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet          = m_ssrUpsampleSet;
        w[i].dstBinding      = static_cast<uint32_t>(i);
        w[i].descriptorCount = 1;
        w[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo      = images[i];
    }
    w[3].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[3].dstSet=m_ssrUpsampleSet;
    w[3].dstBinding=3; w[3].descriptorCount=1;
    w[3].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[3].pImageInfo=&outInfo;
    vkUpdateDescriptorSets(m_device.Handle(), 4, w, 0, nullptr);
}

void VulkanContext::writeSSRTraceDescriptors() {
    // The prevFrame source is a TAA history slot — but at the time the SSR
    // trace runs we don't know yet which slot the current frame will USE
    // as the "current input"; we want the OPPOSITE slot (the one written
    // last frame). Resolve this with one set per slot and bind the right
    // one each frame.
    for (uint32_t slot = 0; slot < 2; ++slot) {
        VkDescriptorImageInfo posInfo{}, norInfo{}, albInfo{}, hizInfo{}, prevInfo{}, outInfo{};
        posInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        posInfo.imageView   = m_gPosition.View();  posInfo.sampler = m_gbufferSampler;
        norInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        norInfo.imageView   = m_gNormal.View();    norInfo.sampler = m_gbufferSampler;
        albInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        albInfo.imageView   = m_gAlbedo.View();    albInfo.sampler = m_gbufferSampler;
        // Hi-Z is kept in GENERAL across the frame to allow per-mip storage
        // writes during the chain build; sampling from GENERAL is legal.
        hizInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        hizInfo.imageView   = m_hiZ.View();
        hizInfo.sampler     = m_hiZSampler;
        prevInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        prevInfo.imageView   = m_taaHistory[slot].View();
        prevInfo.sampler     = m_gbufferSampler;
        // When half-res SSR is enabled the trace writes the half-res target;
        // the upsample pass then reconstructs m_ssrResult. Otherwise it writes
        // m_ssrResult directly (legacy full-res path).
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        outInfo.imageView   = m_ssrHalfResEnabled ? m_ssrResultHalf.View()
                                                  : m_ssrResult.View();

        VkDescriptorBufferInfo sortedInfo{}, countInfo{};
        sortedInfo.buffer = m_ssrSortedRays.Handle();
        sortedInfo.range  = VK_WHOLE_SIZE;
        countInfo.buffer  = m_ssrRayCount.Handle();
        countInfo.range   = VK_WHOLE_SIZE;

        VkWriteDescriptorSet w[8]{};
        const VkDescriptorImageInfo* images[5] = { &posInfo, &norInfo, &albInfo, &hizInfo, &prevInfo };
        for (int i = 0; i < 5; ++i) {
            w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet          = m_ssrTraceSets[slot];
            w[i].dstBinding      = static_cast<uint32_t>(i);
            w[i].descriptorCount = 1;
            w[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].pImageInfo      = images[i];
        }
        w[5].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[5].dstSet=m_ssrTraceSets[slot];
        w[5].dstBinding=5; w[5].descriptorCount=1;
        w[5].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[5].pBufferInfo=&sortedInfo;
        w[6].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[6].dstSet=m_ssrTraceSets[slot];
        w[6].dstBinding=6; w[6].descriptorCount=1;
        w[6].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[6].pBufferInfo=&countInfo;
        w[7].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[7].dstSet=m_ssrTraceSets[slot];
        w[7].dstBinding=7; w[7].descriptorCount=1;
        w[7].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[7].pImageInfo=&outInfo;
        vkUpdateDescriptorSets(m_device.Handle(), 8, w, 0, nullptr);
    }
}

// ===== Post-process: HDR scratch + ACES tonemap (FASE 14.A) ==============

bool VulkanContext::createPostResources() {
    ImageCreateParams p{};
    p.width  = m_swapchain.Extent().width;
    p.height = m_swapchain.Extent().height;
    p.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    // COLOR_ATTACHMENT so the TAA pass writes through dynamic rendering,
    // SAMPLED so the tonemap pass samples it as a fullscreen input.
    p.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (!m_postScratch.Create(m_device.Handle(), m_allocator.Handle(), p)) return false;

    // E1.A — Offscreen viewport color target. Tonemap writes here instead
    // of the swapchain; the editor UI then samples it as a texture and
    // draws it inside the center panel via FluentUI::Image. Format matches
    // the swapchain so the existing tonemap pipeline (declared with that
    // color attachment format) can target this image without changes.
    // TRANSFER_SRC supports a blit-to-swapchain fallback path while we
    // wire the FluentUI Image widget.
    ImageCreateParams vp{};
    vp.width  = m_swapchain.Extent().width;
    vp.height = m_swapchain.Extent().height;
    vp.format = m_swapchain.ImageFormat();
    vp.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
              | VK_IMAGE_USAGE_SAMPLED_BIT
              | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    vp.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (!m_viewportColor.Create(m_device.Handle(), m_allocator.Handle(), vp)) return false;

    // CAS intermediate. When CAS is enabled the tonemap writes here instead of
    // m_viewportColor; the CAS pass then samples this and writes m_viewportColor.
    // Same format as m_viewportColor (swapchain format) so the CAS pipeline —
    // declared with that color attachment format — can target either image. No
    // TRANSFER_SRC needed: this is only ever read by the CAS fragment shader.
    ImageCreateParams tm{};
    tm.width  = m_swapchain.Extent().width;
    tm.height = m_swapchain.Extent().height;
    tm.format = m_swapchain.ImageFormat();
    tm.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    tm.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    return m_tonemapLdr.Create(m_device.Handle(), m_allocator.Handle(), tm);
}

void VulkanContext::destroyPostResources() {
    m_postScratch.Destroy();
    m_viewportColor.Destroy();
    m_tonemapLdr.Destroy();
}

bool VulkanContext::createTonemapPipeline() {
    // ---- Descriptor set layout: 3 combined image samplers + 1 SSBO -----
    // binding 0 = HDR post-scratch, binding 1 = bloom mip 0, binding 2 = vol fog,
    // binding 3 = auto-exposure result (adapted avg luminance).
    VkDescriptorSetLayoutBinding b[4]{};
    for (int i = 0; i < 4; ++i) {
        b[i].binding         = static_cast<uint32_t>(i);
        b[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 4;
    info.pBindings    = b;
    if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                    &m_tonemapSetLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] Tonemap set layout failed\n");
        return false;
    }

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = 40;  // 10 floats

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &m_tonemapSetLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                               &m_tonemapPipelineLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] Tonemap pipeline layout failed\n");
        return false;
    }

    // ---- Graphics pipeline: fullscreen vertex + ACES fragment ---------
    VkShaderModule vs = loadShaderModule("Shaders/SPV/fullscreen.vert.spv");
    VkShaderModule fs = loadShaderModule("Shaders/SPV/tonemap.frag.spv");
    if (!vs || !fs) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState att = makeOpaqueBlend();
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates    = dyn;

    // Dynamic rendering: target swapchain format.
    const VkFormat swapFmt = m_swapchain.ImageFormat();
    VkPipelineRenderingCreateInfo render{};
    render.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    render.colorAttachmentCount    = 1;
    render.pColorAttachmentFormats = &swapFmt;

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.pNext               = &render;
    pi.stageCount          = 2;
    pi.pStages             = stages;
    pi.pVertexInputState   = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState      = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState   = &ms;
    pi.pColorBlendState    = &cb;
    pi.pDynamicState       = &ds;
    pi.layout              = m_tonemapPipelineLayout;

    VkResult r = vkCreateGraphicsPipelines(m_device.Handle(), m_pipelineCache, 1,
                                           &pi, nullptr, &m_tonemapPipeline);
    vkDestroyShaderModule(m_device.Handle(), vs, nullptr);
    vkDestroyShaderModule(m_device.Handle(), fs, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] Tonemap pipeline create failed: %d\n", r);
        return false;
    }
    return true;
}

bool VulkanContext::createTonemapDescriptors() {
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = m_descriptorPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts        = &m_tonemapSetLayout;
    if (vkAllocateDescriptorSets(m_device.Handle(), &alloc, &m_tonemapSet) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] alloc tonemap descriptor set failed\n");
        return false;
    }
    writeTonemapDescriptors();
    return true;
}

void VulkanContext::writeTonemapDescriptors() {
    VkDescriptorImageInfo infos[3]{};
    infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[0].imageView   = m_postScratch.View();
    infos[0].sampler     = m_taaSampler;
    infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[1].imageView   = m_bloomUpSampleViews.empty()
                         ? m_postScratch.View()
                         : m_bloomUpSampleViews[0];
    infos[1].sampler     = m_taaSampler;
    infos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[2].imageView   = (m_volFog.Handle() != VK_NULL_HANDLE)
                         ? m_volFog.View()
                         : m_postScratch.View();      // placeholder
    infos[2].sampler     = m_taaSampler;

    // binding 3 — auto-exposure result SSBO (read for the adaptive multiplier).
    VkDescriptorBufferInfo expInfo{};
    expInfo.buffer = m_exposureBuffer.Handle();
    expInfo.range  = VK_WHOLE_SIZE;

    VkWriteDescriptorSet w[4]{};
    for (int i = 0; i < 3; ++i) {
        w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet          = m_tonemapSet;
        w[i].dstBinding      = static_cast<uint32_t>(i);
        w[i].descriptorCount = 1;
        w[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo      = &infos[i];
    }
    w[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[3].dstSet          = m_tonemapSet;
    w[3].dstBinding      = 3;
    w[3].descriptorCount = 1;
    w[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[3].pBufferInfo     = &expInfo;
    vkUpdateDescriptorSets(m_device.Handle(), 4, w, 0, nullptr);
}

// ===== CAS — AMD Contrast Adaptive Sharpening (post-tonemap pass) =========

bool VulkanContext::createCASPipeline() {
    // ---- Descriptor set layout: 1 combined image sampler (tonemap LDR) ----
    VkDescriptorSetLayoutBinding b{};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings    = &b;
    if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                    &m_casSetLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] CAS set layout failed\n");
        return false;
    }

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = 4;  // 1 float sharpness

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &m_casSetLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                               &m_casPipelineLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] CAS pipeline layout failed\n");
        return false;
    }

    // ---- Graphics pipeline: fullscreen vertex + CAS fragment ----------
    VkShaderModule vs = loadShaderModule("Shaders/SPV/fullscreen.vert.spv");
    VkShaderModule fs = loadShaderModule("Shaders/SPV/cas_sharpen.frag.spv");
    if (!vs || !fs) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState att = makeOpaqueBlend();
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates    = dyn;

    // Dynamic rendering: target swapchain format.
    const VkFormat swapFmt = m_swapchain.ImageFormat();
    VkPipelineRenderingCreateInfo render{};
    render.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    render.colorAttachmentCount    = 1;
    render.pColorAttachmentFormats = &swapFmt;

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.pNext               = &render;
    pi.stageCount          = 2;
    pi.pStages             = stages;
    pi.pVertexInputState   = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState      = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState   = &ms;
    pi.pColorBlendState    = &cb;
    pi.pDynamicState       = &ds;
    pi.layout              = m_casPipelineLayout;

    VkResult r = vkCreateGraphicsPipelines(m_device.Handle(), m_pipelineCache, 1,
                                           &pi, nullptr, &m_casPipeline);
    vkDestroyShaderModule(m_device.Handle(), vs, nullptr);
    vkDestroyShaderModule(m_device.Handle(), fs, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] CAS pipeline create failed: %d\n", r);
        return false;
    }
    return true;
}

bool VulkanContext::createCASDescriptors() {
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = m_descriptorPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts        = &m_casSetLayout;
    if (vkAllocateDescriptorSets(m_device.Handle(), &alloc, &m_casSet) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] alloc CAS descriptor set failed\n");
        return false;
    }
    writeCASDescriptors();
    return true;
}

void VulkanContext::writeCASDescriptors() {
    VkDescriptorImageInfo info{};
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.imageView   = m_tonemapLdr.View();
    info.sampler     = m_taaSampler;

    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = m_casSet;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &info;
    vkUpdateDescriptorSets(m_device.Handle(), 1, &w, 0, nullptr);
}

// ===== Debug-line overlay ================================================

bool VulkanContext::createDebugLineResources() {
    m_debugLineCapacity = 4096;  // 2048 lines
    m_debugTriCapacity  = 4096;
    const VkDeviceSize bytes    = sizeof(DebugLineVertex) * m_debugLineCapacity;
    const VkDeviceSize triBytes = sizeof(DebugLineVertex) * m_debugTriCapacity;
    // One vertex buffer per frame-in-flight (line + triangle) so the CPU never
    // refills a buffer the GPU is still reading from the previous frame.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!m_debugLineBuffer[i].CreateHostCoherent(m_allocator.Handle(), bytes,
                                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            return false;
        }
        if (!m_debugTriBuffer[i].CreateHostCoherent(m_allocator.Handle(), triBytes,
                                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            return false;
        }
    }
    m_debugLineCount = 0;
    m_debugTriCount  = 0;
    return true;
}

void VulkanContext::destroyDebugLineResources() {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        m_debugLineBuffer[i].Destroy();
        m_debugTriBuffer[i].Destroy();
    }
}

bool VulkanContext::createDebugLinePipeline() {
    // Push constant only — a single viewProj mat4 (no descriptor sets).
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 0;
    plInfo.pSetLayouts            = nullptr;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                               &m_debugLinePipelineLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] debug-line pipeline layout failed\n");
        return false;
    }

    VkShaderModule vs = loadShaderModule("Shaders/SPV/debug_line.vert.spv");
    VkShaderModule fs = loadShaderModule("Shaders/SPV/debug_line.frag.spv");
    if (!vs || !fs) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription vbind{};
    vbind.binding   = 0;
    vbind.stride    = sizeof(DebugLineVertex);
    vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription vattr[2]{};
    vattr[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(DebugLineVertex, pos)   };
    vattr[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(DebugLineVertex, color) };
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &vbind;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions    = vattr;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // Thicker gizmo axes need the optional wideLines feature; without it Vulkan
    // requires lineWidth == 1.0 (a static value > 1.0 is a spec violation that
    // the validation layer flags). Fall back to 1.0 on GPUs that lack it.
    rs.lineWidth   = m_device.WideLinesSupported() ? 2.5f : 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Overlay: no depth attachment, no depth test/write.
    VkPipelineDepthStencilStateCreateInfo dss{};
    dss.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable  = VK_FALSE;
    dss.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState att = makeOpaqueBlend();
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates    = dyn;

    // Dynamic rendering: single color attachment = swapchain format (overlay
    // target m_viewportColor is the same format).
    const VkFormat swapFmt = m_swapchain.ImageFormat();
    VkPipelineRenderingCreateInfo render{};
    render.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    render.colorAttachmentCount    = 1;
    render.pColorAttachmentFormats = &swapFmt;

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.pNext               = &render;
    pi.stageCount          = 2;
    pi.pStages             = stages;
    pi.pVertexInputState   = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState      = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState   = &ms;
    pi.pDepthStencilState  = &dss;
    pi.pColorBlendState    = &cb;
    pi.pDynamicState       = &ds;
    pi.layout              = m_debugLinePipelineLayout;

    VkResult r = vkCreateGraphicsPipelines(m_device.Handle(), m_pipelineCache, 1,
                                           &pi, nullptr, &m_debugLinePipeline);
    vkDestroyShaderModule(m_device.Handle(), vs, nullptr);
    vkDestroyShaderModule(m_device.Handle(), fs, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] debug-line pipeline create failed: %d\n", r);
        return false;
    }
    return true;
}

bool VulkanContext::createDebugTriPipeline() {
    // Reuses m_debugLinePipelineLayout (must be created first by
    // createDebugLinePipeline) and the debug_line shaders. Only difference vs.
    // the line pipeline: TRIANGLE_LIST topology with FILL/no-cull rasterization.
    VkShaderModule vs = loadShaderModule("Shaders/SPV/debug_line.vert.spv");
    VkShaderModule fs = loadShaderModule("Shaders/SPV/debug_line.frag.spv");
    if (!vs || !fs) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription vbind{};
    vbind.binding   = 0;
    vbind.stride    = sizeof(DebugLineVertex);
    vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription vattr[2]{};
    vattr[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(DebugLineVertex, pos)   };
    vattr[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(DebugLineVertex, color) };
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &vbind;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions    = vattr;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;   // cone visible from both sides
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;  // irrelevant in FILL mode

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Overlay: no depth attachment, no depth test/write.
    VkPipelineDepthStencilStateCreateInfo dss{};
    dss.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable  = VK_FALSE;
    dss.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState att = makeOpaqueBlend();
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates    = dyn;

    const VkFormat swapFmt = m_swapchain.ImageFormat();
    VkPipelineRenderingCreateInfo render{};
    render.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    render.colorAttachmentCount    = 1;
    render.pColorAttachmentFormats = &swapFmt;

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.pNext               = &render;
    pi.stageCount          = 2;
    pi.pStages             = stages;
    pi.pVertexInputState   = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState      = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState   = &ms;
    pi.pDepthStencilState  = &dss;
    pi.pColorBlendState    = &cb;
    pi.pDynamicState       = &ds;
    pi.layout              = m_debugLinePipelineLayout;   // shared layout

    VkResult r = vkCreateGraphicsPipelines(m_device.Handle(), m_pipelineCache, 1,
                                           &pi, nullptr, &m_debugTriPipeline);
    vkDestroyShaderModule(m_device.Handle(), vs, nullptr);
    vkDestroyShaderModule(m_device.Handle(), fs, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] debug-triangle pipeline create failed: %d\n", r);
        return false;
    }
    return true;
}

void VulkanContext::AddDebugLine(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color) {
    // Write into the CURRENT frame's buffer (the one the DebugLines pass will
    // bind this frame); m_frameIndex is stable between BeginFrame and EndFrame.
    BufferVk& buf = m_debugLineBuffer[m_frameIndex];
    if (buf.Mapped() == nullptr) return;
    if (m_debugLineCount + 2 > m_debugLineCapacity) return;  // drop if full
    auto* v = static_cast<DebugLineVertex*>(buf.Mapped());
    v[m_debugLineCount++] = { a, color };
    v[m_debugLineCount++] = { b, color };
}

void VulkanContext::AddDebugTriangle(const glm::vec3& a, const glm::vec3& b,
                                     const glm::vec3& c, const glm::vec3& color) {
    BufferVk& buf = m_debugTriBuffer[m_frameIndex];
    if (buf.Mapped() == nullptr) return;
    if (m_debugTriCount + 3 > m_debugTriCapacity) return;  // drop if full
    auto* v = static_cast<DebugLineVertex*>(buf.Mapped());
    v[m_debugTriCount++] = { a, color };
    v[m_debugTriCount++] = { b, color };
    v[m_debugTriCount++] = { c, color };
}

// ===== Auto-exposure (histogram-based) ===================================

bool VulkanContext::createAutoExposureResources() {
    const VmaAllocator alloc = m_allocator.Handle();

    // 256-bin uint histogram + a single float adapted average luminance. Both
    // are resolution-independent and persist across frames (the average is the
    // temporal eye-adaptation state) — they are NOT recreated on resize.
    if (!m_histogramBuffer.CreateStorage(alloc, sizeof(uint32_t) * 256)) return false;
    if (!m_exposureBuffer.CreateStorage(alloc, sizeof(float)))           return false;

    // Initialize both to zero so the first frame's build accumulates into a
    // clean histogram and the average pass takes its "no history" branch.
    {
        OneShotCmd one(m_device.Handle(), m_device.GraphicsQueueFamily(),
                       m_device.GraphicsQueue());
        m_histogramBuffer.CmdFill(one.cmd(), 0u);
        m_exposureBuffer.CmdFill(one.cmd(), 0u);
        // OneShotCmd destructor submits + waitIdle.
    }
    return true;
}

bool VulkanContext::createAutoExposurePipelines() {
    // ---- Build: combined image sampler (HDR) + histogram SSBO -----------
    {
        VkDescriptorSetLayoutBinding b[2]{};
        for (int i = 0; i < 2; ++i) {
            b[i].binding         = static_cast<uint32_t>(i);
            b[i].descriptorCount = 1;
            b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 2;
        info.pBindings    = b;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                        &m_histBuildSetLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] histogram build set layout failed\n");
            return false;
        }
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset     = 0;
        pcRange.size       = sizeof(HistBuildPC);
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &m_histBuildSetLayout;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &pcRange;
        if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                                   &m_histBuildPipelineLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] histogram build pipeline layout failed\n");
            return false;
        }
        VkShaderModule cs = loadShaderModule("Shaders/SPV/histogram_build.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = cs;
        stage.pName  = "main";
        VkComputePipelineCreateInfo cpi{};
        cpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage  = stage;
        cpi.layout = m_histBuildPipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                              &cpi, nullptr, &m_histBuildPipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] histogram build pipeline failed: %d\n", r);
            return false;
        }
    }

    // ---- Average: histogram SSBO + exposure SSBO ------------------------
    {
        VkDescriptorSetLayoutBinding b[2]{};
        for (int i = 0; i < 2; ++i) {
            b[i].binding         = static_cast<uint32_t>(i);
            b[i].descriptorCount = 1;
            b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            b[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 2;
        info.pBindings    = b;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                        &m_histAvgSetLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] histogram average set layout failed\n");
            return false;
        }
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset     = 0;
        pcRange.size       = sizeof(HistAvgPC);
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &m_histAvgSetLayout;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &pcRange;
        if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                                   &m_histAvgPipelineLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] histogram average pipeline layout failed\n");
            return false;
        }
        VkShaderModule cs = loadShaderModule("Shaders/SPV/histogram_average.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = cs;
        stage.pName  = "main";
        VkComputePipelineCreateInfo cpi{};
        cpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage  = stage;
        cpi.layout = m_histAvgPipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                              &cpi, nullptr, &m_histAvgPipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] histogram average pipeline failed: %d\n", r);
            return false;
        }
    }
    return true;
}

bool VulkanContext::createAutoExposureDescriptors() {
    VkDescriptorSetLayout layouts[2] = { m_histBuildSetLayout, m_histAvgSetLayout };
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = m_descriptorPool;
    alloc.descriptorSetCount = 2;
    alloc.pSetLayouts        = layouts;
    VkDescriptorSet sets[2] = {};
    if (vkAllocateDescriptorSets(m_device.Handle(), &alloc, sets) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] alloc auto-exposure descriptor sets failed\n");
        return false;
    }
    m_histBuildSet = sets[0];
    m_histAvgSet   = sets[1];
    writeAutoExposureDescriptors();
    return true;
}

void VulkanContext::writeAutoExposureDescriptors() {
    // Build set: binding 0 = HDR postScratch (recreated on resize → rewritten
    // here), binding 1 = histogram SSBO.
    VkDescriptorImageInfo hdrInfo{};
    hdrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    hdrInfo.imageView   = m_postScratch.View();
    hdrInfo.sampler     = m_taaSampler;

    VkDescriptorBufferInfo histInfo{};
    histInfo.buffer = m_histogramBuffer.Handle();
    histInfo.range  = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo expInfo{};
    expInfo.buffer = m_exposureBuffer.Handle();
    expInfo.range  = VK_WHOLE_SIZE;

    VkWriteDescriptorSet w[4]{};
    w[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet          = m_histBuildSet;
    w[0].dstBinding      = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo      = &hdrInfo;
    w[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet          = m_histBuildSet;
    w[1].dstBinding      = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo     = &histInfo;
    // Average set: binding 0 = histogram, binding 1 = exposure. Both buffers
    // persist, so rewriting them on resize is harmless.
    w[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[2].dstSet          = m_histAvgSet;
    w[2].dstBinding      = 0;
    w[2].descriptorCount = 1;
    w[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[2].pBufferInfo     = &histInfo;
    w[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[3].dstSet          = m_histAvgSet;
    w[3].dstBinding      = 1;
    w[3].descriptorCount = 1;
    w[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[3].pBufferInfo     = &expInfo;
    vkUpdateDescriptorSets(m_device.Handle(), 4, w, 0, nullptr);
}

// ===== Bloom (FASE 14.B) ==================================================

bool VulkanContext::createBloomResources() {
    const uint32_t w = m_swapchain.Extent().width;
    const uint32_t h = m_swapchain.Extent().height;

    // Mip 0 is half-resolution; chain ends when a dim reaches ≤ 8 px.
    uint32_t mip0W = w / 2, mip0H = h / 2;
    uint32_t mips = 1;
    while ((mip0W >> mips) >= 8 && (mip0H >> mips) >= 8 && mips < 8) ++mips;
    m_bloomMipCount = mips;

    auto makeChain = [&](ImageVk& out) {
        ImageCreateParams p{};
        p.width     = mip0W;
        p.height    = mip0H;
        p.format    = VK_FORMAT_R16G16B16A16_SFLOAT;
        p.usage     = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        p.aspect    = VK_IMAGE_ASPECT_COLOR_BIT;
        p.mipLevels = m_bloomMipCount;
        return out.Create(m_device.Handle(), m_allocator.Handle(), p);
    };
    if (!makeChain(m_bloomDown)) return false;
    if (!makeChain(m_bloomUp))   return false;

    auto makeViews = [&](VkImage img, std::vector<VkImageView>& store,
                         std::vector<VkImageView>& sample) {
        store.assign(m_bloomMipCount, VK_NULL_HANDLE);
        sample.assign(m_bloomMipCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < m_bloomMipCount; ++i) {
            VkImageViewCreateInfo v{};
            v.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            v.image    = img;
            v.viewType = VK_IMAGE_VIEW_TYPE_2D;
            v.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
            v.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            v.subresourceRange.baseMipLevel   = i;
            v.subresourceRange.levelCount     = 1;
            v.subresourceRange.baseArrayLayer = 0;
            v.subresourceRange.layerCount     = 1;
            if (vkCreateImageView(m_device.Handle(), &v, nullptr, &store[i]) != VK_SUCCESS) return false;
            // sample view is the same shape (single-mip). Identical here, but
            // separate handles let us bind store+sample to different sets if needed.
            if (vkCreateImageView(m_device.Handle(), &v, nullptr, &sample[i]) != VK_SUCCESS) return false;
        }
        return true;
    };
    if (!makeViews(m_bloomDown.Handle(), m_bloomDownMipViews, m_bloomDownSampleViews)) return false;
    if (!makeViews(m_bloomUp.Handle(),   m_bloomUpMipViews,   m_bloomUpSampleViews))   return false;

    std::printf("[Vulkan] Bloom chain: %ux%u, %u mips\n", mip0W, mip0H, m_bloomMipCount);
    return true;
}

void VulkanContext::destroyBloomResources() {
    auto destroy = [&](std::vector<VkImageView>& vs) {
        for (auto v : vs) if (v) vkDestroyImageView(m_device.Handle(), v, nullptr);
        vs.clear();
    };
    destroy(m_bloomDownMipViews);
    destroy(m_bloomUpMipViews);
    destroy(m_bloomDownSampleViews);
    destroy(m_bloomUpSampleViews);
    m_bloomDown.Destroy();
    m_bloomUp.Destroy();
    m_bloomMipCount = 0;
}

bool VulkanContext::createBloomPipelines() {
    // ---- Prefilter set layout: 1 sampler + 1 storage image -------------
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding=0; b[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].binding=1; b[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        for (int i = 0; i < 2; ++i) { b[i].descriptorCount=1; b[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT; }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount=2; info.pBindings=b;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                        &m_bloomPrefilterSetLayout) != VK_SUCCESS) return false;
        VkPushConstantRange pcR{};
        pcR.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcR.size       = 16;  // 2 floats + 2 ints
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount=1; plInfo.pSetLayouts=&m_bloomPrefilterSetLayout;
        plInfo.pushConstantRangeCount=1; plInfo.pPushConstantRanges=&pcR;
        if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                                   &m_bloomPrefilterPipelineLayout) != VK_SUCCESS) return false;
        VkShaderModule cs = loadShaderModule("Shaders/SPV/bloom_prefilter.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo s{};
        s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage=VK_SHADER_STAGE_COMPUTE_BIT; s.module=cs; s.pName="main";
        VkComputePipelineCreateInfo c{};
        c.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        c.stage=s; c.layout=m_bloomPrefilterPipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                              &c, nullptr, &m_bloomPrefilterPipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) return false;
    }
    // ---- Downsample set layout: 1 sampler + 1 storage image ------------
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding=0; b[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].binding=1; b[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        for (int i = 0; i < 2; ++i) { b[i].descriptorCount=1; b[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT; }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount=2; info.pBindings=b;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                        &m_bloomDownsampleSetLayout) != VK_SUCCESS) return false;
        VkPushConstantRange pcR{};
        pcR.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
        pcR.size = 8;  // 2 ints
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount=1; plInfo.pSetLayouts=&m_bloomDownsampleSetLayout;
        plInfo.pushConstantRangeCount=1; plInfo.pPushConstantRanges=&pcR;
        if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                                   &m_bloomDownsamplePipelineLayout) != VK_SUCCESS) return false;
        VkShaderModule cs = loadShaderModule("Shaders/SPV/bloom_downsample.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo s{};
        s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage=VK_SHADER_STAGE_COMPUTE_BIT; s.module=cs; s.pName="main";
        VkComputePipelineCreateInfo c{};
        c.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        c.stage=s; c.layout=m_bloomDownsamplePipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                              &c, nullptr, &m_bloomDownsamplePipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) return false;
    }
    // ---- Upsample set layout: 2 samplers + 1 storage image -------------
    {
        VkDescriptorSetLayoutBinding b[3]{};
        b[0].binding=0; b[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].binding=1; b[1].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[2].binding=2; b[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        for (int i = 0; i < 3; ++i) { b[i].descriptorCount=1; b[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT; }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount=3; info.pBindings=b;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                        &m_bloomUpsampleSetLayout) != VK_SUCCESS) return false;
        VkPushConstantRange pcR{};
        pcR.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
        pcR.size = 16;  // 2 ints + 2 floats
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount=1; plInfo.pSetLayouts=&m_bloomUpsampleSetLayout;
        plInfo.pushConstantRangeCount=1; plInfo.pPushConstantRanges=&pcR;
        if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                                   &m_bloomUpsamplePipelineLayout) != VK_SUCCESS) return false;
        VkShaderModule cs = loadShaderModule("Shaders/SPV/bloom_upsample.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo s{};
        s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage=VK_SHADER_STAGE_COMPUTE_BIT; s.module=cs; s.pName="main";
        VkComputePipelineCreateInfo c{};
        c.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        c.stage=s; c.layout=m_bloomUpsamplePipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                              &c, nullptr, &m_bloomUpsamplePipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) return false;
    }
    return true;
}

bool VulkanContext::createBloomDescriptors() {
    // Allocate descriptor sets for the MAX possible mip count (matches the
    // pool reservation in createDescriptorPool: `kMaxBloomMips`). On window
    // resize the actual `m_bloomMipCount` can grow past the initial value;
    // pre-allocating to the cap prevents the per-mip dispatch loop from
    // indexing past `m_bloomDownsampleSets.size()`. The initial writes only
    // cover the current `m_bloomMipCount` mips; `writeBloomDescriptors` does
    // the same after a resize.
    constexpr uint32_t kMaxBloomMipsLocal = 8;
    const uint32_t allocCount = kMaxBloomMipsLocal - 1u;
    const uint32_t downCount  = (m_bloomMipCount > 0u) ? (m_bloomMipCount - 1u) : 0u;
    const uint32_t upCount    = downCount;  // same count

    // Allocate: 1 prefilter set + allocCount downsample sets + allocCount upsample
    // sets. Using allocCount (= kMaxBloomMips - 1) reserves headroom for resizes
    // that grow the mip count past initial. `writeBloomDescriptors` and the
    // per-mip dispatch loops use `m_bloomMipCount` to address only the live tail.
    std::vector<VkDescriptorSetLayout> layouts;
    layouts.reserve(1 + allocCount + allocCount);
    layouts.push_back(m_bloomPrefilterSetLayout);
    for (uint32_t i = 0; i < allocCount; ++i) layouts.push_back(m_bloomDownsampleSetLayout);
    for (uint32_t i = 0; i < allocCount; ++i) layouts.push_back(m_bloomUpsampleSetLayout);

    std::vector<VkDescriptorSet> sets(layouts.size(), VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = m_descriptorPool;
    alloc.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    alloc.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(m_device.Handle(), &alloc, sets.data()) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] alloc bloom descriptor sets failed\n");
        return false;
    }
    m_bloomPrefilterSet = sets[0];
    m_bloomDownsampleSets.assign(sets.begin() + 1, sets.begin() + 1 + allocCount);
    m_bloomUpsampleSets.assign(sets.begin() + 1 + allocCount, sets.end());

    writeBloomDescriptors();
    return true;
}

void VulkanContext::writeBloomDescriptors() {
    if (m_bloomDownMipViews.empty()) return;

    // ---- Prefilter set: read postScratch, write down[0] ----------------
    {
        VkDescriptorImageInfo src{}, dst{};
        src.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        src.imageView   = m_postScratch.View();
        src.sampler     = m_taaSampler;
        dst.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        dst.imageView   = m_bloomDownMipViews[0];
        VkWriteDescriptorSet w[2]{};
        w[0].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet=m_bloomPrefilterSet;
        w[0].dstBinding=0; w[0].descriptorCount=1;
        w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo=&src;
        w[1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet=m_bloomPrefilterSet;
        w[1].dstBinding=1; w[1].descriptorCount=1;
        w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo=&dst;
        vkUpdateDescriptorSets(m_device.Handle(), 2, w, 0, nullptr);
    }

    // ---- Downsample sets: read down[i], write down[i+1] ----------------
    // Only write the live mip count; the descriptor set array is pre-allocated
    // to kMaxBloomMips-1 so we don't index past m_bloomDown{Sample,Mip}Views.
    const size_t downIters = (m_bloomMipCount > 0u)
        ? std::min<size_t>(m_bloomDownsampleSets.size(), m_bloomMipCount - 1u)
        : 0u;
    for (size_t i = 0; i < downIters; ++i) {
        VkDescriptorImageInfo src{}, dst{};
        src.imageLayout = VK_IMAGE_LAYOUT_GENERAL;  // chain stays in GENERAL during build
        src.imageView   = m_bloomDownSampleViews[i];
        src.sampler     = m_taaSampler;
        dst.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        dst.imageView   = m_bloomDownMipViews[i + 1];
        VkWriteDescriptorSet w[2]{};
        w[0].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet=m_bloomDownsampleSets[i];
        w[0].dstBinding=0; w[0].descriptorCount=1;
        w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo=&src;
        w[1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet=m_bloomDownsampleSets[i];
        w[1].dstBinding=1; w[1].descriptorCount=1;
        w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo=&dst;
        vkUpdateDescriptorSets(m_device.Handle(), 2, w, 0, nullptr);
    }

    // ---- Upsample sets: read up[i+1] (or down[i+1] for first iter)
    //                  + down[i], write up[i] -------------------------
    // Iteration count = downCount. First iter (i = mipCount-2) uses
    // down[i+1] as source (no up data exists yet). Subsequent use up[i+1].
    // Same live-mip clamp as the downsample loop above.
    const size_t n = (m_bloomMipCount > 0u)
        ? std::min<size_t>(m_bloomUpsampleSets.size(), m_bloomMipCount - 1u)
        : 0u;
    for (size_t k = 0; k < n; ++k) {
        // k=0 is the deepest upsample (small → less small), so it reads down[N-1].
        // Subsequent k uses up[N-1-k] as upSrc.
        const size_t i = n - 1 - k;   // dst mip index (0..N-2)
        VkDescriptorImageInfo upSrc{}, downSrc{}, dst{};
        upSrc.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        upSrc.imageView   = (k == 0)
            ? m_bloomDownSampleViews[i + 1]   // first up iter: pull from down chain
            : m_bloomUpSampleViews[i + 1];
        upSrc.sampler     = m_taaSampler;

        downSrc.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        downSrc.imageView   = m_bloomDownSampleViews[i];
        downSrc.sampler     = m_taaSampler;

        dst.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        dst.imageView   = m_bloomUpMipViews[i];

        VkWriteDescriptorSet w[3]{};
        w[0].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet=m_bloomUpsampleSets[k];
        w[0].dstBinding=0; w[0].descriptorCount=1;
        w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo=&upSrc;
        w[1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet=m_bloomUpsampleSets[k];
        w[1].dstBinding=1; w[1].descriptorCount=1;
        w[1].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo=&downSrc;
        w[2].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet=m_bloomUpsampleSets[k];
        w[2].dstBinding=2; w[2].descriptorCount=1;
        w[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[2].pImageInfo=&dst;
        vkUpdateDescriptorSets(m_device.Handle(), 3, w, 0, nullptr);
    }
}

// ===== Volumetric Fog (FASE 14.D) =========================================

bool VulkanContext::createVolFogResources() {
    ImageCreateParams p{};
    p.width  = std::max(m_swapchain.Extent().width  / 2u, 1u);
    p.height = std::max(m_swapchain.Extent().height / 2u, 1u);
    p.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    // TRANSFER_SRC lets us copy this frame's blended result into the history.
    p.usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
             | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (!m_volFog.Create(m_device.Handle(), m_allocator.Handle(), p)) return false;

    // Temporal history: sampled by the raymarch, written by the per-frame copy.
    ImageCreateParams hp = p;
    hp.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!m_volFogHistory.Create(m_device.Handle(), m_allocator.Handle(), hp)) return false;

    // Fresh on init and after every resize — no valid history yet.
    m_fogHistoryInitialized = false;
    return true;
}

void VulkanContext::destroyVolFogResources() {
    m_volFog.Destroy();
    m_volFogHistory.Destroy();
}

bool VulkanContext::createVolFogPipeline() {
    VkDescriptorSetLayoutBinding b[4]{};
    b[0].binding=0; b[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].binding=1; b[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[2].binding=2; b[2].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;  // CSM shadow (god rays)
    b[3].binding=3; b[3].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;  // temporal history
    for (int i = 0; i < 4; ++i) { b[i].descriptorCount=1; b[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT; }

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 4;
    info.pBindings    = b;
    if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                    &m_volFogSetLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] VolFog set layout failed\n");
        return false;
    }

    VkPushConstantRange pcR{};
    pcR.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcR.size       = sizeof(glm::mat4) + sizeof(glm::vec4) * 3 + sizeof(int32_t) * 3 + sizeof(float) * 5;
    // Aligning to 16 to be safe: 64 + 48 + 12 + 20 = 144. Round to 160 (multiple of 16) just in case.
    pcR.size = 160;
    // set 0 = fog's own (depth + dst + shadow), set 1 = per-frame CameraUBO
    // (reused for the cascade matrices the god-ray shadow lookup needs).
    VkDescriptorSetLayout fogLayouts[2] = { m_volFogSetLayout, m_frameSetLayout };
    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount=2; plInfo.pSetLayouts=fogLayouts;
    plInfo.pushConstantRangeCount=1; plInfo.pPushConstantRanges=&pcR;
    if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                               &m_volFogPipelineLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] VolFog pipeline layout failed\n");
        return false;
    }

    VkShaderModule cs = loadShaderModule("Shaders/SPV/vol_fog.comp.spv");
    if (!cs) return false;
    VkPipelineShaderStageCreateInfo s{};
    s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    s.stage=VK_SHADER_STAGE_COMPUTE_BIT; s.module=cs; s.pName="main";
    VkComputePipelineCreateInfo c{};
    c.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    c.stage=s; c.layout=m_volFogPipelineLayout;
    VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                          &c, nullptr, &m_volFogPipeline);
    vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] VolFog pipeline failed: %d\n", r);
        return false;
    }
    return true;
}

bool VulkanContext::createVolFogDescriptors() {
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool=m_descriptorPool;
    alloc.descriptorSetCount=1; alloc.pSetLayouts=&m_volFogSetLayout;
    if (vkAllocateDescriptorSets(m_device.Handle(), &alloc, &m_volFogSet) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] alloc VolFog descriptor set failed\n");
        return false;
    }
    writeVolFogDescriptors();
    return true;
}

void VulkanContext::writeVolFogDescriptors() {
    VkDescriptorImageInfo depthInfo{}, dstInfo{};
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depthInfo.imageView   = m_depth.View();
    depthInfo.sampler     = m_depthSampler;
    dstInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    dstInfo.imageView   = m_volFog.View();

    // CSM shadow array (comparison sampler) for god-ray shadowing.
    VkDescriptorImageInfo shadowInfo{};
    shadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    shadowInfo.imageView   = m_shadowMap.View();
    shadowInfo.sampler     = m_shadowSampler;

    // Temporal history — linear + clamp (m_taaSampler), so reprojected fetches
    // filter and don't bleed across the screen edge. Layout matches the barrier
    // the fog pass issues before its dispatch (SHADER_READ_ONLY).
    VkDescriptorImageInfo histInfo{};
    histInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    histInfo.imageView   = m_volFogHistory.View();
    histInfo.sampler     = m_taaSampler;

    VkWriteDescriptorSet w[4]{};
    w[0].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet=m_volFogSet;
    w[0].dstBinding=0; w[0].descriptorCount=1;
    w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo=&depthInfo;
    w[1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet=m_volFogSet;
    w[1].dstBinding=1; w[1].descriptorCount=1;
    w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo=&dstInfo;
    w[2].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet=m_volFogSet;
    w[2].dstBinding=2; w[2].descriptorCount=1;
    w[2].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo=&shadowInfo;
    w[3].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[3].dstSet=m_volFogSet;
    w[3].dstBinding=3; w[3].descriptorCount=1;
    w[3].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[3].pImageInfo=&histInfo;
    vkUpdateDescriptorSets(m_device.Handle(), 4, w, 0, nullptr);
}

// ===== Particles (FASE 15.A) ==============================================

namespace {
// Per-particle SSBO struct — matches Shaders/GLSL/particle_update.comp.
struct ParticleGPU {
    glm::vec4 position;  // xyz + size
    glm::vec4 velocity;  // xyz + lifetime
    glm::vec4 color;
    glm::vec4 params;    // maxLife, startSize, endSize, alive
    glm::vec4 extra;     // gravity + pad
};
static_assert(sizeof(ParticleGPU) == 80, "ParticleGPU must match GLSL Particle");
}

bool VulkanContext::createParticleResources() {
    // Reserve a fixed-capacity SSBO. Emitters get slot ranges inside it.
    m_particleCapacity = 8192;
    const VkDeviceSize bytes = sizeof(ParticleGPU) * m_particleCapacity;
    // Host-coherent so we can write new particle slots from the CPU each frame
    // without a staging buffer. Also storage so the compute can update.
    if (!m_particleBuffer.CreateHostCoherent(m_allocator.Handle(), bytes,
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        return false;
    }
    // Initialise to zero — params.w = 0 means "dead", so empty slots stay
    // skipped by the update + render shaders.
    if (m_particleBuffer.Mapped()) {
        std::memset(m_particleBuffer.Mapped(), 0, static_cast<size_t>(bytes));
    }
    return true;
}

void VulkanContext::destroyParticleResources() {
    m_particleBuffer.Destroy();
    m_particleEmitters.clear();
    m_particleCapacity = 0;
}

bool VulkanContext::createLightResources() {
    m_lightCapacity = 64;
    // SSBO layout: 16-byte header (uvec4, .x = active count) + capacity GpuLight.
    // Per-frame-in-flight: the CPU re-packs the buffer every BeginFrame from
    // m_lightDescs, so a single shared buffer would race the in-flight GPU read.
    const VkDeviceSize bytes = 16 + sizeof(GpuLight) * m_lightCapacity;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!m_lightBuffer[i].CreateHostCoherent(m_allocator.Handle(), bytes,
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
            return false;
        }
        if (m_lightBuffer[i].Mapped()) {
            std::memset(m_lightBuffer[i].Mapped(), 0, static_cast<size_t>(bytes));
        }
    }
    m_lightCount = 0;
    m_lightDescs.clear();
    return true;
}

void VulkanContext::destroyLightResources() {
    for (auto& b : m_lightBuffer) b.Destroy();
    m_lightCapacity = 0;
    m_lightCount    = 0;
    m_lightDescs.clear();
}

bool VulkanContext::createParticlePipelines() {
    // ---- SSBO set layout (binding 0 = particle storage) ----------------
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount=1; info.pBindings=&b;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                        &m_particleSsboSetLayout) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] Particle SSBO set layout failed\n");
            return false;
        }
    }

    // ---- Update pipeline (compute) -------------------------------------
    {
        VkPushConstantRange pcR{};
        pcR.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcR.size       = 8;  // float dt + int count
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount=1; plInfo.pSetLayouts=&m_particleSsboSetLayout;
        plInfo.pushConstantRangeCount=1; plInfo.pPushConstantRanges=&pcR;
        if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                                   &m_particleUpdatePipelineLayout) != VK_SUCCESS) return false;
        VkShaderModule cs = loadShaderModule("Shaders/SPV/particle_update.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo s{};
        s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage=VK_SHADER_STAGE_COMPUTE_BIT; s.module=cs; s.pName="main";
        VkComputePipelineCreateInfo c{};
        c.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        c.stage=s; c.layout=m_particleUpdatePipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1,
                                              &c, nullptr, &m_particleUpdatePipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) return false;
    }

    // ---- Render pipeline (graphics, alpha blend, no depth write) -------
    {
        // set 2 = the existing G-Buffer descriptor (reused) so the fragment can
        // sample gPosition for the soft-particle depth fade. Push constant
        // carries the soft toggle + fade distance.
        VkDescriptorSetLayout layouts[3] = { m_frameSetLayout, m_particleSsboSetLayout,
                                             m_gbufferSetLayout };
        VkPushConstantRange pcR{};
        pcR.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcR.offset     = 0;
        pcR.size       = 16;  // softEnabled + fadeDist (frag) + motionBlur + stretchScale (vert)
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount=3; plInfo.pSetLayouts=layouts;
        plInfo.pushConstantRangeCount=1; plInfo.pPushConstantRanges=&pcR;
        if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                                   &m_particleRenderPipelineLayout) != VK_SUCCESS) return false;

        VkShaderModule vs = loadShaderModule("Shaders/SPV/particle.vert.spv");
        VkShaderModule fs = loadShaderModule("Shaders/SPV/particle.frag.spv");
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vs; stages[0].pName="main";
        stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fs; stages[1].pName="main";

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

        VkPipelineViewportStateCreateInfo vp{};
        vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount=1; vp.scissorCount=1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE;
        rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;

        // No depth write (read only — render after lighting, particles do not
        // contribute to depth). Comparison less so they're occluded by opaque.
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable=VK_FALSE;     // sceneColor pass already finished depth-aware lighting;
                                          // we just want additive overlay sorted by emission order.
        ds.depthWriteEnable=VK_FALSE;

        // Premultiplied alpha blend over sceneColor.
        VkPipelineColorBlendAttachmentState att{};
        att.blendEnable=VK_TRUE;
        att.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
        att.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.colorBlendOp=VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
        att.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.alphaBlendOp=VK_BLEND_OP_ADD;
        att.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                           VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount=1; cb.pAttachments=&att;

        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynState{};
        dynState.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynState.dynamicStateCount=2; dynState.pDynamicStates=dyn;

        const VkFormat sceneFmt = VK_FORMAT_R16G16B16A16_SFLOAT;  // m_sceneColor format
        VkPipelineRenderingCreateInfo render{};
        render.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        render.colorAttachmentCount=1; render.pColorAttachmentFormats=&sceneFmt;

        VkGraphicsPipelineCreateInfo info{};
        info.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext=&render;
        info.stageCount=2; info.pStages=stages;
        info.pVertexInputState=&vi;
        info.pInputAssemblyState=&ia;
        info.pViewportState=&vp;
        info.pRasterizationState=&rs;
        info.pMultisampleState=&ms;
        info.pDepthStencilState=&ds;
        info.pColorBlendState=&cb;
        info.pDynamicState=&dynState;
        info.layout=m_particleRenderPipelineLayout;
        VkResult r = vkCreateGraphicsPipelines(m_device.Handle(), m_pipelineCache, 1,
                                               &info, nullptr, &m_particleRenderPipeline);
        vkDestroyShaderModule(m_device.Handle(), vs, nullptr);
        vkDestroyShaderModule(m_device.Handle(), fs, nullptr);
        if (r != VK_SUCCESS) return false;
    }
    return true;
}

bool VulkanContext::createParticleDescriptors() {
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool=m_descriptorPool;
    alloc.descriptorSetCount=1; alloc.pSetLayouts=&m_particleSsboSetLayout;
    if (vkAllocateDescriptorSets(m_device.Handle(), &alloc, &m_particleSsboSet) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] alloc particle ssbo set failed\n");
        return false;
    }
    VkDescriptorBufferInfo bi{};
    bi.buffer=m_particleBuffer.Handle();
    bi.range=VK_WHOLE_SIZE;
    VkWriteDescriptorSet w{};
    w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet=m_particleSsboSet;
    w.dstBinding=0; w.descriptorCount=1;
    w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo=&bi;
    vkUpdateDescriptorSets(m_device.Handle(), 1, &w, 0, nullptr);
    return true;
}

uint32_t VulkanContext::RegisterParticleEmitter(const ParticleEmitterDesc& desc) {
    // Reserve a contiguous slot range for this emitter inside the SSBO. We
    // bail if the cumulative allocations would exceed the global capacity —
    // bumping kCapacity is the right fix once we hit that ceiling.
    uint32_t base = 0;
    for (const auto& e : m_particleEmitters) base += static_cast<uint32_t>(e.desc.maxParticles);
    const uint32_t requested = static_cast<uint32_t>(desc.maxParticles);
    if (base + requested > m_particleCapacity) {
        std::fprintf(stderr, "[Vulkan] particle SSBO full — emitter rejected (would use %u/%u)\n",
                     base + requested, m_particleCapacity);
        return UINT32_MAX;
    }
    ParticleEmitterState s{};
    s.desc        = desc;
    s.accumulator = 0.0f;
    s.spawnCursor = 0;
    s.baseSlot    = base;
    m_particleEmitters.push_back(std::move(s));
    return static_cast<uint32_t>(m_particleEmitters.size() - 1);
}

void VulkanContext::UpdateParticleEmitter(uint32_t index, const ParticleEmitterDesc& desc) {
    if (index >= m_particleEmitters.size()) return;
    // Slots were reserved (baseSlot/maxParticles) at register time — keep them so
    // the SSBO range stays valid; everything else (incl. position) can change.
    const int keepMax = m_particleEmitters[index].desc.maxParticles;
    m_particleEmitters[index].desc = desc;
    m_particleEmitters[index].desc.maxParticles = keepMax;
}

VulkanContext::GpuLight VulkanContext::packLight(const LightDesc& desc) const {
    GpuLight lt{};
    lt.posRange       = glm::vec4(desc.position, desc.range);
    lt.colorIntensity = glm::vec4(desc.color, desc.intensity);
    if (desc.type == 1) {        // spot
        lt.dirType = glm::vec4(glm::normalize(desc.direction), 1.0f);
        lt.spotCos = glm::vec4(glm::cos(glm::radians(desc.innerDegrees)),
                               glm::cos(glm::radians(desc.outerDegrees)), 0.0f, 0.0f);
    } else if (desc.type == 2) { // area sphere
        lt.dirType = glm::vec4(0.0f, -1.0f, 0.0f, 2.0f);
        lt.spotCos = glm::vec4(desc.radius, 0.0f, 0.0f, 0.0f);
    } else {                     // point
        lt.dirType = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
        lt.spotCos = glm::vec4(0.0f);
    }
    return lt;
}

void VulkanContext::writeLightBuffer(BufferVk& buf) const {
    if (buf.Mapped() == nullptr) return;
    uint8_t* base = static_cast<uint8_t*>(buf.Mapped());
    const uint32_t headerCount = m_pointLightsEnabled
        ? static_cast<uint32_t>(m_lightDescs.size()) : 0u;
    std::memcpy(base, &headerCount, sizeof(uint32_t));
    for (size_t i = 0; i < m_lightDescs.size(); ++i) {
        GpuLight lt = packLight(m_lightDescs[i]);
        std::memcpy(base + 16 + sizeof(GpuLight) * i, &lt, sizeof(GpuLight));
    }
}

uint32_t VulkanContext::RegisterPointLight(const glm::vec3& position, const glm::vec3& color,
                                           float intensity, float range) {
    if (m_lightCount >= m_lightCapacity) return UINT32_MAX;
    // The per-frame SSBO is re-packed from m_lightDescs in BeginFrame.
    m_lightDescs.push_back(LightDesc{ 0, position, color, intensity, range,
                                      glm::vec3(0.0f, -1.0f, 0.0f), 18.0f, 28.0f, 0.5f });
    return m_lightCount++;
}

uint32_t VulkanContext::RegisterSpotLight(const glm::vec3& position, const glm::vec3& direction,
                                          const glm::vec3& color, float intensity, float range,
                                          float innerDegrees, float outerDegrees) {
    if (m_lightCount >= m_lightCapacity) return UINT32_MAX;
    // The per-frame SSBO is re-packed from m_lightDescs in BeginFrame.
    m_lightDescs.push_back(LightDesc{ 1, position, color, intensity, range,
                                      glm::normalize(direction), innerDegrees, outerDegrees, 0.5f });
    return m_lightCount++;
}

uint32_t VulkanContext::RegisterAreaLight(const glm::vec3& position, const glm::vec3& color,
                                          float intensity, float range, float radius) {
    if (m_lightCount >= m_lightCapacity) return UINT32_MAX;
    // The per-frame SSBO is re-packed from m_lightDescs in BeginFrame.
    m_lightDescs.push_back(LightDesc{ 2, position, color, intensity, range,
                                      glm::vec3(0.0f, -1.0f, 0.0f), 18.0f, 28.0f, radius });
    return m_lightCount++;
}

uint32_t VulkanContext::LightCount() const {
    return static_cast<uint32_t>(m_lightDescs.size());
}

VulkanContext::LightDesc VulkanContext::GetLight(uint32_t index) const {
    return (index < m_lightDescs.size()) ? m_lightDescs[index] : LightDesc{};
}

void VulkanContext::UpdateLight(uint32_t index, const LightDesc& desc) {
    if (index >= m_lightDescs.size()) return;
    m_lightDescs[index] = desc;   // re-packed into the per-frame buffer in BeginFrame
}

VulkanContext::RenderSettingsRefs VulkanContext::GetRenderSettingsRefs() {
    // Order matches the RenderSettingsRefs struct field order (see header).
    return RenderSettingsRefs{
        // Shadows
        &m_pcssEnabled,
        &m_pcssLightSize,
        &m_csmFrustumFitEnabled,
        &m_spriteShadowsEnabled,
        // Material AA
        &m_specularAAEnabled,
        // Reflections (SSR)
        &m_ssrHalfResEnabled,
        // Bloom
        &m_bloomEnabled,
        &m_bloomThreshold,
        &m_bloomKnee,
        &m_bloomIntensity,
        &m_bloomUpsampleIntensity,
        // Volumetric fog
        &m_fogEnabled,
        &m_godRaysEnabled,
        &m_fogTemporalEnabled,
        &m_fogTemporalAlpha,
        &m_fogSteps,
        &m_fogDensityFloor,
        &m_fogDensityScale,
        &m_fogHeightFactor,
        &m_fogScatterStrength,
        &m_fogMaxDistance,
        &m_fogColor,
        // Tonemap / grading
        &m_exposure,
        &m_contrast,
        &m_saturation,
        &m_celEnabled,
        &m_celSteps,
        // Auto-exposure
        &m_autoExposureEnabled,
        &m_autoExposureKey,
        &m_autoExposureTau,
        &m_autoExposureMinLum,
        &m_autoExposureMaxLum,
        // Sharpening (CAS)
        &m_casEnabled,
        &m_casSharpness,
        // Particles
        &m_particlesEnabled,
        &m_softParticlesEnabled,
        &m_particleMotionBlurEnabled,
        &m_particleStretchScale,
        // Lights
        &m_pointLightsEnabled,
    };
}

void VulkanContext::emitParticles(float dt) {
    if (m_particleBuffer.Mapped() == nullptr) return;
    ParticleGPU* gpu = static_cast<ParticleGPU*>(m_particleBuffer.Mapped());

    auto frand = [](float lo, float hi) {
        return lo + (hi - lo) * (float(std::rand()) / float(RAND_MAX));
    };

    for (auto& e : m_particleEmitters) {
        e.accumulator += dt * e.desc.emitRate;
        int spawn = static_cast<int>(e.accumulator);
        if (spawn <= 0) continue;
        e.accumulator -= float(spawn);

        glm::vec3 d = glm::normalize(e.desc.direction);
        for (int i = 0; i < spawn; ++i) {
            const uint32_t slot = e.baseSlot + (e.spawnCursor % uint32_t(e.desc.maxParticles));
            e.spawnCursor++;

            // Cone-direction sampling: pick a small random offset in tangent
            // plane, lerp toward the principal direction by (1 - spread).
            glm::vec3 t = glm::normalize(glm::cross(d,
                std::abs(d.y) < 0.99f ? glm::vec3(0,1,0) : glm::vec3(1,0,0)));
            glm::vec3 b = glm::cross(d, t);
            glm::vec3 jitter = (t * frand(-1.0f, 1.0f) + b * frand(-1.0f, 1.0f)) * e.desc.spread;
            glm::vec3 dir = glm::normalize(d + jitter);

            float speed = frand(e.desc.minSpeed, e.desc.maxSpeed);
            float life  = frand(e.desc.minLifetime, e.desc.maxLifetime);

            ParticleGPU& p = gpu[slot];
            p.position = glm::vec4(e.desc.position, e.desc.startSize);
            p.velocity = glm::vec4(dir * speed, life);
            p.color    = e.desc.startColor;
            p.params   = glm::vec4(life, e.desc.startSize, e.desc.endSize, 1.0f);
            p.extra    = glm::vec4(e.desc.gravity, 0.0f);
        }
    }
}

// ===== Sprites (FASE 15.B) ================================================

namespace {
// Per-sprite SSBO struct — matches sprite_gbuffer.vert/frag layout.
struct SpriteInstanceGPU {
    glm::mat4 model;
    glm::vec4 color;
    glm::vec4 uvOffsetScale;
    glm::vec4 material;  // alphaClip, metallic, roughness, textureIndex(uint-bitcast)
};
static_assert(sizeof(SpriteInstanceGPU) == 64 + 16 * 3, "SpriteInstanceGPU layout");
}

void VulkanContext::updateSprites(float dt) {
    if (m_spriteAnims.empty() || m_spriteInstanceBuffer.Mapped() == nullptr) return;
    auto* gpu = static_cast<SpriteInstanceGPU*>(m_spriteInstanceBuffer.Mapped());
    for (auto& a : m_spriteAnims) {
        a.time += dt;
        const uint32_t step = static_cast<uint32_t>(a.time * a.frameRate);
        uint32_t frame = a.looping
            ? a.frameStart + (step % a.frameCount)
            : a.frameStart + std::min(step, a.frameCount - 1u);
        const uint32_t col = frame % a.sheetCols;
        const uint32_t row = frame / a.sheetCols;
        gpu[a.spriteIndex].uvOffsetScale = glm::vec4(
            float(col) / float(a.sheetCols), float(row) / float(a.sheetRows),
            1.0f / float(a.sheetCols), 1.0f / float(a.sheetRows));
    }
}

bool VulkanContext::createSpriteResources() {
    // Reusable unit quad. Y-up plane, normal = +Z (sprite faces toward +Z).
    // Host transforms each sprite via SpriteDesc.model.
    const MeshVertex quad[4] = {
        // pos               normal           uv
        { {-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f} },
        { { 0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f} },
        { { 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f} },
        { {-0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f} },
    };
    const uint32_t indices[6] = { 0, 1, 2,  0, 2, 3 };

    // Vertex/index buffers via staging.
    const VkDeviceSize vbBytes = sizeof(quad);
    const VkDeviceSize ibBytes = sizeof(indices);
    if (!m_spriteQuadVertices.CreateDeviceLocal(m_allocator.Handle(), vbBytes,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return false;
    if (!m_spriteQuadIndices.CreateDeviceLocal(m_allocator.Handle(), ibBytes,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return false;

    BufferVk stagingV, stagingI;
    if (!stagingV.CreateStaging(m_allocator.Handle(), vbBytes)) return false;
    if (!stagingI.CreateStaging(m_allocator.Handle(), ibBytes)) return false;
    std::memcpy(stagingV.Mapped(), quad,    static_cast<size_t>(vbBytes));
    std::memcpy(stagingI.Mapped(), indices, static_cast<size_t>(ibBytes));

    OneShotCmd one(m_device.Handle(), m_device.GraphicsQueueFamily(), m_device.GraphicsQueue());
    BufferVk::CmdCopy(one.cmd(), stagingV, m_spriteQuadVertices, vbBytes);
    BufferVk::CmdCopy(one.cmd(), stagingI, m_spriteQuadIndices,  ibBytes);

    // SSBO of sprite instances, host-coherent so we can update from CPU
    // each frame if needed.
    m_spriteCapacity = 1024;
    const VkDeviceSize ssboBytes = sizeof(SpriteInstanceGPU) * m_spriteCapacity;
    if (!m_spriteInstanceBuffer.CreateHostCoherent(m_allocator.Handle(), ssboBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return false;
    if (m_spriteInstanceBuffer.Mapped()) {
        std::memset(m_spriteInstanceBuffer.Mapped(), 0, static_cast<size_t>(ssboBytes));
    }
    m_spriteCount = 0;
    return true;
}

void VulkanContext::destroySpriteResources() {
    m_spriteQuadVertices.Destroy();
    m_spriteQuadIndices.Destroy();
    m_spriteInstanceBuffer.Destroy();
    m_spriteCapacity = 0;
    m_spriteCount    = 0;
    m_spriteAnims.clear();
}

bool VulkanContext::createSpritePipeline() {
    // Set 2 = sprite SSBO (binding 0). Set 0 and 1 reuse frame UBO + bindless.
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount=1; info.pBindings=&b;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &info, nullptr,
                                        &m_spriteSsboSetLayout) != VK_SUCCESS) return false;
    }

    VkDescriptorSetLayout layouts[3] = {
        m_frameSetLayout, m_bindlessSetLayout, m_spriteSsboSetLayout,
    };
    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount=3; plInfo.pSetLayouts=layouts;
    if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                               &m_spritePipelineLayout) != VK_SUCCESS) return false;

    VkShaderModule vs = loadShaderModule("Shaders/SPV/sprite_gbuffer.vert.spv");
    VkShaderModule fs = loadShaderModule("Shaders/SPV/sprite_gbuffer.frag.spv");
    if (!vs || !fs) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vs; stages[0].pName="main";
    stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fs; stages[1].pName="main";

    // Vertex input = MeshVertex (pos vec3, normal vec3, uv vec2).
    VkVertexInputBindingDescription vbind{};
    vbind.binding=0; vbind.stride=sizeof(MeshVertex);
    vbind.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription vattr[3]{};
    vattr[0]={ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, pos)    };
    vattr[1]={ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal) };
    vattr[2]={ 2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(MeshVertex, uv)     };
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount=1; vi.pVertexBindingDescriptions=&vbind;
    vi.vertexAttributeDescriptionCount=3; vi.pVertexAttributeDescriptions=vattr;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount=1; vp.scissorCount=1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode=VK_POLYGON_MODE_FILL;
    rs.cullMode=VK_CULL_MODE_NONE;       // sprites are double-sided
    rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable=VK_TRUE; ds.depthWriteEnable=VK_TRUE;
    ds.depthCompareOp=VK_COMPARE_OP_LESS;

    // 5 G-Buffer color attachments — same formats as geometry pipeline.
    VkPipelineColorBlendAttachmentState atts[5]{};
    for (int i = 0; i < 5; ++i) atts[i] = makeOpaqueBlend();
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount=5; cb.pAttachments=atts;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount=2; dynState.pDynamicStates=dyn;

    const VkFormat colorFormats[5] = {
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R8G8B8A8_UNORM,      VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R16G16_SFLOAT,
    };
    VkPipelineRenderingCreateInfo render{};
    render.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    render.colorAttachmentCount=5; render.pColorAttachmentFormats=colorFormats;
    render.depthAttachmentFormat = kDepthFormat;

    VkGraphicsPipelineCreateInfo info{};
    info.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext=&render;
    info.stageCount=2; info.pStages=stages;
    info.pVertexInputState=&vi;
    info.pInputAssemblyState=&ia;
    info.pViewportState=&vp;
    info.pRasterizationState=&rs;
    info.pMultisampleState=&ms;
    info.pDepthStencilState=&ds;
    info.pColorBlendState=&cb;
    info.pDynamicState=&dynState;
    info.layout=m_spritePipelineLayout;
    VkResult r = vkCreateGraphicsPipelines(m_device.Handle(), m_pipelineCache, 1,
                                           &info, nullptr, &m_spritePipeline);
    vkDestroyShaderModule(m_device.Handle(), vs, nullptr);
    vkDestroyShaderModule(m_device.Handle(), fs, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] sprite pipeline create failed: %d\n", r);
        return false;
    }
    return true;
}

bool VulkanContext::createSpriteShadowPipeline() {
    // Alpha-tested billboard shadow casting. Reuses set 0 = bindless textures,
    // set 1 = sprite SSBO. Push constant = cascade light-space matrix only.
    // Two stages (vert + frag) — the fragment does the atlas alpha discard so
    // the sprite casts a cut-out shadow rather than a solid quad.
    VkDescriptorSetLayout layouts[2] = { m_bindlessSetLayout, m_spriteSsboSetLayout };

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(glm::mat4);  // lightSpace

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 2;
    plInfo.pSetLayouts            = layouts;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                               &m_spriteShadowPipelineLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] sprite shadow pipelineLayout failed\n");
        return false;
    }

    VkShaderModule vs = loadShaderModule("Shaders/SPV/sprite_shadow.vert.spv");
    VkShaderModule fs = loadShaderModule("Shaders/SPV/sprite_shadow.frag.spv");
    if (!vs || !fs) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vs; stages[0].pName="main";
    stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fs; stages[1].pName="main";

    // Vertex input = MeshVertex (pos, normal, uv) — the frag needs uv for the
    // atlas alpha lookup, so all three attributes are bound (unlike the mesh
    // shadow pipeline which only feeds position).
    VkVertexInputBindingDescription vbind{};
    vbind.binding=0; vbind.stride=sizeof(MeshVertex);
    vbind.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription vattr[3]{};
    vattr[0]={ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, pos)    };
    vattr[1]={ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal) };
    vattr[2]={ 2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(MeshVertex, uv)     };
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount=1; vi.pVertexBindingDescriptions=&vbind;
    vi.vertexAttributeDescriptionCount=3; vi.pVertexAttributeDescriptions=vattr;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount=1; vp.scissorCount=1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode=VK_POLYGON_MODE_FILL;
    rs.cullMode=VK_CULL_MODE_NONE;       // double-sided billboards
    rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1.0f;
    rs.depthBiasEnable=VK_TRUE;
    rs.depthBiasConstantFactor=1.25f;
    rs.depthBiasSlopeFactor=1.75f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable=VK_TRUE; ds.depthWriteEnable=VK_TRUE;
    ds.depthCompareOp=VK_COMPARE_OP_LESS;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount=0;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount=2; dynState.pDynamicStates=dyn;

    VkPipelineRenderingCreateInfo render{};
    render.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    render.colorAttachmentCount=0;
    render.depthAttachmentFormat = kDepthFormat;

    VkGraphicsPipelineCreateInfo info{};
    info.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext=&render;
    info.stageCount=2; info.pStages=stages;
    info.pVertexInputState=&vi;
    info.pInputAssemblyState=&ia;
    info.pViewportState=&vp;
    info.pRasterizationState=&rs;
    info.pMultisampleState=&ms;
    info.pDepthStencilState=&ds;
    info.pColorBlendState=&cb;
    info.pDynamicState=&dynState;
    info.layout=m_spriteShadowPipelineLayout;
    VkResult r = vkCreateGraphicsPipelines(m_device.Handle(), m_pipelineCache, 1,
                                           &info, nullptr, &m_spriteShadowPipeline);
    vkDestroyShaderModule(m_device.Handle(), vs, nullptr);
    vkDestroyShaderModule(m_device.Handle(), fs, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] sprite shadow pipeline create failed: %d\n", r);
        return false;
    }
    return true;
}

bool VulkanContext::createSpriteDescriptors() {
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool=m_descriptorPool;
    alloc.descriptorSetCount=1; alloc.pSetLayouts=&m_spriteSsboSetLayout;
    if (vkAllocateDescriptorSets(m_device.Handle(), &alloc, &m_spriteSsboSet) != VK_SUCCESS) return false;

    VkDescriptorBufferInfo bi{};
    bi.buffer=m_spriteInstanceBuffer.Handle();
    bi.range=VK_WHOLE_SIZE;
    VkWriteDescriptorSet w{};
    w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet=m_spriteSsboSet;
    w.dstBinding=0; w.descriptorCount=1;
    w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo=&bi;
    vkUpdateDescriptorSets(m_device.Handle(), 1, &w, 0, nullptr);
    return true;
}

uint32_t VulkanContext::RegisterSpriteTexture(const std::string& path) {
    return loadOrGetTexture(path, VK_FORMAT_R8G8B8A8_SRGB);
}

uint32_t VulkanContext::RegisterSprite(const SpriteDesc& desc) {
    if (m_spriteCount >= m_spriteCapacity || m_spriteInstanceBuffer.Mapped() == nullptr) {
        std::fprintf(stderr, "[Vulkan] sprite SSBO full (%u/%u)\n",
                     m_spriteCount, m_spriteCapacity);
        return UINT32_MAX;
    }
    auto* gpu = static_cast<SpriteInstanceGPU*>(m_spriteInstanceBuffer.Mapped());
    SpriteInstanceGPU s{};
    s.model         = desc.model;
    s.color         = desc.color;
    s.uvOffsetScale = desc.uvOffsetScale;
    // Pack textureIndex into the .w slot via uintBitsToFloat (round-trips
    // bit-exactly through a 32-bit storage buffer write).
    const float texBits = *reinterpret_cast<const float*>(&desc.textureIndex);
    s.material      = glm::vec4(desc.alphaClip, desc.metallic, desc.roughness, texBits);

    // Atlas animation: if animating, snap uvOffsetScale to the first frame and
    // register a CPU-side animation entry that updateSprites() advances.
    auto frameUV = [](uint32_t frame, uint32_t cols, uint32_t rows) {
        uint32_t col = frame % cols;
        uint32_t row = frame / cols;
        return glm::vec4(float(col) / float(cols), float(row) / float(rows),
                         1.0f / float(cols), 1.0f / float(rows));
    };
    if (desc.frameRate > 0.0f && desc.frameCount > 1) {
        const uint32_t cols = (desc.sheetCols  > 0) ? desc.sheetCols  : 1u;
        const uint32_t rows = (desc.sheetRows  > 0) ? desc.sheetRows  : 1u;
        s.uvOffsetScale = frameUV(desc.frameStart, cols, rows);
        m_spriteAnims.push_back({ m_spriteCount, cols, rows,
                                  desc.frameStart, desc.frameCount,
                                  desc.frameRate, desc.looping, 0.0f });
    }

    gpu[m_spriteCount] = s;
    return m_spriteCount++;
}

void VulkanContext::UpdateSprite(uint32_t index, const SpriteDesc& desc) {
    if (index >= m_spriteCount || m_spriteInstanceBuffer.Mapped() == nullptr) return;
    auto* gpu = static_cast<SpriteInstanceGPU*>(m_spriteInstanceBuffer.Mapped());
    gpu[index].model = desc.model;
    gpu[index].color = desc.color;
    // Leave uvOffsetScale alone — RegisterSprite seeds it and updateSprites() owns
    // it for animated sheets, so we don't fight the per-frame frame advance here.
    // Pack textureIndex bit-exactly into .w (matches RegisterSprite).
    const float texBits = *reinterpret_cast<const float*>(&desc.textureIndex);
    gpu[index].material = glm::vec4(desc.alphaClip, desc.metallic, desc.roughness, texBits);
}

void VulkanContext::ClearLights() {
    m_lightDescs.clear();
    m_lightCount = 0;
}

void VulkanContext::ClearParticleEmitters() {
    // Emitter params only; alive particles in the SSBO simply finish their life.
    m_particleEmitters.clear();
}

void VulkanContext::ClearSprites() {
    // Count → 0 means none are drawn; the next RegisterSprite reuses row 0.
    m_spriteCount = 0;
    m_spriteAnims.clear();
}

bool VulkanContext::createShadowResources() {
    // D32 array, kCascadeCount layers. SAMPLED so lighting pass can read it,
    // DEPTH_STENCIL_ATTACHMENT so we can render to each layer.
    ImageCreateParams p{};
    p.width       = kShadowMapSize;
    p.height      = kShadowMapSize;
    p.format      = kDepthFormat;
    p.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    p.aspect      = VK_IMAGE_ASPECT_DEPTH_BIT;
    p.arrayLayers = kCascadeCount;
    if (!m_shadowMap.Create(m_device.Handle(), m_allocator.Handle(), p)) return false;

    // Per-layer write views — one for each cascade, used as depth attachment
    // in the geometry-style render of that cascade.
    for (uint32_t i = 0; i < kCascadeCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image    = m_shadowMap.Handle();
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format   = kDepthFormat;
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.baseMipLevel   = 0;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = i;
        viewInfo.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(m_device.Handle(), &viewInfo, nullptr,
                              &m_shadowLayerViews[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] shadow layer view %u failed\n", i);
            return false;
        }
    }

    // Linear PCF + compareOp LESS gives free 2×2 percentage-closer filtering.
    // CLAMP_TO_BORDER with opaque-white border = "outside shadow map = no shadow".
    VkSamplerCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter    = VK_FILTER_LINEAR;
    info.minFilter    = VK_FILTER_LINEAR;
    info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    info.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    info.compareEnable = VK_TRUE;
    info.compareOp     = VK_COMPARE_OP_LESS;
    if (vkCreateSampler(m_device.Handle(), &info, nullptr, &m_shadowSampler) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] shadow sampler create failed\n");
        return false;
    }

    // PCSS blocker search needs the RAW depth, not a comparison result, so it uses
    // a second sampler with compareEnable OFF and NEAREST filtering (averaging
    // depths across blockers/non-blockers would corrupt the average-blocker-depth
    // estimate). Same CLAMP_TO_BORDER white so off-map taps read depth 1.0 (far =
    // not a blocker). Reuses the very same m_shadowMap image view.
    info.magFilter     = VK_FILTER_NEAREST;
    info.minFilter     = VK_FILTER_NEAREST;
    info.compareEnable = VK_FALSE;
    if (vkCreateSampler(m_device.Handle(), &info, nullptr, &m_shadowDepthSampler) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] shadow depth sampler create failed\n");
        return false;
    }
    return true;
}

void VulkanContext::destroyShadowResources() {
    for (auto& v : m_shadowLayerViews) {
        if (v) vkDestroyImageView(m_device.Handle(), v, nullptr);
        v = VK_NULL_HANDLE;
    }
    m_shadowMap.Destroy();
}

bool VulkanContext::createShadowPipeline() {
    // ---- Layout: just a push constant (lightSpace + model), no descriptors ---
    struct ShadowPC {
        glm::mat4 lightSpace;
        glm::mat4 model;
    };
    static_assert(sizeof(ShadowPC) == 128, "ShadowPC must fit the 128-byte push range");

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(ShadowPC);

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr,
                               &m_shadowPipelineLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] shadow pipelineLayout failed\n");
        return false;
    }

    VkShaderModule vs = loadShaderModule("Shaders/SPV/shadow.vert.spv");
    if (!vs) return false;

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vs;
    stage.pName  = "main";

    // Vertex input — only POSITION, the rest of MeshVertex is ignored.
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding   = 0;
    bindingDesc.stride    = sizeof(MeshVertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attr{};
    attr.location = 0;
    attr.binding  = 0;
    attr.format   = VK_FORMAT_R32G32B32_SFLOAT;
    attr.offset   = offsetof(MeshVertex, pos);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions    = &attr;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_FRONT_BIT;  // peter-panning fix: shadow off the back faces
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    rs.depthBiasEnable    = VK_TRUE;
    rs.depthBiasConstantFactor = 1.25f;
    rs.depthBiasSlopeFactor    = 1.75f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 0;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dyn;

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType                 = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount  = 0;
    renderingInfo.depthAttachmentFormat = kDepthFormat;

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &renderingInfo;
    info.stageCount          = 1;
    info.pStages             = &stage;
    info.pVertexInputState   = &vertexInput;
    info.pInputAssemblyState = &ia;
    info.pViewportState      = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState   = &ms;
    info.pDepthStencilState  = &ds;
    info.pColorBlendState    = &cb;
    info.pDynamicState       = &dynState;
    info.layout              = m_shadowPipelineLayout;

    VkResult r = vkCreateGraphicsPipelines(m_device.Handle(), m_pipelineCache, 1,
                                           &info, nullptr, &m_shadowPipeline);
    vkDestroyShaderModule(m_device.Handle(), vs, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] shadow pipeline create failed: %d\n", r);
        return false;
    }
    return true;
}

bool VulkanContext::createIBLResources() {
    auto makeCubeArray = [&](ImageVk& out, uint32_t size, uint32_t mips = 1,
                             VkImageUsageFlags extraUsage = 0) {
        ImageCreateParams p{};
        p.width       = size;
        p.height      = size;
        p.format      = VK_FORMAT_R16G16B16A16_SFLOAT;
        p.usage       = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extraUsage;
        p.aspect      = VK_IMAGE_ASPECT_COLOR_BIT;
        p.arrayLayers = 6;
        p.mipLevels   = mips;
        return out.Create(m_device.Handle(), m_allocator.Handle(), p);
    };
    // The env cubemap gets a full mip chain (built by image blits, hence the
    // TRANSFER usage) so the GGX prefilter can read a pre-blurred level per
    // importance-sample PDF — that's what tames the sun-pixel fireflies.
    if (!makeCubeArray(m_envCubemap, kEnvCubeSize, kEnvCubeMips,
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
    if (!makeCubeArray(m_irradianceCubemap,  kIrradianceSize))                    return false;
    if (!makeCubeArray(m_prefilteredCubemap, kPrefilteredSize, kPrefilteredMips)) return false;

    // Single-mip storage view: the equirect→cube compute writes mip 0 through
    // this. m_envCubemap.View() spans all mips and is for sampling only.
    {
        VkImageViewCreateInfo v{};
        v.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        v.image    = m_envCubemap.Handle();
        v.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        v.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
        v.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
        if (vkCreateImageView(m_device.Handle(), &v, nullptr, &m_envCubemapMip0View) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] env cubemap mip0 view create failed\n");
            return false;
        }
    }

    // BRDF LUT — 2D, single layer, R16G16_SFLOAT.
    {
        ImageCreateParams p{};
        p.width  = kBrdfLutSize;
        p.height = kBrdfLutSize;
        p.format = VK_FORMAT_R16G16_SFLOAT;
        p.usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        if (!m_brdfLut.Create(m_device.Handle(), m_allocator.Handle(), p)) return false;
    }

    // Per-mip storage views into the prefiltered cubemap (one VIEW_TYPE_2D_ARRAY
    // covering all 6 layers per mip).
    for (uint32_t i = 0; i < kPrefilteredMips; ++i) {
        VkImageViewCreateInfo vi{};
        vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image    = m_prefilteredCubemap.Handle();
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.format   = VK_FORMAT_R16G16B16A16_SFLOAT;
        vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel   = i;
        vi.subresourceRange.levelCount     = 1;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount     = 6;
        if (vkCreateImageView(m_device.Handle(), &vi, nullptr,
                              &m_prefilteredMipViews[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] prefiltered mip view %u failed\n", i);
            return false;
        }
    }

    VkSamplerCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter    = VK_FILTER_LINEAR;
    info.minFilter    = VK_FILTER_LINEAR;
    info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    // The prefiltered cubemap stores roughness across kPrefilteredMips levels and
    // the lighting pass selects one with textureLod(prefiltered, R, roughness*N).
    // A default maxLod of 0 clamps EVERY explicit-LOD fetch to mip 0, so rough
    // reflections were silently sharp. Let the sampler reach every mip (each
    // image clamps to its own real mip count, so the 1-mip irradiance/env cubes
    // are unaffected).
    info.minLod       = 0.0f;
    info.maxLod       = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(m_device.Handle(), &info, nullptr, &m_iblSampler) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] IBL sampler create failed\n");
        return false;
    }

    // Load the equirectangular HDRI for image-based lighting. stbi_loadf returns
    // linear float RGB(A) — exactly what we want (no sRGB decode). Falls back to a
    // flat sky tint if the file is missing so the pipeline still runs.
    {
        int hw = 0, hh = 0, hc = 0;
        float* hdr = stbi_loadf("Assets/Textures/kloofendal_48d_partly_cloudy_puresky_4k.hdr",
                                &hw, &hh, &hc, STBI_rgb_alpha);
        std::vector<float> fallback;
        const float* src = hdr;
        int ew = hw, eh = hh;
        if (!hdr) {
            std::fprintf(stderr, "[Vulkan] HDRI load failed — using flat sky fallback\n");
            ew = 2; eh = 1;
            fallback = { 0.45f,0.55f,0.80f,1.0f,  0.45f,0.55f,0.80f,1.0f };  // 2x1 RGBA
            src = fallback.data();
        }
        const VkDeviceSize bytes = VkDeviceSize(ew) * eh * 4 * sizeof(float);
        BufferVk staging;
        if (!staging.CreateStaging(m_allocator.Handle(), bytes)) { if (hdr) stbi_image_free(hdr); return false; }
        std::memcpy(staging.Mapped(), src, size_t(bytes));
        if (hdr) stbi_image_free(hdr);

        // The env bake samples this equirect with a LINEAR sampler, but LINEAR
        // filtering of R32G32B32A32_SFLOAT is OPTIONAL in Vulkan. Desktop discrete
        // GPUs support it (so we keep the simple full-float path), but warn loudly
        // if a device lacks it — the symptom there is undefined sampling, typically
        // a black/garbage IBL, and this log points straight at the cause.
        {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(m_device.PhysicalHandle(),
                                                VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
                std::fprintf(stderr,
                    "[Vulkan] WARNING: this GPU does not support LINEAR filtering of "
                    "R32G32B32A32_SFLOAT; the HDRI equirect is sampled with LINEAR during "
                    "IBL bake -> expect black/garbage IBL. Needs an RGBA16F fallback path.\n");
            }
        }

        ImageCreateParams p{};
        p.width  = uint32_t(ew);
        p.height = uint32_t(eh);
        p.format = VK_FORMAT_R32G32B32A32_SFLOAT;  // match the float staging data
        p.usage  = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        if (!m_envEquirect.Create(m_device.Handle(), m_allocator.Handle(), p)) return false;

        OneShotCmd one(m_device.Handle(), m_device.GraphicsQueueFamily(), m_device.GraphicsQueue());
        m_envEquirect.CmdTransition(one.cmd(),
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        m_envEquirect.CmdCopyFromBuffer(one.cmd(), staging.Handle());
        m_envEquirect.CmdTransition(one.cmd(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        // OneShotCmd dtor submits + waits.
    }
    return true;
}

void VulkanContext::destroyIBLResources() {
    for (auto& v : m_prefilteredMipViews) {
        if (v) vkDestroyImageView(m_device.Handle(), v, nullptr);
        v = VK_NULL_HANDLE;
    }
    if (m_envCubemapMip0View) {
        vkDestroyImageView(m_device.Handle(), m_envCubemapMip0View, nullptr);
        m_envCubemapMip0View = VK_NULL_HANDLE;
    }
    m_envEquirect.Destroy();
    m_envCubemap.Destroy();
    m_irradianceCubemap.Destroy();
    m_prefilteredCubemap.Destroy();
    m_brdfLut.Destroy();
}

bool VulkanContext::createIBLPipelines() {
    // ---- Env equirect→cube: combined sampler (equirect HDRI) + storage (cube) ----
    {
        VkDescriptorSetLayoutBinding bs[2]{};
        bs[0].binding         = 0;
        bs[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bs[0].descriptorCount = 1;
        bs[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bs[1].binding         = 1;
        bs[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bs[1].descriptorCount = 1;
        bs[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2;
        li.pBindings    = bs;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &li, nullptr, &m_envSetLayout) != VK_SUCCESS) return false;

        VkPipelineLayoutCreateInfo pl{};
        pl.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts    = &m_envSetLayout;
        if (vkCreatePipelineLayout(m_device.Handle(), &pl, nullptr, &m_envPipelineLayout) != VK_SUCCESS) return false;

        VkShaderModule cs = loadShaderModule("Shaders/SPV/equirect_to_cubemap.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = cs;
        stage.pName  = "main";
        VkComputePipelineCreateInfo ci{};
        ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage  = stage;
        ci.layout = m_envPipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1, &ci, nullptr, &m_envPipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) return false;
    }

    // ---- Irradiance: combined sampler (env) + storage (irradiance) ----
    {
        VkDescriptorSetLayoutBinding bs[2]{};
        bs[0].binding         = 0;
        bs[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bs[0].descriptorCount = 1;
        bs[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bs[1].binding         = 1;
        bs[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bs[1].descriptorCount = 1;
        bs[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2;
        li.pBindings    = bs;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &li, nullptr, &m_irradianceSetLayout) != VK_SUCCESS) return false;

        VkPipelineLayoutCreateInfo pl{};
        pl.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts    = &m_irradianceSetLayout;
        if (vkCreatePipelineLayout(m_device.Handle(), &pl, nullptr, &m_irradiancePipelineLayout) != VK_SUCCESS) return false;

        VkShaderModule cs = loadShaderModule("Shaders/SPV/ibl_irradiance.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = cs;
        stage.pName  = "main";
        VkComputePipelineCreateInfo ci{};
        ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage  = stage;
        ci.layout = m_irradiancePipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1, &ci, nullptr, &m_irradiancePipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) return false;
    }

    // ---- Prefilter compute (same layout as irradiance: sampler + storage) + push constant ----
    {
        VkDescriptorSetLayoutBinding bs[2]{};
        bs[0].binding         = 0;
        bs[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bs[0].descriptorCount = 1;
        bs[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bs[1].binding         = 1;
        bs[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bs[1].descriptorCount = 1;
        bs[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2;
        li.pBindings    = bs;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &li, nullptr, &m_prefilterSetLayout) != VK_SUCCESS) return false;

        struct PrefilterPC { float roughness; uint32_t mipSize; uint32_t envSize; };
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pc.offset     = 0;
        pc.size       = sizeof(PrefilterPC);

        VkPipelineLayoutCreateInfo pl{};
        pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount         = 1;
        pl.pSetLayouts            = &m_prefilterSetLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges    = &pc;
        if (vkCreatePipelineLayout(m_device.Handle(), &pl, nullptr, &m_prefilterPipelineLayout) != VK_SUCCESS) return false;

        VkShaderModule cs = loadShaderModule("Shaders/SPV/ibl_prefilter.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = cs;
        stage.pName  = "main";
        VkComputePipelineCreateInfo ci{};
        ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage  = stage;
        ci.layout = m_prefilterPipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1, &ci, nullptr, &m_prefilterPipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) return false;
    }

    // ---- BRDF LUT compute: 1 storage image, no input ----
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1;
        li.pBindings    = &b;
        if (vkCreateDescriptorSetLayout(m_device.Handle(), &li, nullptr, &m_brdfLutSetLayout) != VK_SUCCESS) return false;

        VkPipelineLayoutCreateInfo pl{};
        pl.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts    = &m_brdfLutSetLayout;
        if (vkCreatePipelineLayout(m_device.Handle(), &pl, nullptr, &m_brdfLutPipelineLayout) != VK_SUCCESS) return false;

        VkShaderModule cs = loadShaderModule("Shaders/SPV/ibl_brdflut.comp.spv");
        if (!cs) return false;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = cs;
        stage.pName  = "main";
        VkComputePipelineCreateInfo ci{};
        ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage  = stage;
        ci.layout = m_brdfLutPipelineLayout;
        VkResult r = vkCreateComputePipelines(m_device.Handle(), m_pipelineCache, 1, &ci, nullptr, &m_brdfLutPipeline);
        vkDestroyShaderModule(m_device.Handle(), cs, nullptr);
        if (r != VK_SUCCESS) return false;
    }

    // ---- Allocate + write all compute sets in one shot ----
    {
        std::vector<VkDescriptorSetLayout> layouts;
        layouts.push_back(m_envSetLayout);
        layouts.push_back(m_irradianceSetLayout);
        for (uint32_t i = 0; i < kPrefilteredMips; ++i) layouts.push_back(m_prefilterSetLayout);
        layouts.push_back(m_brdfLutSetLayout);

        std::vector<VkDescriptorSet> sets(layouts.size(), VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool     = m_descriptorPool;
        alloc.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        alloc.pSetLayouts        = layouts.data();
        if (vkAllocateDescriptorSets(m_device.Handle(), &alloc, sets.data()) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] alloc IBL descriptor sets failed\n");
            return false;
        }
        m_envSet        = sets[0];
        m_irradianceSet = sets[1];
        for (uint32_t i = 0; i < kPrefilteredMips; ++i) m_prefilterSets[i] = sets[2 + i];
        m_brdfLutSet    = sets[2 + kPrefilteredMips];

        VkDescriptorImageInfo envStorage{};
        envStorage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        envStorage.imageView   = m_envCubemapMip0View;  // storage write targets mip 0 only

        // Equirect HDRI source for the env compute (binding 0, sampled).
        VkDescriptorImageInfo envEquirect{};
        envEquirect.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        envEquirect.imageView   = m_envEquirect.View();
        envEquirect.sampler     = m_iblSampler;

        VkDescriptorImageInfo envSampled{};
        envSampled.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        envSampled.imageView   = m_envCubemap.View();
        envSampled.sampler     = m_iblSampler;

        VkDescriptorImageInfo irrStorage{};
        irrStorage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        irrStorage.imageView   = m_irradianceCubemap.View();

        VkDescriptorImageInfo brdfStorage{};
        brdfStorage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        brdfStorage.imageView   = m_brdfLut.View();

        std::vector<VkWriteDescriptorSet> writes;
        std::vector<VkDescriptorImageInfo> prefilterStorage(kPrefilteredMips);
        std::vector<VkDescriptorImageInfo> prefilterSampled(kPrefilteredMips);

        auto pushWrite = [&](VkDescriptorSet ds, uint32_t binding,
                             VkDescriptorType type, VkDescriptorImageInfo* info) {
            VkWriteDescriptorSet w{};
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = ds;
            w.dstBinding      = binding;
            w.descriptorCount = 1;
            w.descriptorType  = type;
            w.pImageInfo      = info;
            writes.push_back(w);
        };

        pushWrite(m_envSet,        0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &envEquirect);
        pushWrite(m_envSet,        1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          &envStorage);
        pushWrite(m_irradianceSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &envSampled);
        pushWrite(m_irradianceSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          &irrStorage);

        for (uint32_t i = 0; i < kPrefilteredMips; ++i) {
            prefilterSampled[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            prefilterSampled[i].imageView   = m_envCubemap.View();
            prefilterSampled[i].sampler     = m_iblSampler;
            prefilterStorage[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            prefilterStorage[i].imageView   = m_prefilteredMipViews[i];
            pushWrite(m_prefilterSets[i], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &prefilterSampled[i]);
            pushWrite(m_prefilterSets[i], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          &prefilterStorage[i]);
        }

        pushWrite(m_brdfLutSet, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &brdfStorage);

        vkUpdateDescriptorSets(m_device.Handle(),
                               static_cast<uint32_t>(writes.size()), writes.data(),
                               0, nullptr);
    }
    return true;
}

bool VulkanContext::precomputeIBL() {
    // Runs once at init time — record into a transient command buffer, submit,
    // wait idle, throw the pool away. Both cubemaps end up in SHADER_READ_ONLY
    // so the lighting pass can sample them every frame.
    OneShotCmd one(m_device.Handle(), m_device.GraphicsQueueFamily(), m_device.GraphicsQueue());
    VkCommandBuffer c = one.cmd();

    auto barrier = [&](VkImage img, VkImageLayout oldL, VkImageLayout newL,
                       VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                       uint32_t mipLevels = 1, uint32_t layers = 6) {
        VkImageMemoryBarrier2 b{};
        b.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask  = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask  = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout     = oldL;
        b.newLayout     = newL;
        b.image         = img;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, layers };
        VkDependencyInfo dep{};
        dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &b;
        vkCmdPipelineBarrier2(c, &dep);
    };

    // 1. Env: UNDEFINED → GENERAL, dispatch equirect HDRI → cube projection.
    barrier(m_envCubemap.Handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_envPipeline);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_envPipelineLayout,
                            0, 1, &m_envSet, 0, nullptr);
    vkCmdDispatch(c, (kEnvCubeSize + 7) / 8, (kEnvCubeSize + 7) / 8, 6);

    // 2. Env: build the mip chain with image blits (mip 0 → 1 → 2 → ...), then
    //    move every level to SHADER_READ_ONLY so irradiance + prefilter sample it.
    //    The mip-targeted barriers can't use the baseMip-0 `barrier` lambda above.
    auto mipBarrier = [&](uint32_t baseMip, uint32_t mipCount,
                          VkImageLayout oldL, VkImageLayout newL,
                          VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                          VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{};
        b.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask  = srcStage;  b.srcAccessMask = srcAccess;
        b.dstStageMask  = dstStage;  b.dstAccessMask = dstAccess;
        b.oldLayout     = oldL;      b.newLayout     = newL;
        b.image         = m_envCubemap.Handle();
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 6 };
        VkDependencyInfo dep{};
        dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &b;
        vkCmdPipelineBarrier2(c, &dep);
    };

    // Mip 0 (just written by the env compute): GENERAL → TRANSFER_SRC.
    mipBarrier(0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    // Mips 1..N-1: UNDEFINED → TRANSFER_DST (they were never written yet).
    mipBarrier(1, kEnvCubeMips - 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (uint32_t i = 1; i < kEnvCubeMips; ++i) {
        int32_t srcDim = int32_t(std::max(kEnvCubeSize >> (i - 1), 1u));
        int32_t dstDim = int32_t(std::max(kEnvCubeSize >> i,       1u));
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 6 };
        blit.srcOffsets[1]  = { srcDim, srcDim, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i,     0, 6 };
        blit.dstOffsets[1]  = { dstDim, dstDim, 1 };
        vkCmdBlitImage(c, m_envCubemap.Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          m_envCubemap.Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          1, &blit, VK_FILTER_LINEAR);
        // This level just got written — flip it to SRC so it feeds the next blit.
        mipBarrier(i, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    // Every level is now TRANSFER_SRC → SHADER_READ_ONLY for the compute samplers.
    mipBarrier(0, kEnvCubeMips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // 3. Irradiance: UNDEFINED → GENERAL, dispatch hemisphere integral.
    barrier(m_irradianceCubemap.Handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_irradiancePipeline);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_irradiancePipelineLayout,
                            0, 1, &m_irradianceSet, 0, nullptr);
    vkCmdDispatch(c, (kIrradianceSize + 7) / 8, (kIrradianceSize + 7) / 8, 6);

    // 4. Irradiance: GENERAL → SHADER_READ_ONLY so the lighting pass can sample it.
    barrier(m_irradianceCubemap.Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // 5. Prefiltered cubemap: all mips UNDEFINED → GENERAL, then dispatch per mip.
    barrier(m_prefilteredCubemap.Handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            kPrefilteredMips, 6);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_prefilterPipeline);
    for (uint32_t i = 0; i < kPrefilteredMips; ++i) {
        struct PrefilterPC { float roughness; uint32_t mipSize; uint32_t envSize; };
        PrefilterPC pc{};
        pc.roughness = (kPrefilteredMips > 1)
            ? float(i) / float(kPrefilteredMips - 1)
            : 0.0f;
        pc.mipSize   = kPrefilteredSize >> i;
        if (pc.mipSize == 0) pc.mipSize = 1;
        pc.envSize   = kEnvCubeSize;  // base face resolution, for the prefilter mip-select math

        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_prefilterPipelineLayout,
                                0, 1, &m_prefilterSets[i], 0, nullptr);
        vkCmdPushConstants(c, m_prefilterPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(PrefilterPC), &pc);
        uint32_t g = (pc.mipSize + 7) / 8;
        vkCmdDispatch(c, g, g, 6);
    }
    // 6. Prefiltered: GENERAL → SHADER_READ_ONLY (all mips).
    barrier(m_prefilteredCubemap.Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            kPrefilteredMips, 6);

    // 7. BRDF LUT: UNDEFINED → GENERAL → dispatch → SHADER_READ_ONLY.
    barrier(m_brdfLut.Handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            1, 1);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_brdfLutPipeline);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_brdfLutPipelineLayout,
                            0, 1, &m_brdfLutSet, 0, nullptr);
    vkCmdDispatch(c, (kBrdfLutSize + 7) / 8, (kBrdfLutSize + 7) / 8, 1);
    barrier(m_brdfLut.Handle(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            1, 1);

    // OneShotCmd destructor submits + waitIdle.
    return true;
}

bool VulkanContext::createTAAResources() {
    auto make = [&](ImageVk& out, VkImageUsageFlags extra) {
        ImageCreateParams p{};
        p.width  = m_swapchain.Extent().width;
        p.height = m_swapchain.Extent().height;
        p.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        p.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra;
        p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        return out.Create(m_device.Handle(), m_allocator.Handle(), p);
    };
    if (!make(m_sceneColor,     0))                                  return false;
    if (!make(m_taaHistory[0],  0))                                  return false;
    if (!make(m_taaHistory[1],  0))                                  return false;

    if (m_taaSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo info{};
        info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter    = VK_FILTER_LINEAR;
        info.minFilter    = VK_FILTER_LINEAR;
        info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_device.Handle(), &info, nullptr, &m_taaSampler) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] TAA sampler create failed\n");
            return false;
        }
    }
    return true;
}

void VulkanContext::destroyTAAResources() {
    m_sceneColor.Destroy();
    m_taaHistory[0].Destroy();
    m_taaHistory[1].Destroy();
}

bool VulkanContext::createTAAPipeline() {
    // Set layout — 3 sampled images (sceneColor, history, motion).
    VkDescriptorSetLayoutBinding bs[3]{};
    for (int i = 0; i < 3; ++i) {
        bs[i].binding         = static_cast<uint32_t>(i);
        bs[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bs[i].descriptorCount = 1;
        bs[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings    = bs;
    if (vkCreateDescriptorSetLayout(m_device.Handle(), &li, nullptr, &m_taaSetLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] TAA DescriptorSetLayout failed\n");
        return false;
    }

    VkPipelineLayoutCreateInfo pl{};
    pl.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts    = &m_taaSetLayout;
    if (vkCreatePipelineLayout(m_device.Handle(), &pl, nullptr, &m_taaPipelineLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] TAA pipelineLayout failed\n");
        return false;
    }

    VkShaderModule vs = loadShaderModule("Shaders/SPV/fullscreen.vert.spv");
    VkShaderModule fs = loadShaderModule("Shaders/SPV/taa.frag.spv");
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(m_device.Handle(), vs, nullptr);
        if (fs) vkDestroyShaderModule(m_device.Handle(), fs, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

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

    // MRT: location 0 → swapchain, location 1 → history(next).
    VkPipelineColorBlendAttachmentState blends[2];
    for (int i = 0; i < 2; ++i) blends[i] = makeOpaqueBlend();

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 2;
    cb.pAttachments    = blends;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dyn;

    // Both TAA outputs are HDR linear: location 0 = m_postScratch (input to
    // tonemap), location 1 = m_taaHistory ping-pong slot. The sRGB encode
    // happens later in the tonemap pass writing to the swapchain.
    const VkFormat formats[2] = {
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R16G16B16A16_SFLOAT,
    };
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount    = 2;
    renderingInfo.pColorAttachmentFormats = formats;

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &renderingInfo;
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
    info.layout              = m_taaPipelineLayout;

    VkResult r = vkCreateGraphicsPipelines(m_device.Handle(), m_pipelineCache, 1,
                                           &info, nullptr, &m_taaPipeline);
    vkDestroyShaderModule(m_device.Handle(), vs, nullptr);
    vkDestroyShaderModule(m_device.Handle(), fs, nullptr);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] TAA pipeline create failed: %d\n", r);
        return false;
    }
    return true;
}

bool VulkanContext::createTAADescriptors() {
    VkDescriptorSetLayout layouts[2] = { m_taaSetLayout, m_taaSetLayout };
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = m_descriptorPool;
    alloc.descriptorSetCount = 2;
    alloc.pSetLayouts        = layouts;
    if (vkAllocateDescriptorSets(m_device.Handle(), &alloc, m_taaSets) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] alloc TAA descriptor sets failed\n");
        return false;
    }
    writeTAADescriptors();
    return true;
}

void VulkanContext::writeTAADescriptors() {
    // taaSets[i] = "this frame's input direction" — reads sceneColor + history[i]
    // + motion. The frag writes location 1 to history[1-i] externally.
    for (int i = 0; i < 2; ++i) {
        VkDescriptorImageInfo infos[3]{};
        infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[0].imageView   = m_sceneColor.View();
        infos[0].sampler     = m_taaSampler;
        infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[1].imageView   = m_taaHistory[i].View();
        infos[1].sampler     = m_taaSampler;
        infos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[2].imageView   = m_gMotion.View();
        infos[2].sampler     = m_taaSampler;

        VkWriteDescriptorSet writes[3]{};
        for (int b = 0; b < 3; ++b) {
            writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet          = m_taaSets[i];
            writes[b].dstBinding      = static_cast<uint32_t>(b);
            writes[b].descriptorCount = 1;
            writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].pImageInfo      = &infos[b];
        }
        vkUpdateDescriptorSets(m_device.Handle(), 3, writes, 0, nullptr);
    }
}

bool VulkanContext::createGBufferDescriptors() {
    // 5 G-Buffer + shadow (5) + irradiance (6) + prefiltered (7) + brdfLut (8)
    // + SVGF denoised SSR (9). The binding 9 alternates between m_ssrColor[0]
    // and m_ssrColor[1] each frame (SVGF ping-pong) — UPDATE_AFTER_BIND lets
    // us patch only that slot per-frame without re-allocating the whole set.
    constexpr uint32_t kSet2Samplers = 10;          // combined-image-sampler bindings 0..9
    constexpr uint32_t kSet2Total    = kSet2Samplers + 2;  // + binding 10 SSBO + binding 11 raw shadow depth
    VkDescriptorSetLayoutBinding bindings[kSet2Total]{};
    for (uint32_t i = 0; i < kSet2Samplers; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    // binding 10 — punctual (point/spot) light SSBO.
    bindings[kSet2Samplers].binding         = kSet2Samplers;
    bindings[kSet2Samplers].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[kSet2Samplers].descriptorCount = 1;
    bindings[kSet2Samplers].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    // binding 11 — raw CSM depth (non-comparison) for the PCSS blocker search.
    bindings[kSet2Samplers + 1].binding         = kSet2Samplers + 1;
    bindings[kSet2Samplers + 1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[kSet2Samplers + 1].descriptorCount = 1;
    bindings[kSet2Samplers + 1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Per-binding flags: only binding 9 is UPDATE_AFTER_BIND. The rest are
    // static for the whole app lifetime.
    VkDescriptorBindingFlags bindFlags[kSet2Total] = {};
    bindFlags[9] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo bindFlagsInfo{};
    bindFlagsInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    bindFlagsInfo.bindingCount  = kSet2Total;
    bindFlagsInfo.pBindingFlags = bindFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext        = &bindFlagsInfo;
    layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = kSet2Total;
    layoutInfo.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(m_device.Handle(), &layoutInfo, nullptr,
                                    &m_gbufferSetLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] G-Buffer DescriptorSetLayout failed\n");
        return false;
    }

    // One set per frame-in-flight: binding 9 is rewritten each frame, so a
    // single shared set would race the previous frame still reading it.
    VkDescriptorSetLayout layouts[kFramesInFlight];
    std::fill(std::begin(layouts), std::end(layouts), m_gbufferSetLayout);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = m_descriptorPool;
    alloc.descriptorSetCount = kFramesInFlight;
    alloc.pSetLayouts        = layouts;
    if (vkAllocateDescriptorSets(m_device.Handle(), &alloc, m_gbufferSet.data()) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] alloc G-Buffer descriptor set failed\n");
        return false;
    }

    VkDescriptorImageInfo infos[kSet2Samplers]{};
    const VkImageView views[kSet2Samplers] = {
        m_gPosition.View(), m_gNormal.View(),
        m_gAlbedo.View(),   m_gEmissive.View(),
        m_gMotion.View(),
        m_shadowMap.View(),
        m_irradianceCubemap.View(),
        m_prefilteredCubemap.View(),
        m_brdfLut.View(),
        m_ssrColor[0].View(),  // SVGF denoised, slot 0 — patched per-frame to curSlot
    };
    const VkSampler samplers[kSet2Samplers] = {
        m_gbufferSampler, m_gbufferSampler, m_gbufferSampler,
        m_gbufferSampler, m_gbufferSampler,
        m_shadowSampler,
        m_iblSampler, m_iblSampler, m_iblSampler,
        m_gbufferSampler,
    };
    // Write all 12 bindings (0..11) into every per-frame set. The infos/views/
    // samplers are identical for all frames — only the dstSet differs. Reusing
    // the local arrays across iterations is safe because each vkUpdateDescriptorSets
    // executes synchronously.
    for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
        VkWriteDescriptorSet writes[kSet2Samplers]{};
        for (uint32_t i = 0; i < kSet2Samplers; ++i) {
            infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            infos[i].imageView   = views[i];
            infos[i].sampler     = samplers[i];
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = m_gbufferSet[frame];
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo      = &infos[i];
        }
        vkUpdateDescriptorSets(m_device.Handle(), kSet2Samplers, writes, 0, nullptr);

        // binding 10 — punctual light SSBO.
        VkDescriptorBufferInfo lightBufInfo{};
        lightBufInfo.buffer = m_lightBuffer[frame].Handle();
        lightBufInfo.range  = VK_WHOLE_SIZE;
        VkWriteDescriptorSet lightWrite{};
        lightWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        lightWrite.dstSet          = m_gbufferSet[frame];
        lightWrite.dstBinding      = kSet2Samplers;  // 10
        lightWrite.descriptorCount = 1;
        lightWrite.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        lightWrite.pBufferInfo     = &lightBufInfo;
        vkUpdateDescriptorSets(m_device.Handle(), 1, &lightWrite, 0, nullptr);

        // binding 11 — raw CSM depth for PCSS. Same image view as binding 5, but read
        // through the non-comparison nearest sampler so the shader gets actual depths.
        VkDescriptorImageInfo shadowDepthInfo{};
        shadowDepthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        shadowDepthInfo.imageView   = m_shadowMap.View();
        shadowDepthInfo.sampler     = m_shadowDepthSampler;
        VkWriteDescriptorSet shadowDepthWrite{};
        shadowDepthWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        shadowDepthWrite.dstSet          = m_gbufferSet[frame];
        shadowDepthWrite.dstBinding      = kSet2Samplers + 1;  // 11
        shadowDepthWrite.descriptorCount = 1;
        shadowDepthWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowDepthWrite.pImageInfo      = &shadowDepthInfo;
        vkUpdateDescriptorSets(m_device.Handle(), 1, &shadowDepthWrite, 0, nullptr);
    }
    return true;
}

bool VulkanContext::createSampler() {
    VkSamplerCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter    = VK_FILTER_LINEAR;
    info.minFilter    = VK_FILTER_LINEAR;
    info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.minLod       = 0.0f;
    info.maxLod       = 0.0f;
    if (vkCreateSampler(m_device.Handle(), &info, nullptr, &m_sampler) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] vkCreateSampler failed\n");
        return false;
    }
    return true;
}

namespace {

// Uploads a CPU pixel buffer to a new ImageVk in SHADER_READ_ONLY_OPTIMAL layout.
bool uploadImageRGBA(VulkanDevice& device, VulkanAllocator& allocator,
                     const uint8_t* pixels, int w, int h, VkFormat format,
                     ImageVk& out) {
    VkDeviceSize byteSize = static_cast<VkDeviceSize>(w) * h * 4;
    BufferVk staging;
    if (!staging.CreateStaging(allocator.Handle(), byteSize)) return false;
    std::memcpy(staging.Mapped(), pixels, byteSize);

    ImageCreateParams p{};
    p.width  = static_cast<uint32_t>(w);
    p.height = static_cast<uint32_t>(h);
    p.format = format;
    p.usage  = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (!out.Create(device.Handle(), allocator.Handle(), p)) return false;

    OneShotCmd one(device.Handle(), device.GraphicsQueueFamily(), device.GraphicsQueue());
    out.CmdTransition(one.cmd(),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    out.CmdCopyFromBuffer(one.cmd(), staging.Handle());
    out.CmdTransition(one.cmd(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    return true;
}

}  // namespace

bool VulkanContext::createFallbackTexture() {
    // Slot 0: 1×1 white. Meshes without baseColorTexture sample this and the
    // shader multiplies by their material's baseColorFactor — so glTF materials
    // that rely on baseColorFactor alone render with the right color.
    const uint8_t whitePixel[4] = { 255, 255, 255, 255 };
    ImageVk img;
    if (!uploadImageRGBA(m_device, m_allocator, whitePixel, 1, 1,
                         VK_FORMAT_R8G8B8A8_SRGB, img)) {
        return false;
    }
    m_textures.push_back(std::move(img));
    m_texturePathToIndex[""] = 0;
    return true;
}

uint32_t VulkanContext::loadOrGetTexture(const std::string& path, VkFormat format) {
    if (path.empty()) return 0;
    auto it = m_texturePathToIndex.find(path);
    if (it != m_texturePathToIndex.end()) return it->second;

    int w = 0, h = 0, ch = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!pixels) {
        std::fprintf(stderr, "[Vulkan] stbi_load failed for material texture '%s' — using fallback\n",
                     path.c_str());
        m_texturePathToIndex[path] = 0;
        return 0;
    }
    ImageVk img;
    bool ok = uploadImageRGBA(m_device, m_allocator, pixels, w, h, format, img);
    stbi_image_free(pixels);
    if (!ok) {
        m_texturePathToIndex[path] = 0;
        return 0;
    }
    uint32_t idx = static_cast<uint32_t>(m_textures.size());
    m_textures.push_back(std::move(img));
    m_texturePathToIndex[path] = idx;
    std::printf("[Vulkan] loaded texture[%u] %s (%dx%d, fmt=%d)\n", idx, path.c_str(), w, h, format);

    // If the bindless set already exists (i.e. we're loading textures past
    // Initialize() — e.g. RegisterSpriteTexture at runtime), patch the new
    // slot in. UPDATE_AFTER_BIND on the bindless binding makes this legal
    // even while the previous frame is still in flight.
    if (m_bindlessSet != VK_NULL_HANDLE) {
        VkDescriptorImageInfo info{};
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        info.imageView   = m_textures.back().View();
        info.sampler     = m_sampler;
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = m_bindlessSet;
        w.dstBinding      = 0;
        w.dstArrayElement = idx;
        w.descriptorCount = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo      = &info;
        vkUpdateDescriptorSets(m_device.Handle(), 1, &w, 0, nullptr);
    }
    return idx;
}

uint32_t VulkanContext::RegisterMesh(const MeshCPU& cpu, const glm::vec4& baseColor,
                                     float metallic, float roughness) {
    MeshVk mesh;
    if (!mesh.Upload(m_allocator.Handle(), m_device.Handle(),
                     m_device.GraphicsQueueFamily(), m_device.GraphicsQueue(),
                     cpu.vertices, cpu.indices)) {
        std::fprintf(stderr, "[Vulkan] RegisterMesh upload failed\n");
        return 0;  // collapse to mesh 0 — caller can detect via MeshCount() pre/post
    }
    mesh.SetLocalTransform(cpu.localTransform);
    mesh.SetMaterialIndex(0);            // fallback white texture (slot 0)
    mesh.SetMetallicRoughnessIndex(0);   // fallback white MR → factors used as-is
    mesh.SetBaseColorFactor(baseColor);
    // Override the glTF-style 1.0/1.0 defaults so a plain proc mesh is matte and
    // doesn't behave like a chrome mirror (the cause of the grazing-angle streaks
    // on the ground: a metallic floor reflecting the env cubemap horizon).
    mesh.SetMetallicFactor(metallic);
    mesh.SetRoughnessFactor(roughness);

    uint32_t idx = static_cast<uint32_t>(m_meshes.size());
    m_meshes.push_back(std::move(mesh));
    return idx;
}

bool VulkanContext::loadModel(const char* path) {
    LoadedModel loaded = LoadModelCPU(path);
    if (loaded.meshes.empty()) {
        std::fprintf(stderr, "[Vulkan] loadModel('%s') returned no meshes\n", path);
        return false;
    }

    // Resolve every glTF material → bindless texture slots exactly once. The
    // baseColor texture is loaded as SRGB (perceptual color), the
    // metallicRoughness texture as UNORM (raw numeric data — gamma would
    // corrupt the metallic/roughness values).
    std::vector<uint32_t> materialToBaseColor(loaded.materials.size(), 0);
    std::vector<uint32_t> materialToMR(loaded.materials.size(), 0);
    for (size_t i = 0; i < loaded.materials.size(); ++i) {
        materialToBaseColor[i] = loadOrGetTexture(loaded.materials[i].diffusePath,
                                                  VK_FORMAT_R8G8B8A8_SRGB);
        materialToMR[i]        = loadOrGetTexture(loaded.materials[i].metallicRoughnessPath,
                                                  VK_FORMAT_R8G8B8A8_UNORM);
        std::printf("[Vulkan] material[%zu] -> baseColor slot %u, mr slot %u\n",
                    i, materialToBaseColor[i], materialToMR[i]);
    }

    m_meshes.reserve(loaded.meshes.size());
    for (size_t i = 0; i < loaded.meshes.size(); ++i) {
        auto& cpu = loaded.meshes[i];
        MeshVk mesh;
        if (!mesh.Upload(m_allocator.Handle(), m_device.Handle(),
                         m_device.GraphicsQueueFamily(), m_device.GraphicsQueue(),
                         cpu.vertices, cpu.indices)) {
            std::fprintf(stderr, "[Vulkan] MeshVk::Upload failed\n");
            return false;
        }
        mesh.SetLocalTransform(cpu.localTransform);

        uint32_t baseTex = 0;
        uint32_t mrTex   = 0;
        glm::vec4 baseColor(1.0f);
        float metallic  = 1.0f;
        float roughness = 1.0f;
        if (cpu.sceneMaterialIndex < loaded.materials.size()) {
            const auto& mat = loaded.materials[cpu.sceneMaterialIndex];
            baseTex   = materialToBaseColor[cpu.sceneMaterialIndex];
            mrTex     = materialToMR[cpu.sceneMaterialIndex];
            baseColor = mat.baseColorFactor;
            metallic  = mat.metallicFactor;
            roughness = mat.roughnessFactor;
        }
        mesh.SetMaterialIndex(baseTex);
        mesh.SetBaseColorFactor(baseColor);
        mesh.SetMetallicRoughnessIndex(mrTex);
        mesh.SetMetallicFactor(metallic);
        mesh.SetRoughnessFactor(roughness);
        std::printf("[Vulkan] mesh[%zu] sceneMat=%u -> base=%u mr=%u baseColor=(%.2f,%.2f,%.2f,%.2f) m=%.2f r=%.2f\n",
                    i, cpu.sceneMaterialIndex, baseTex, mrTex,
                    baseColor.r, baseColor.g, baseColor.b, baseColor.a,
                    metallic, roughness);

        m_meshes.push_back(std::move(mesh));
    }
    return true;
}

bool VulkanContext::createDescriptors() {
    // ---- set=0 layout (per-frame UBO) -----------------------------------
    VkDescriptorSetLayoutBinding frameBinding{};
    frameBinding.binding         = 0;
    frameBinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    frameBinding.descriptorCount = 1;
    frameBinding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                                 | VK_SHADER_STAGE_COMPUTE_BIT;  // vol_fog god rays reads cascades

    VkDescriptorSetLayoutCreateInfo frameLayoutInfo{};
    frameLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    frameLayoutInfo.bindingCount = 1;
    frameLayoutInfo.pBindings    = &frameBinding;
    if (vkCreateDescriptorSetLayout(m_device.Handle(), &frameLayoutInfo, nullptr,
                                    &m_frameSetLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] frame DescriptorSetLayout failed\n");
        return false;
    }

    // ---- set=1 layout (bindless sampler2D[kMaxBindlessTextures]) --------
    VkDescriptorSetLayoutBinding bindlessBinding{};
    bindlessBinding.binding         = 0;
    bindlessBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindlessBinding.descriptorCount = kMaxBindlessTextures;
    bindlessBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorBindingFlags bindlessFlags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo bindlessFlagsInfo{};
    bindlessFlagsInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    bindlessFlagsInfo.bindingCount  = 1;
    bindlessFlagsInfo.pBindingFlags = &bindlessFlags;

    VkDescriptorSetLayoutCreateInfo bindlessLayoutInfo{};
    bindlessLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    bindlessLayoutInfo.pNext        = &bindlessFlagsInfo;
    bindlessLayoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    bindlessLayoutInfo.bindingCount = 1;
    bindlessLayoutInfo.pBindings    = &bindlessBinding;
    if (vkCreateDescriptorSetLayout(m_device.Handle(), &bindlessLayoutInfo, nullptr,
                                    &m_bindlessSetLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] bindless DescriptorSetLayout failed\n");
        return false;
    }

    // ---- Pool ------------------------------------------------------------
    // Mip count for the Hi-Z chain isn't known until the swapchain extent is
    // resolved; we conservatively budget for 16 mips (matches up to 32k×32k
    // screens — well above the realistic max).
    constexpr uint32_t kMaxHiZMips = 16;

    VkDescriptorPoolSize poolSizes[4]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = kFramesInFlight;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // bindless + gbuffer (10) + hiZ seed depth + (kMaxHiZMips - 1) downsample src
    //         + env→irradiance + env→prefilter (kPrefilteredMips) + TAA (2×3)
    //         + SSR sort (2) + SSR trace (2 sets × 5 samplers)
    //         + SVGF temporal (2 sets × 6 samplers) + SVGF spatial (4 sets × 4 samplers).
    constexpr uint32_t kMaxBloomMips = 8;
    poolSizes[1].descriptorCount = kMaxBindlessTextures + 10 + 1 + (kMaxHiZMips - 1)
                                 + 1 + kPrefilteredMips + 6
                                 + 2 + 10
                                 + 12 + 16
                                 + 3  // tonemap (post + bloom + fog)
                                 + 1  // bloom prefilter sampler
                                 + (kMaxBloomMips - 1)  // bloom downsample sampler per iter
                                 + (kMaxBloomMips - 1) * 2  // bloom upsample 2 samplers per iter
                                 + 2   // vol fog: depth + CSM shadow (god rays)
                                 + 1   // vol fog temporal history
                                 + 3   // SSR upsample: gPos + gNor + ssrHalf
                                 + 1   // auto-exposure histogram build: postScratch
                                 + 1   // CAS input sampler (tonemap LDR)
                                 + 1   // gbuffer set: raw CSM depth for PCSS (binding 11)
                                 + 1   // IBL env equirect sampler (env set binding 0)
                                 + (kFramesInFlight - 1) * 11;  // extra gbuffer sets (per-frame): 10 samplers + binding 11
    poolSizes[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    // hiZ seed (mip 0) + downsample dst (kMaxHiZMips - 1) + env + irradiance
    //   + prefiltered (per-mip) + brdf LUT + SSR raw (×2 trace sets)
    //   + SVGF temporal (2 sets × 2 outputs) + SVGF spatial (4 sets × 1 output).
    poolSizes[2].descriptorCount = 1 + (kMaxHiZMips - 1) + 2 + kPrefilteredMips + 1 + 2
                                 + 4 + 4
                                 + 1                    // bloom prefilter dst
                                 + (kMaxBloomMips - 1)  // bloom downsample dst
                                 + (kMaxBloomMips - 1)  // bloom upsample dst
                                 + 1                    // SSR upsample dst
                                 + 1;                   // vol fog storage
    poolSizes[3].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    // SSR sort set: 5 SSBOs; prefix set: 4 SSBOs; trace sets (×2): 2 each;
    // particle set: 1 SSBO; sprite set: 1 SSBO; punctual lights: 1 SSBO.
    poolSizes[3].descriptorCount = 5 + 4 + 4 + 1 + 1 + 1
                                 + 4   // auto-exposure: build(1 hist) + average(2: hist+exposure) + tonemap(1 exposure)
                                 + (kFramesInFlight - 1) * 1;  // extra gbuffer sets: light SSBO binding 10

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    // frame + bindless + G-Buffer + Hi-Z seed + Hi-Z downsample (×N) + env
    // + irradiance + prefilter mips + brdf LUT + TAA (×2) + SSR sort
    // + SSR prefix + SSR trace (×2) + SVGF temporal (×2) + SVGF spatial (×4).
    poolInfo.maxSets       = kFramesInFlight + 4 + kMaxHiZMips + kPrefilteredMips
                           + 1 + 2 + 4 + 6
                           + 1                            // tonemap set
                           + 1                            // CAS set
                           + 1                            // bloom prefilter set
                           + (kMaxBloomMips - 1) * 2      // bloom down + up sets
                           + 1                            // vol fog set
                           + 1                            // SSR upsample set
                           + 2                            // auto-exposure build + average sets
                           + 1                            // particle ssbo set
                           + 1                            // sprite ssbo set
                           + (kFramesInFlight - 1);       // extra gbuffer sets (per-frame)
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes    = poolSizes;
    if (vkCreateDescriptorPool(m_device.Handle(), &poolInfo, nullptr,
                               &m_descriptorPool) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] vkCreateDescriptorPool failed\n");
        return false;
    }

    // ---- Per-frame UBOs + set=0 -----------------------------------------
    for (auto& f : m_frames) {
        if (!f.ubo.CreateHostCoherent(m_allocator.Handle(), sizeof(CameraUBO),
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
            return false;
        }
    }

    {
        std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{};
        layouts.fill(m_frameSetLayout);

        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool     = m_descriptorPool;
        alloc.descriptorSetCount = kFramesInFlight;
        alloc.pSetLayouts        = layouts.data();

        VkDescriptorSet sets[kFramesInFlight]{};
        if (vkAllocateDescriptorSets(m_device.Handle(), &alloc, sets) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] alloc frame descriptor sets failed\n");
            return false;
        }
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            m_frames[i].frameDescriptorSet = sets[i];

            VkDescriptorBufferInfo bi{};
            bi.buffer = m_frames[i].ubo.Handle();
            bi.range  = sizeof(CameraUBO);

            VkWriteDescriptorSet w{};
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = sets[i];
            w.dstBinding      = 0;
            w.descriptorCount = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.pBufferInfo     = &bi;
            vkUpdateDescriptorSets(m_device.Handle(), 1, &w, 0, nullptr);
        }
    }

    // ---- Bindless set=1 -------------------------------------------------
    {
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool     = m_descriptorPool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts        = &m_bindlessSetLayout;
        if (vkAllocateDescriptorSets(m_device.Handle(), &alloc, &m_bindlessSet) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] alloc bindless descriptor set failed\n");
            return false;
        }

        const uint32_t numTex = static_cast<uint32_t>(m_textures.size());
        std::vector<VkDescriptorImageInfo> imgInfos(numTex);
        std::vector<VkWriteDescriptorSet> writes(numTex);
        for (uint32_t i = 0; i < numTex; ++i) {
            imgInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imgInfos[i].imageView   = m_textures[i].View();
            imgInfos[i].sampler     = m_sampler;

            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = m_bindlessSet;
            writes[i].dstBinding      = 0;
            writes[i].dstArrayElement = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo      = &imgInfos[i];
        }
        if (numTex > 0) {
            vkUpdateDescriptorSets(m_device.Handle(), numTex, writes.data(), 0, nullptr);
        }
    }
    return true;
}

void VulkanContext::loadPipelineCache() {
    auto data = readBinaryFile(m_pipelineCachePath.c_str());
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = data.size();
    info.pInitialData    = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(m_device.Handle(), &info, nullptr, &m_pipelineCache) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] vkCreatePipelineCache failed (continuing without)\n");
        m_pipelineCache = VK_NULL_HANDLE;
    }
}

void VulkanContext::savePipelineCache() {
    if (!m_pipelineCache) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(m_device.Handle(), m_pipelineCache, &size, nullptr) != VK_SUCCESS) return;
    if (size == 0) return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(m_device.Handle(), m_pipelineCache, &size, data.data()) != VK_SUCCESS) return;
    std::ofstream out(m_pipelineCachePath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return;
    out.write(data.data(), static_cast<std::streamsize>(size));
}

VkShaderModule VulkanContext::loadShaderModule(const char* spvPath) {
    auto bytes = readBinaryFile(spvPath);
    if (bytes.empty()) {
        std::fprintf(stderr, "[Vulkan] Could not read SPV file: %s\n", spvPath);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes.size();
    info.pCode = reinterpret_cast<const uint32_t*>(bytes.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_device.Handle(), &info, nullptr, &mod) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] vkCreateShaderModule failed for %s\n", spvPath);
        return VK_NULL_HANDLE;
    }
    return mod;
}

bool VulkanContext::createGeometryPipeline() {
    // Pipeline layout — shared with the lighting pipeline. Set 0/1 are used by
    // geometry, set 2 by lighting; declaring all three in the same layout means
    // both pipelines can bind a single descriptor set range.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(PushConstants);

    VkDescriptorSetLayout setLayouts[3] = {
        m_frameSetLayout, m_bindlessSetLayout, m_gbufferSetLayout,
    };
    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 3;
    plInfo.pSetLayouts            = setLayouts;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(m_device.Handle(), &plInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] geometry pipelineLayout failed\n");
        return false;
    }

    VkShaderModule vs = loadShaderModule("Shaders/SPV/geometry.vert.spv");
    VkShaderModule fs = loadShaderModule("Shaders/SPV/geometry.frag.spv");
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(m_device.Handle(), vs, nullptr);
        if (fs) vkDestroyShaderModule(m_device.Handle(), fs, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding   = 0;
    bindingDesc.stride    = sizeof(MeshVertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, pos)    };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal) };
    attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(MeshVertex, uv)     };

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_BACK_BIT;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;

    // 5 attachments, opaque writes — matches G-Buffer layout in createGBuffer().
    VkPipelineColorBlendAttachmentState blends[5];
    for (int i = 0; i < 5; ++i) blends[i] = makeOpaqueBlend();

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 5;
    cb.pAttachments    = blends;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dyn;

    const VkFormat colorFormats[5] = {
        VK_FORMAT_R16G16B16A16_SFLOAT,  // gPosition
        VK_FORMAT_R16G16B16A16_SFLOAT,  // gNormal
        VK_FORMAT_R8G8B8A8_UNORM,       // gAlbedo
        VK_FORMAT_R16G16B16A16_SFLOAT,  // gEmissive
        VK_FORMAT_R16G16_SFLOAT,        // gMotion
    };
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount    = 5;
    renderingInfo.pColorAttachmentFormats = colorFormats;
    renderingInfo.depthAttachmentFormat   = kDepthFormat;

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &renderingInfo;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vertexInput;
    info.pInputAssemblyState = &ia;
    info.pViewportState      = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState   = &ms;
    info.pDepthStencilState  = &ds;
    info.pColorBlendState    = &cb;
    info.pDynamicState       = &dynState;
    info.layout              = m_pipelineLayout;

    VkResult r = vkCreateGraphicsPipelines(m_device.Handle(), m_pipelineCache, 1,
                                           &info, nullptr, &m_geometryPipeline);

    vkDestroyShaderModule(m_device.Handle(), vs, nullptr);
    vkDestroyShaderModule(m_device.Handle(), fs, nullptr);

    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] geometry pipeline create failed: %d\n", r);
        return false;
    }
    return true;
}

bool VulkanContext::createLightingPipeline() {
    VkShaderModule vs = loadShaderModule("Shaders/SPV/fullscreen.vert.spv");
    VkShaderModule fs = loadShaderModule("Shaders/SPV/lighting.frag.spv");
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(m_device.Handle(), vs, nullptr);
        if (fs) vkDestroyShaderModule(m_device.Handle(), fs, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";

    // No vertex input — fullscreen.vert uses gl_VertexIndex.
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;  // fullscreen triangle — no culling
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend = makeOpaqueBlend();
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &blend;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dyn;

    // Lighting writes to m_sceneColor (RGBA16F) — TAA consumes it and resolves to swapchain.
    VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount    = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    // No depth attachment in the lighting pass.

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &renderingInfo;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vertexInput;
    info.pInputAssemblyState = &ia;
    info.pViewportState      = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState   = &ms;
    info.pDepthStencilState  = &ds;
    info.pColorBlendState    = &cb;
    info.pDynamicState       = &dynState;
    info.layout              = m_pipelineLayout;

    VkResult r = vkCreateGraphicsPipelines(m_device.Handle(), m_pipelineCache, 1,
                                           &info, nullptr, &m_lightingPipeline);

    vkDestroyShaderModule(m_device.Handle(), vs, nullptr);
    vkDestroyShaderModule(m_device.Handle(), fs, nullptr);

    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] lighting pipeline create failed: %d\n", r);
        return false;
    }
    return true;
}

bool VulkanContext::createPerFrameResources() {
    for (auto& f : m_frames) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_device.GraphicsQueueFamily();
        if (vkCreateCommandPool(m_device.Handle(), &poolInfo, nullptr, &f.pool) != VK_SUCCESS) return false;

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool        = f.pool;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m_device.Handle(), &allocInfo, &f.cmd) != VK_SUCCESS) return false;

        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(m_device.Handle(), &semInfo, nullptr, &f.imageAvailable) != VK_SUCCESS) return false;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(m_device.Handle(), &fenceInfo, nullptr, &f.inFlight) != VK_SUCCESS) return false;
    }
    return true;
}

bool VulkanContext::createPerImageSemaphores() {
    destroyPerImageSemaphores();
    m_renderFinishedPerImage.assign(m_swapchain.ImageCount(), VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (auto& s : m_renderFinishedPerImage) {
        if (vkCreateSemaphore(m_device.Handle(), &semInfo, nullptr, &s) != VK_SUCCESS) {
            std::fprintf(stderr, "[Vulkan] vkCreateSemaphore (per-image) failed\n");
            return false;
        }
    }
    return true;
}

void VulkanContext::destroyPerImageSemaphores() {
    for (auto& s : m_renderFinishedPerImage) {
        if (s) vkDestroySemaphore(m_device.Handle(), s, nullptr);
    }
    m_renderFinishedPerImage.clear();
}

bool VulkanContext::recreateSwapchain(SDL_Window* window) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w == 0 || h == 0) return false;  // minimized — keep current state intact
    vkDeviceWaitIdle(m_device.Handle());
    destroyPerImageSemaphores();
    destroyVolFogResources();
    destroyBloomResources();
    destroyPostResources();
    destroySSRResources();
    destroyTAAResources();
    destroyHiZResources();
    destroyGBuffer();
    destroyDepthResources();
    // The VkImage handles tracked across frames are about to become invalid.
    m_graph.ClearPersistedState();
    m_swapchain.Shutdown(m_device);
    if (!m_swapchain.Initialize(m_device, m_surface,
                                static_cast<uint32_t>(w),
                                static_cast<uint32_t>(h))) return false;
    if (!createDepthResources()) return false;
    if (!createGBuffer())        return false;
    if (!createHiZResources())   return false;
    if (!createSSRResources())   return false;
    if (!createTAAResources())   return false;
    if (!createPostResources())  return false;
    if (!createBloomResources()) return false;
    if (!createVolFogResources()) return false;
    writeTAADescriptors();
    writeBloomDescriptors();
    writeVolFogDescriptors();
    writeTonemapDescriptors();
    writeCASDescriptors();           // m_tonemapLdr recreated by createPostResources
    writeAutoExposureDescriptors();  // build set references recreated postScratch
    m_taaFrameCounter = 0;
    m_prevViewProj    = glm::mat4(1.0f);
    m_prevJitter      = glm::vec2(0.0f);

    // G-Buffer images changed → rewrite the 5 G-Buffer slots + SVGF slot 9.
    // Shadow/IBL views (bindings 5-8) are fixed-size and never recreated.
    if (m_gbufferSet[0] != VK_NULL_HANDLE) {
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame) {
            VkDescriptorImageInfo infos[6]{};
            const VkImageView views[6] = {
                m_gPosition.View(), m_gNormal.View(),
                m_gAlbedo.View(),   m_gEmissive.View(),
                m_gMotion.View(),
                m_ssrColor[0].View(),  // patched each frame to curSlot in recordCommandBuffer
            };
            const uint32_t bindings[6] = { 0, 1, 2, 3, 4, 9 };
            VkWriteDescriptorSet writes[6]{};
            for (int i = 0; i < 6; ++i) {
                infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                infos[i].imageView   = views[i];
                infos[i].sampler     = m_gbufferSampler;
                writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet          = m_gbufferSet[frame];
                writes[i].dstBinding      = bindings[i];
                writes[i].descriptorCount = 1;
                writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo      = &infos[i];
            }
            vkUpdateDescriptorSets(m_device.Handle(), 6, writes, 0, nullptr);
        }
    }

    // Hi-Z seed set — depth view + mip 0 storage. Both recreated above.
    if (m_hizSet != VK_NULL_HANDLE) {
        VkDescriptorImageInfo depthInfo{};
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depthInfo.imageView   = m_depth.View();
        depthInfo.sampler     = m_depthSampler;

        VkDescriptorImageInfo hizInfo{};
        hizInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        hizInfo.imageView   = m_hiZMipViews.empty() ? m_hiZ.View() : m_hiZMipViews[0];

        VkWriteDescriptorSet w[2]{};
        w[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet          = m_hizSet;
        w[0].dstBinding      = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo      = &depthInfo;

        w[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[1].dstSet          = m_hizSet;
        w[1].dstBinding      = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo      = &hizInfo;

        vkUpdateDescriptorSets(m_device.Handle(), 2, w, 0, nullptr);
    }

    // Hi-Z downsample sets: each reads mip i, writes mip i+1. The descriptor
    // sets themselves were allocated in createHiZPipeline (one-time) but the
    // image views are tied to the resolution — recreate the writes.
    if (!m_hizDownsampleSets.empty()) {
        const uint32_t downCount = static_cast<uint32_t>(m_hizDownsampleSets.size());
        // After the resolution change, the number of mips may have changed; if
        // so, we'd need to re-allocate sets. Realistically Hi-Z mip count only
        // changes by ±1 between common resolutions and our budget covers up to
        // 16 mips — so we silently truncate to whatever was originally allocated.
        const uint32_t writeCount = std::min(downCount,
            (m_hiZMipCount > 0u) ? (m_hiZMipCount - 1u) : 0u);
        std::vector<VkDescriptorImageInfo> srcInfos(writeCount);
        std::vector<VkDescriptorImageInfo> dstInfos(writeCount);
        std::vector<VkWriteDescriptorSet>  writes(writeCount * 2);
        for (uint32_t i = 0; i < writeCount; ++i) {
            srcInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            srcInfos[i].imageView   = m_hiZMipViews[i];
            srcInfos[i].sampler     = m_depthSampler;
            dstInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            dstInfos[i].imageView   = m_hiZMipViews[i + 1];
            writes[i * 2 + 0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i * 2 + 0].dstSet = m_hizDownsampleSets[i];
            writes[i * 2 + 0].dstBinding = 0;
            writes[i * 2 + 0].descriptorCount = 1;
            writes[i * 2 + 0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i * 2 + 0].pImageInfo = &srcInfos[i];
            writes[i * 2 + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i * 2 + 1].dstSet = m_hizDownsampleSets[i];
            writes[i * 2 + 1].dstBinding = 1;
            writes[i * 2 + 1].descriptorCount = 1;
            writes[i * 2 + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i * 2 + 1].pImageInfo = &dstInfos[i];
        }
        if (!writes.empty()) {
            vkUpdateDescriptorSets(m_device.Handle(),
                                   static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
    }

    // SSR sort + prefix sets sample G-Buffer + SSBOs that just got recreated.
    if (m_ssrSortSet != VK_NULL_HANDLE) {
        VkDescriptorImageInfo posInfo{}, norInfo{};
        posInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        posInfo.imageView   = m_gPosition.View();
        posInfo.sampler     = m_gbufferSampler;
        norInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        norInfo.imageView   = m_gNormal.View();
        norInfo.sampler     = m_gbufferSampler;

        const BufferVk* sortSsbos[5] = {
            &m_ssrRayCount, &m_ssrBinCounters, &m_ssrBinOffsets,
            &m_ssrScatterCounters, &m_ssrSortedRays,
        };
        VkDescriptorBufferInfo sortBufs[5]{};
        for (int i = 0; i < 5; ++i) {
            sortBufs[i].buffer = sortSsbos[i]->Handle();
            sortBufs[i].range  = VK_WHOLE_SIZE;
        }
        VkWriteDescriptorSet sw[7]{};
        sw[0].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; sw[0].dstSet=m_ssrSortSet;
        sw[0].dstBinding=0; sw[0].descriptorCount=1;
        sw[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[0].pImageInfo=&posInfo;
        sw[1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; sw[1].dstSet=m_ssrSortSet;
        sw[1].dstBinding=1; sw[1].descriptorCount=1;
        sw[1].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[1].pImageInfo=&norInfo;
        for (int i = 0; i < 5; ++i) {
            sw[2+i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; sw[2+i].dstSet=m_ssrSortSet;
            sw[2+i].dstBinding=static_cast<uint32_t>(2+i); sw[2+i].descriptorCount=1;
            sw[2+i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sw[2+i].pBufferInfo=&sortBufs[i];
        }
        vkUpdateDescriptorSets(m_device.Handle(), 7, sw, 0, nullptr);

        const BufferVk* prefixSsbos[4] = {
            &m_ssrBinCounters, &m_ssrBinOffsets, &m_ssrRayCount, &m_ssrIndirectArgs,
        };
        VkDescriptorBufferInfo prefixBufs[4]{};
        VkWriteDescriptorSet   pw[4]{};
        for (int i = 0; i < 4; ++i) {
            prefixBufs[i].buffer = prefixSsbos[i]->Handle();
            prefixBufs[i].range  = VK_WHOLE_SIZE;
            pw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            pw[i].dstSet = m_ssrPrefixSet;
            pw[i].dstBinding = static_cast<uint32_t>(i);
            pw[i].descriptorCount = 1;
            pw[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            pw[i].pBufferInfo = &prefixBufs[i];
        }
        vkUpdateDescriptorSets(m_device.Handle(), 4, pw, 0, nullptr);

        writeSSRTraceDescriptors();
        writeSSRUpsampleDescriptors();

        // SVGF descriptor sets reference m_ssrResult / ssrColor[*] / Moments[*]
        // / G-Buffer views — all recreated above. Re-emit the same write
        // pattern that createSSRDescriptors uses for SVGF sets.
        for (uint32_t curSlot = 0; curSlot < 2; ++curSlot) {
            const uint32_t prevSlot = 1u - curSlot;
            VkDescriptorImageInfo curInfo{}, histColInfo{}, histMomInfo{};
            VkDescriptorImageInfo nInfo{}, pInfo{}, mInfo{};
            VkDescriptorImageInfo outColInfo{}, outMomInfo{};
            curInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            curInfo.imageView = m_ssrResult.View(); curInfo.sampler = m_gbufferSampler;
            histColInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            histColInfo.imageView = m_ssrColor[prevSlot].View(); histColInfo.sampler = m_gbufferSampler;
            histMomInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            histMomInfo.imageView = m_ssrMoments[prevSlot].View(); histMomInfo.sampler = m_gbufferSampler;
            nInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            nInfo.imageView = m_gNormal.View();   nInfo.sampler = m_gbufferSampler;
            pInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            pInfo.imageView = m_gPosition.View(); pInfo.sampler = m_gbufferSampler;
            mInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            mInfo.imageView = m_gMotion.View();   mInfo.sampler = m_gbufferSampler;
            outColInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            outColInfo.imageView = m_ssrColorTemp.View();
            outMomInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            outMomInfo.imageView = m_ssrMoments[curSlot].View();

            VkWriteDescriptorSet w[8]{};
            const VkDescriptorImageInfo* images[6] = {
                &curInfo, &histColInfo, &histMomInfo, &nInfo, &pInfo, &mInfo,
            };
            for (int i = 0; i < 6; ++i) {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet = m_ssrTemporalSets[curSlot];
                w[i].dstBinding = static_cast<uint32_t>(i);
                w[i].descriptorCount = 1;
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[i].pImageInfo = images[i];
            }
            w[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[6].dstSet = m_ssrTemporalSets[curSlot];
            w[6].dstBinding = 6; w[6].descriptorCount = 1;
            w[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[6].pImageInfo = &outColInfo;
            w[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[7].dstSet = m_ssrTemporalSets[curSlot];
            w[7].dstBinding = 7; w[7].descriptorCount = 1;
            w[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[7].pImageInfo = &outMomInfo;
            vkUpdateDescriptorSets(m_device.Handle(), 8, w, 0, nullptr);

            for (uint32_t dir = 0; dir < 2; ++dir) {
                VkDescriptorImageInfo inInfo{}, momInfo{}, nrmInfo{}, posInfo{}, outInfo{};
                const VkImageView inView  = (dir == 0) ? m_ssrColorTemp.View() : m_ssrColor[curSlot].View();
                const VkImageView outView = (dir == 0) ? m_ssrColor[curSlot].View() : m_ssrColorTemp.View();
                inInfo.imageLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                inInfo.imageView    = inView; inInfo.sampler = m_gbufferSampler;
                momInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                momInfo.imageView   = m_ssrMoments[curSlot].View();
                momInfo.sampler     = m_gbufferSampler;
                nrmInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                nrmInfo.imageView   = m_gNormal.View();   nrmInfo.sampler = m_gbufferSampler;
                posInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                posInfo.imageView   = m_gPosition.View(); posInfo.sampler = m_gbufferSampler;
                outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                outInfo.imageView   = outView;

                VkWriteDescriptorSet sw[5]{};
                const VkDescriptorImageInfo* imgs[4] = { &inInfo, &momInfo, &nrmInfo, &posInfo };
                for (int i = 0; i < 4; ++i) {
                    sw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    sw[i].dstSet = m_ssrSpatialSets[curSlot][dir];
                    sw[i].dstBinding = static_cast<uint32_t>(i);
                    sw[i].descriptorCount = 1;
                    sw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    sw[i].pImageInfo = imgs[i];
                }
                sw[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                sw[4].dstSet = m_ssrSpatialSets[curSlot][dir];
                sw[4].dstBinding = 4; sw[4].descriptorCount = 1;
                sw[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                sw[4].pImageInfo = &outInfo;
                vkUpdateDescriptorSets(m_device.Handle(), 5, sw, 0, nullptr);
            }
        }
    }

    return createPerImageSemaphores();
}

void VulkanContext::updateUniformBuffer(uint32_t frameIndex,
                                        const glm::mat4& view, const glm::mat4& proj,
                                        const glm::vec3& cameraPos,
                                        const DirectionalLight& light) {
    CameraUBO ubo{};
    ubo.view = view;
    ubo.proj = proj;
    // Vulkan clip-space Y points down; flip the perspective to keep the
    // OpenGL-style +Y-up convention in our world/view matrices.
    ubo.proj[1][1] *= -1.0f;
    ubo.cameraPos      = glm::vec4(cameraPos, 1.0f);
    ubo.lightDirection = glm::vec4(glm::normalize(light.direction), 0.0f);
    ubo.lightColor     = glm::vec4(light.color, 1.0f);
    ubo.ambient        = glm::vec4(light.ambient, 1.0f);

    // Light-space matrices, one orthographic box per cascade.
    glm::vec3 lightDir = glm::normalize(light.direction);
    glm::vec3 up = (std::abs(lightDir.y) > 0.99f)
        ? glm::vec3(0.0f, 0.0f, 1.0f)
        : glm::vec3(0.0f, 1.0f, 0.0f);

    if (m_csmFrustumFitEnabled) {
        // Fit each cascade to the slice of the camera frustum it covers. A bounding
        // sphere gives an orientation-independent box size (so rotating the camera
        // only translates the cascade, never resizes it), and texel snapping pins
        // that translation to the shadow grid — together they stop edge crawl.
        //
        // Recover the camera near/far from the projection (glm::perspective, depth
        // 0..1) to map cascade world-distances onto frustum-corner fractions.
        const float camNear = proj[3][2] / proj[2][2];
        const float camFar  = proj[3][2] / (proj[2][2] + 1.0f);
        const glm::mat4 invVP = glm::inverse(ubo.proj * view);  // ubo.proj is Y-flipped

        // World-space corners of the full camera frustum. Index layout: x fastest,
        // then y, then z — so corner j (near, z=0) pairs with j+4 (far, z=1).
        glm::vec3 fc[8];
        int ci = 0;
        for (int z = 0; z <= 1; ++z)
            for (int y = 0; y <= 1; ++y)
                for (int x = 0; x <= 1; ++x) {
                    glm::vec4 p = invVP * glm::vec4(2.0f * float(x) - 1.0f,
                                                    2.0f * float(y) - 1.0f,
                                                    float(z), 1.0f);
                    fc[ci++] = glm::vec3(p) / p.w;
                }

        for (uint32_t i = 0; i < kCascadeCount; ++i) {
            const float nearD = (i == 0) ? camNear : kCascadeFarSplits[i - 1];
            const float farD  = kCascadeFarSplits[i];
            const float nearF = (nearD - camNear) / (camFar - camNear);
            const float farF  = (farD  - camNear) / (camFar - camNear);

            glm::vec3 center(0.0f);
            glm::vec3 cc[8];
            for (int j = 0; j < 4; ++j) {
                glm::vec3 ray = fc[j + 4] - fc[j];
                cc[j]     = fc[j] + ray * nearF;
                cc[j + 4] = fc[j] + ray * farF;
            }
            for (int j = 0; j < 8; ++j) center += cc[j];
            center /= 8.0f;

            float radius = 0.0f;
            for (int j = 0; j < 8; ++j) radius = std::max(radius, glm::length(cc[j] - center));
            radius = std::ceil(radius * 16.0f) / 16.0f;  // quantize to steady the size

            // Ortho looking at the sphere centre; pull the near plane back by the
            // radius so casters just behind the slice still cast into it.
            const float zExtend = radius;
            glm::vec3 lightPos = center - lightDir * (radius + zExtend);
            glm::mat4 lv = glm::lookAt(lightPos, center, up);
            glm::mat4 lp = glm::ortho(-radius, radius, -radius, radius,
                                      0.0f, 2.0f * radius + zExtend);
            lp[1][1] *= -1.0f;  // Vulkan clip-space Y-flip
            glm::mat4 shadowMat = lp * lv;

            // Texel snapping: round the projected world origin to the shadow texel
            // grid and fold the residual back as a clip-space translation.
            const float halfSize = float(kShadowMapSize) * 0.5f;
            glm::vec4 origin = shadowMat * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            origin *= halfSize;
            glm::vec2 rounded = glm::round(glm::vec2(origin));
            glm::vec2 offset  = (rounded - glm::vec2(origin)) / halfSize;
            glm::mat4 snap = glm::translate(glm::mat4(1.0f), glm::vec3(offset, 0.0f));

            ubo.lightSpaceMatrix[i] = snap * shadowMat;
            ubo.cascadeSplits[i]    = kCascadeFarSplits[i];
        }
    } else {
        // Legacy origin-centred ortho (pre-frustum-fit) — wastes resolution but
        // is orientation-stable without snapping.
        glm::vec3 target(0.0f);
        for (uint32_t i = 0; i < kCascadeCount; ++i) {
            const float size  = kCascadeFarSplits[i];
            const float depth = size * 4.0f;
            glm::vec3 lightPos = target - lightDir * depth * 0.5f;
            glm::mat4 lv = glm::lookAt(lightPos, target, up);
            glm::mat4 lp = glm::ortho(-size, size, -size, size, 0.1f, depth);
            lp[1][1] *= -1.0f;
            ubo.lightSpaceMatrix[i] = lp * lv;
            ubo.cascadeSplits[i]    = kCascadeFarSplits[i];
        }
    }
    // .w drives PCSS: light size in shadow-UV units, 0 = disabled (fixed PCF).
    ubo.cascadeSplits.w = m_pcssEnabled ? m_pcssLightSize : 0.0f;

    // TAA jitter: Halton(2,3) in [-1,1] mapped to one pixel of clip-space.
    const uint32_t haltonIdx = (m_taaFrameCounter % kHaltonSamples) + 1;  // 1-based
    glm::vec2 j(
        haltonAt(haltonIdx, 2) - 0.5f,
        haltonAt(haltonIdx, 3) - 0.5f);
    const auto extent = m_swapchain.Extent();
    j.x *= 2.0f / float(extent.width);
    j.y *= 2.0f / float(extent.height);
    ubo.jitter = glm::vec4(j, m_prevJitter);

    // prevViewProj already includes the Vulkan Y-flip — we apply the same
    // transformation to view * proj as we did to the current frame.
    ubo.prevViewProj = m_prevViewProj;

    std::memcpy(m_frames[frameIndex].ubo.Mapped(), &ubo, sizeof(ubo));

    // Stash current view-projection (with Y-flip) for next frame's reprojection.
    m_prevViewProj = ubo.proj * view;
    m_prevJitter   = j;
    ++m_taaFrameCounter;
}

void VulkanContext::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

    // ---- Build the per-frame render graph -------------------------------
    m_graph.Reset();

    // The SSR half-res toggle changes which image the packet-trace writes to:
    // binding 7 of the trace set points at m_ssrResult (full-res) or
    // m_ssrResultHalf (half-res). That descriptor is only written at init/resize
    // (writeSSRTraceDescriptors), so flipping the toggle at runtime leaves it
    // stale — the shader imageStore's the OLD image (which the graph left in
    // SHADER_READ_ONLY) while the graph transitions the NEW one → layout mismatch.
    // On the switch: wait idle (the trace sets aren't UPDATE_AFTER_BIND so we
    // can't rewrite them while in flight), rewrite the trace descriptors to the
    // correct target, and forget the two SSR images so the graph re-imports them
    // as UNDEFINED (their content is regenerated every frame anyway).
    if (m_ssrHalfResEnabled != m_ssrHalfResPrev) {
        vkDeviceWaitIdle(m_device.Handle());
        writeSSRTraceDescriptors();
        m_graph.ForgetImage(m_ssrResult.Handle());
        m_graph.ForgetImage(m_ssrResultHalf.Handle());
        m_ssrHalfResPrev = m_ssrHalfResEnabled;
    }

    // Swapchain image: the imageAvailable semaphore is waited on at
    // COLOR_ATTACHMENT_OUTPUT, so the very first barrier touching this
    // image must declare that as srcStage — otherwise the layout
    // transition isn't ordered after the acquire (sync2 treats
    // TOP_OF_PIPE as NONE in srcStageMask). Forget last frame's persisted
    // state (PRESENT_SRC + BOTTOM_OF_PIPE) so ImportImage uses the
    // initial values we pass here, and oldLayout=UNDEFINED discards the
    // post-present contents (we overwrite the whole image anyway).
    m_graph.ForgetImage(m_swapchain.Image(imageIndex));
    GraphResource swapImg = m_graph.ImportImage(
        "swapchain", m_swapchain.Image(imageIndex),
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);
    GraphResource depthImg = m_graph.ImportImage(
        "depth", m_depth.Handle(),
        VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource gPos   = m_graph.ImportImage("gPosition", m_gPosition.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource gNor   = m_graph.ImportImage("gNormal",   m_gNormal.Handle(),   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource gAlb   = m_graph.ImportImage("gAlbedo",   m_gAlbedo.Handle(),   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource gEmi   = m_graph.ImportImage("gEmissive", m_gEmissive.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource gMot   = m_graph.ImportImage("gMotion",   m_gMotion.Handle(),   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource hiZ    = m_graph.ImportImage("hiZ",       m_hiZ.Handle(),       VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource shadow = m_graph.ImportImage("shadow",    m_shadowMap.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED);

    // TAA (FASE 11). taaInIdx = which history holds last frame's resolve and is sampled by
    // the shader; taaOutIdx is written through MRT location 1 and becomes next frame's input.
    const uint32_t taaInIdx  = m_taaFrameCounter % 2;
    const uint32_t taaOutIdx = (m_taaFrameCounter + 1u) % 2;
    GraphResource sceneCol  = m_graph.ImportImage("sceneColor", m_sceneColor.Handle(),       VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource histIn    = m_graph.ImportImage("taaHistoryIn",  m_taaHistory[taaInIdx].Handle(),  VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource histOut   = m_graph.ImportImage("taaHistoryOut", m_taaHistory[taaOutIdx].Handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);

    const uint32_t frameIndex = m_frameIndex;
    const VkExtent2D extent = m_swapchain.Extent();

    // ---- Shadow pass: 3 cascades into m_shadowMap layers ---------------
    // Reads the per-cascade lightSpaceMatrix off the frame UBO (already updated
    // for this frame). Each cascade renders the draw queue into its own layer.
    GraphPass shadowPass{};
    shadowPass.name = "Shadow";
    shadowPass.writes = {
        { shadow, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
              | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
              | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT },
    };
    shadowPass.execute = [this](VkCommandBuffer c) {
        struct ShadowPC {
            glm::mat4 lightSpace;
            glm::mat4 model;
        };

        // Pull the just-written light-space matrices straight from the UBO so
        // we don't have to recompute them in two places.
        const CameraUBO* ubo = reinterpret_cast<const CameraUBO*>(
            m_frames[m_frameIndex].ubo.Mapped());

        VkClearValue clearDepth{};
        clearDepth.depthStencil = { 1.0f, 0 };

        VkViewport vp{};
        vp.width    = static_cast<float>(kShadowMapSize);
        vp.height   = static_cast<float>(kShadowMapSize);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        VkRect2D scissor{ {0, 0}, { kShadowMapSize, kShadowMapSize } };

        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
        vkCmdSetViewport(c, 0, 1, &vp);
        vkCmdSetScissor(c, 0, 1, &scissor);

        for (uint32_t i = 0; i < kCascadeCount; ++i) {
            VkRenderingAttachmentInfo depthAtt{};
            depthAtt.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAtt.imageView   = m_shadowLayerViews[i];
            depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAtt.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
            depthAtt.clearValue  = clearDepth;

            VkRenderingInfo render{};
            render.sType           = VK_STRUCTURE_TYPE_RENDERING_INFO;
            render.renderArea      = scissor;
            render.layerCount      = 1;
            render.pDepthAttachment = &depthAtt;

            vkCmdBeginRendering(c, &render);

            for (const auto& item : m_drawQueue) {
                const MeshVk& mesh = m_meshes[item.meshIndex];
                ShadowPC pc{};
                pc.lightSpace = ubo->lightSpaceMatrix[i];
                pc.model      = item.worldMatrix * mesh.LocalTransform();
                vkCmdPushConstants(c, m_shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(ShadowPC), &pc);

                VkBuffer vb = mesh.VertexBuffer();
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(c, 0, 1, &vb, &offset);
                vkCmdBindIndexBuffer(c, mesh.IndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(c, mesh.IndexCount(), 1, 0, 0, 0);
            }

            // Alpha-tested billboard sprites cast cut-out shadows. Different
            // pipeline + layout (push constant is just the light-space matrix,
            // and it samples the atlas in the fragment), so bind it here and
            // re-bind the mesh shadow pipeline afterward for the next cascade.
            if (m_spriteShadowsEnabled && m_spriteCount > 0) {
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_spriteShadowPipeline);
                VkDescriptorSet sets[2] = { m_bindlessSet, m_spriteSsboSet };
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_spriteShadowPipelineLayout, 0, 2, sets, 0, nullptr);
                glm::mat4 ls = ubo->lightSpaceMatrix[i];
                vkCmdPushConstants(c, m_spriteShadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(glm::mat4), &ls);
                VkBuffer vb = m_spriteQuadVertices.Handle();
                VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(c, 0, 1, &vb, &off);
                vkCmdBindIndexBuffer(c, m_spriteQuadIndices.Handle(), 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(c, 6, m_spriteCount, 0, 0, 0);
                // Re-bind the mesh shadow pipeline for the next cascade iteration's meshes.
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
            }

            vkCmdEndRendering(c);
        }
    };
    m_graph.AddPass(std::move(shadowPass));

    // ---- Geometry pass: writes the 5 G-Buffer attachments + depth -------
    GraphPass geometry{};
    geometry.name = "Geometry";
    geometry.writes = {
        { gPos, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT },
        { gNor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT },
        { gAlb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT },
        { gEmi, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT },
        { gMot, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT },
        { depthImg, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
              | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
              | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT },
    };
    geometry.execute = [this, frameIndex, extent](VkCommandBuffer c) {
        VkClearValue clearZero{};
        clearZero.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
        VkClearValue clearDepth{};
        clearDepth.depthStencil = { 1.0f, 0 };

        VkRenderingAttachmentInfo color[5]{};
        const VkImageView views[5] = {
            m_gPosition.View(), m_gNormal.View(),
            m_gAlbedo.View(),   m_gEmissive.View(),
            m_gMotion.View(),
        };
        for (int i = 0; i < 5; ++i) {
            color[i].sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color[i].imageView   = views[i];
            color[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color[i].loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color[i].storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
            color[i].clearValue  = clearZero;
        }

        VkRenderingAttachmentInfo depthAtt{};
        depthAtt.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAtt.imageView   = m_depth.View();
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.clearValue  = clearDepth;

        VkRenderingInfo render{};
        render.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        render.renderArea           = { {0, 0}, extent };
        render.layerCount           = 1;
        render.colorAttachmentCount = 5;
        render.pColorAttachments    = color;
        render.pDepthAttachment     = &depthAtt;

        vkCmdBeginRendering(c, &render);

        VkViewport vp{};
        vp.width    = static_cast<float>(extent.width);
        vp.height   = static_cast<float>(extent.height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(c, 0, 1, &vp);
        VkRect2D scissor{ {0, 0}, extent };
        vkCmdSetScissor(c, 0, 1, &scissor);

        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_geometryPipeline);
        VkDescriptorSet sets[2] = {
            m_frames[frameIndex].frameDescriptorSet,
            m_bindlessSet,
        };
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                                0, 2, sets, 0, nullptr);

        for (const auto& item : m_drawQueue) {
            const MeshVk& mesh = m_meshes[item.meshIndex];

            PushConstants pc{};
            pc.model                   = item.worldMatrix * mesh.LocalTransform();
            pc.baseColorFactor         = mesh.BaseColorFactor();
            pc.materialIndex           = mesh.MaterialIndex();
            pc.metallicRoughnessIndex  = mesh.MetallicRoughnessIndex();
            pc.metallicFactor          = mesh.MetallicFactor();
            pc.roughnessFactor         = mesh.RoughnessFactor();
            pc.specularAA              = m_specularAAEnabled ? 1.0f : 0.0f;
            vkCmdPushConstants(c, m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(PushConstants), &pc);

            VkBuffer vb = mesh.VertexBuffer();
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(c, 0, 1, &vb, &offset);
            vkCmdBindIndexBuffer(c, mesh.IndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(c, mesh.IndexCount(), 1, 0, 0, 0);
        }

        // Sprites: same G-Buffer, same depth, distinct pipeline. Instanced
        // draw of the shared unit quad — all sprite slots in one call.
        if (m_spriteCount > 0) {
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_spritePipeline);
            VkDescriptorSet spriteSets[3] = {
                m_frames[frameIndex].frameDescriptorSet,
                m_bindlessSet,
                m_spriteSsboSet,
            };
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_spritePipelineLayout,
                                    0, 3, spriteSets, 0, nullptr);
            VkBuffer vb = m_spriteQuadVertices.Handle();
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(c, 0, 1, &vb, &offset);
            vkCmdBindIndexBuffer(c, m_spriteQuadIndices.Handle(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(c, 6, m_spriteCount, 0, 0, 0);
        }

        vkCmdEndRendering(c);
    };
    m_graph.AddPass(std::move(geometry));

    // ---- Hi-Z chain: seed (depth → mip 0) + N-1 downsamples (mip i → i+1) --
    // The render graph tracks the image as a whole; we keep all mips in
    // GENERAL throughout the chain and emit a global memory barrier between
    // each dispatch to enforce write→read ordering on the previous mip.
    GraphPass hizPass{};
    hizPass.name = "HiZ_chain";
    hizPass.reads = {
        { depthImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
    };
    hizPass.writes = {
        { hiZ, VK_IMAGE_LAYOUT_GENERAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
    };
    hizPass.execute = [this, extent](VkCommandBuffer c) {
        // Seed: depth → mip 0
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_hizPipeline);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_hizPipelineLayout,
                                0, 1, &m_hizSet, 0, nullptr);
        const uint32_t gx = (extent.width  + 7u) / 8u;
        const uint32_t gy = (extent.height + 7u) / 8u;
        vkCmdDispatch(c, gx, gy, 1);

        // Downsample chain: one dispatch per destination mip. Memory barrier
        // between each — previous mip must be fully written before this mip
        // can sample it.
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_hizDownsamplePipeline);
        uint32_t mipW = extent.width;
        uint32_t mipH = extent.height;
        for (uint32_t mip = 1; mip < m_hiZMipCount; ++mip) {
            VkMemoryBarrier2 mb{};
            mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            mb.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            mb.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            VkDependencyInfo dep{};
            dep.sType                  = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.memoryBarrierCount     = 1;
            dep.pMemoryBarriers        = &mb;
            vkCmdPipelineBarrier2(c, &dep);

            mipW = (mipW > 1) ? (mipW / 2) : 1;
            mipH = (mipH > 1) ? (mipH / 2) : 1;
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_hizPipelineLayout,
                                    0, 1, &m_hizDownsampleSets[mip - 1], 0, nullptr);
            const uint32_t dgx = (mipW + 7u) / 8u;
            const uint32_t dgy = (mipH + 7u) / 8u;
            vkCmdDispatch(c, dgx, dgy, 1);
        }
    };
    m_graph.AddPass(std::move(hizPass));

    // ---- SSR pipeline: sort.count → prefix → sort.scatter → trace ---------
    // SSBO buffer barriers are emitted manually inside the execute lambdas
    // (the graph only tracks images for now). Resource layout convention:
    //   m_hiZ stays in GENERAL (sampleable via m_hiZSampler).
    //   m_ssrResult stays in GENERAL throughout the SSR passes; the lighting
    //   pass transitions it to SHADER_READ_ONLY for sampling.
    GraphResource ssrImg = m_graph.ImportImage(
        "ssrResult", m_ssrResult.Handle(),
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);

    // Half-res SSR: sort + trace run at extent/2 (rounded up) and the trace
    // writes m_ssrResultHalf; a joint-bilateral SSR_Upsample pass reconstructs
    // m_ssrResult at full-res. Disabled → legacy full-res trace into ssrImg.
    const bool ssrHalf = m_ssrHalfResEnabled;
    const uint32_t ssrW = ssrHalf ? (extent.width  + 1u) / 2u : extent.width;
    const uint32_t ssrH = ssrHalf ? (extent.height + 1u) / 2u : extent.height;
    GraphResource ssrHalfImg = m_graph.ImportImage(
        "ssrResultHalf", m_ssrResultHalf.Handle(),
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);

    // Push constant payloads computed once per frame.
    const auto* uboMapped = reinterpret_cast<const CameraUBO*>(
        m_frames[frameIndex].ubo.Mapped());

    SSRSortPC sortPC{};
    sortPC.cameraPos = glm::vec4(glm::vec3(uboMapped->cameraPos), 0.0f);
    sortPC.width  = static_cast<int32_t>(ssrW);
    sortPC.height = static_cast<int32_t>(ssrH);
    sortPC.mode   = 0;  // overwritten per dispatch

    SSRTracePC tracePC{};
    tracePC.view         = uboMapped->view;
    tracePC.proj         = uboMapped->proj;
    tracePC.cameraPos    = glm::vec4(glm::vec3(uboMapped->cameraPos), 0.0f);
    // width/height drive the half-res uv + imageStore; fullWidth/fullHeight keep
    // the ray-march step count tied to the full-res screen footprint.
    tracePC.width        = static_cast<int32_t>(ssrW);
    tracePC.height       = static_cast<int32_t>(ssrH);
    tracePC.fullWidth    = static_cast<int32_t>(extent.width);
    tracePC.fullHeight   = static_cast<int32_t>(extent.height);
    tracePC.hiZMipLevels = static_cast<int32_t>(m_hiZMipCount);
    tracePC.maxDistance  = 30.0f;
    tracePC.thickness    = 0.02f;
    tracePC.frameId      = static_cast<int32_t>(m_taaFrameCounter);
    // taaInIdx already holds the slot last frame's resolve was written to.
    const uint32_t prevFrameSlot = taaInIdx;

    GraphPass ssrSortCount{};
    ssrSortCount.name = "SSR_SortCount";
    ssrSortCount.reads = {
        { gPos, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { gNor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
    };
    ssrSortCount.execute = [this, sortPC, ssrW, ssrH](VkCommandBuffer c) mutable {
        const BufferVk* bufs[3] = { &m_ssrRayCount, &m_ssrBinCounters, &m_ssrScatterCounters };
        // Pre-fill barrier: the SSR compute passes in the previous frame both
        // read and wrote these buffers. Without an explicit COMPUTE→CLEAR
        // dependency, the new frame's vkCmdFillBuffer races the old frame's
        // shader access (vkWaitForFences doesn't satisfy sync-validation).
        {
            VkBufferMemoryBarrier2 bb[3]{};
            for (int i = 0; i < 3; ++i) {
                bb[i].sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                bb[i].srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                bb[i].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                bb[i].dstStageMask  = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                bb[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                bb[i].buffer        = bufs[i]->Handle();
                bb[i].size          = VK_WHOLE_SIZE;
            }
            VkDependencyInfo dep{};
            dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.bufferMemoryBarrierCount = 3;
            dep.pBufferMemoryBarriers    = bb;
            vkCmdPipelineBarrier2(c, &dep);
        }

        // Reset ray count + bin counters + scatter counters to 0. (offsets and
        // sortedRays don't need clearing — they're overwritten.)
        m_ssrRayCount.CmdFill(c, 0u);
        m_ssrBinCounters.CmdFill(c, 0u);
        m_ssrScatterCounters.CmdFill(c, 0u);
        // Barrier: TRANSFER writes → COMPUTE reads/writes.
        VkBufferMemoryBarrier2 bb[3]{};
        for (int i = 0; i < 3; ++i) {
            bb[i].sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            bb[i].srcStageMask  = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            bb[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            bb[i].dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            bb[i].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            bb[i].buffer        = bufs[i]->Handle();
            bb[i].size          = VK_WHOLE_SIZE;
        }
        VkDependencyInfo dep{};
        dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = 3;
        dep.pBufferMemoryBarriers    = bb;
        vkCmdPipelineBarrier2(c, &dep);

        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrSortPipeline);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrSortPipelineLayout,
                                0, 1, &m_ssrSortSet, 0, nullptr);
        sortPC.mode = 0;
        vkCmdPushConstants(c, m_ssrSortPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(SSRSortPC), &sortPC);
        const uint32_t gx = (ssrW + 15u) / 16u;
        const uint32_t gy = (ssrH + 15u) / 16u;
        vkCmdDispatch(c, gx, gy, 1);
    };
    m_graph.AddPass(std::move(ssrSortCount));

    GraphPass ssrPrefix{};
    ssrPrefix.name = "SSR_Prefix";
    ssrPrefix.execute = [this](VkCommandBuffer c) {
        // Sort count's writes to binCounters + totalRayCount must be visible
        // to prefix's reads.
        VkBufferMemoryBarrier2 bb[2]{};
        const BufferVk* bufs[2] = { &m_ssrBinCounters, &m_ssrRayCount };
        for (int i = 0; i < 2; ++i) {
            bb[i].sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            bb[i].srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            bb[i].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            bb[i].dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            bb[i].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            bb[i].buffer        = bufs[i]->Handle();
            bb[i].size          = VK_WHOLE_SIZE;
        }
        VkDependencyInfo dep{};
        dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = 2;
        dep.pBufferMemoryBarriers    = bb;
        vkCmdPipelineBarrier2(c, &dep);

        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrPrefixPipeline);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrPrefixPipelineLayout,
                                0, 1, &m_ssrPrefixSet, 0, nullptr);
        vkCmdDispatch(c, 1, 1, 1);  // single workgroup of 64 threads
    };
    m_graph.AddPass(std::move(ssrPrefix));

    GraphPass ssrSortScatter{};
    ssrSortScatter.name = "SSR_SortScatter";
    ssrSortScatter.reads = {
        { gPos, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { gNor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
    };
    ssrSortScatter.execute = [this, sortPC, ssrW, ssrH](VkCommandBuffer c) mutable {
        // Prefix writes binOffsets + indirectArgs; both readable by us (we
        // only read binOffsets here; indirectArgs goes to the trace dispatch
        // and gets its own barrier there).
        VkBufferMemoryBarrier2 bb{};
        bb.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        bb.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        bb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        bb.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        bb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        bb.buffer        = m_ssrBinOffsets.Handle();
        bb.size          = VK_WHOLE_SIZE;
        VkDependencyInfo dep{};
        dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers    = &bb;
        vkCmdPipelineBarrier2(c, &dep);

        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrSortPipeline);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrSortPipelineLayout,
                                0, 1, &m_ssrSortSet, 0, nullptr);
        sortPC.mode = 1;
        vkCmdPushConstants(c, m_ssrSortPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(SSRSortPC), &sortPC);
        const uint32_t gx = (ssrW + 15u) / 16u;
        const uint32_t gy = (ssrH + 15u) / 16u;
        vkCmdDispatch(c, gx, gy, 1);
    };
    m_graph.AddPass(std::move(ssrSortScatter));

    GraphPass ssrTrace{};
    ssrTrace.name = "SSR_Trace";
    ssrTrace.reads = {
        // Hi-Z is sampled via textureLod across all mips — declared as
        // GENERAL since we keep the chain in GENERAL across the frame.
        { hiZ, VK_IMAGE_LAYOUT_GENERAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { gPos, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { gNor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { gAlb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        // prevFrame source — the TAA history slot written last frame.
        { histIn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
    };
    ssrTrace.writes = {
        // Half-res path writes m_ssrResultHalf (binding 7 already targets it via
        // writeSSRTraceDescriptors); full-res path writes m_ssrResult directly.
        { ssrHalf ? ssrHalfImg : ssrImg, VK_IMAGE_LAYOUT_GENERAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
    };
    ssrTrace.execute = [this, tracePC, prevFrameSlot](VkCommandBuffer c) {
        // Scatter writes sortedPixels; trace reads. Same for indirectArgs:
        // prefix writes → indirect dispatch reads.
        VkBufferMemoryBarrier2 bb[2]{};
        bb[0].sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        bb[0].srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        bb[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        bb[0].dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        bb[0].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        bb[0].buffer        = m_ssrSortedRays.Handle();
        bb[0].size          = VK_WHOLE_SIZE;
        bb[1].sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        bb[1].srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        bb[1].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        bb[1].dstStageMask  = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        bb[1].dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        bb[1].buffer        = m_ssrIndirectArgs.Handle();
        bb[1].size          = VK_WHOLE_SIZE;
        VkDependencyInfo dep{};
        dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = 2;
        dep.pBufferMemoryBarriers    = bb;
        vkCmdPipelineBarrier2(c, &dep);

        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrTracePipeline);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrTracePipelineLayout,
                                0, 1, &m_ssrTraceSets[prevFrameSlot], 0, nullptr);
        vkCmdPushConstants(c, m_ssrTracePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(SSRTracePC), &tracePC);
        vkCmdDispatchIndirect(c, m_ssrIndirectArgs.Handle(), 0);
    };
    m_graph.AddPass(std::move(ssrTrace));

    // ---- SSR_Upsample: joint-bilateral reconstruction half→full ----------
    // Only when half-res SSR is active. Reads the half-res trace output + the
    // full-res G-Buffer (position/normal) and writes the full-res m_ssrResult
    // the SVGF temporal pass consumes.
    if (ssrHalf) {
        SSRUpsamplePC upPC{};
        upPC.fullWidth  = static_cast<int32_t>(extent.width);
        upPC.fullHeight = static_cast<int32_t>(extent.height);
        upPC.halfWidth  = static_cast<int32_t>(ssrW);
        upPC.halfHeight = static_cast<int32_t>(ssrH);
        GraphPass ssrUpsample{};
        ssrUpsample.name = "SSR_Upsample";
        ssrUpsample.reads = {
            { ssrHalfImg, VK_IMAGE_LAYOUT_GENERAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
            { gPos, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
            { gNor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        };
        ssrUpsample.writes = {
            { ssrImg, VK_IMAGE_LAYOUT_GENERAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
        };
        ssrUpsample.execute = [this, upPC, extent](VkCommandBuffer c) {
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrUpsamplePipeline);
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrUpsamplePipelineLayout,
                                    0, 1, &m_ssrUpsampleSet, 0, nullptr);
            vkCmdPushConstants(c, m_ssrUpsamplePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(SSRUpsamplePC), &upPC);
            const uint32_t gx = (extent.width  + 15u) / 16u;
            const uint32_t gy = (extent.height + 15u) / 16u;
            vkCmdDispatch(c, gx, gy, 1);
        };
        m_graph.AddPass(std::move(ssrUpsample));
    }

    // ---- SVGF: temporal + 3× spatial A-Trous (F13.E) ---------------------
    // Frame-level ping-pong for color/moments history. curSlot writes this
    // frame, prevSlot is the history input.
    const uint32_t svgfCurSlot  = m_svgfFrameCounter % 2u;
    const uint32_t svgfPrevSlot = 1u - svgfCurSlot;
    const int      historyValid = (m_svgfFrameCounter > 0u) ? 1 : 0;
    ++m_svgfFrameCounter;

    // Patch the gbuffer set binding 9 to point at this frame's curSlot color
    // (the final SVGF spatial output). UPDATE_AFTER_BIND on the layout makes
    // this legal mid-frame; the lighting bind happens after this update.
    {
        VkDescriptorImageInfo ssrColInfo{};
        ssrColInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ssrColInfo.imageView   = m_ssrColor[svgfCurSlot].View();
        ssrColInfo.sampler     = m_gbufferSampler;
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = m_gbufferSet[frameIndex];  // only this frame's set; GPU is no longer reading it (fence waited in BeginFrame)
        w.dstBinding      = 9;
        w.descriptorCount = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo      = &ssrColInfo;
        vkUpdateDescriptorSets(m_device.Handle(), 1, &w, 0, nullptr);
    }

    // Import SVGF images. prevSlot/curSlot color & moments persist content
    // frame-to-frame; the render graph tracks UNDEFINED each frame and the
    // implementation preserves the contents in practice (same pattern as TAA).
    GraphResource ssrColPrev = m_graph.ImportImage(
        "ssrColPrev", m_ssrColor[svgfPrevSlot].Handle(),
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource ssrColCur  = m_graph.ImportImage(
        "ssrColCur", m_ssrColor[svgfCurSlot].Handle(),
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource ssrColTemp = m_graph.ImportImage(
        "ssrColTemp", m_ssrColorTemp.Handle(),
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource ssrMomPrev = m_graph.ImportImage(
        "ssrMomPrev", m_ssrMoments[svgfPrevSlot].Handle(),
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource ssrMomCur  = m_graph.ImportImage(
        "ssrMomCur", m_ssrMoments[svgfCurSlot].Handle(),
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);

    // --- SVGF Temporal: reads m_ssrResult + history → ssrColTemp + momCur ---
    GraphPass svgfTemporal{};
    svgfTemporal.name = "SVGF_Temporal";
    svgfTemporal.reads = {
        // Raw SSR was written in GENERAL by the trace; transition to
        // SHADER_READ_ONLY_OPTIMAL so the combined-image-sampler descriptor
        // matches its declared layout. The graph emits the barrier.
        { ssrImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { ssrColPrev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { ssrMomPrev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { gNor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { gPos, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { gMot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
    };
    svgfTemporal.writes = {
        { ssrColTemp, VK_IMAGE_LAYOUT_GENERAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
        { ssrMomCur, VK_IMAGE_LAYOUT_GENERAL,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
    };
    svgfTemporal.execute = [this, svgfCurSlot, historyValid, extent](VkCommandBuffer c) {
        struct PC { int32_t width, height, historyValid, _pad0; } pc;
        pc.width        = static_cast<int32_t>(extent.width);
        pc.height       = static_cast<int32_t>(extent.height);
        pc.historyValid = historyValid;
        pc._pad0        = 0;
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrTemporalPipeline);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrTemporalPipelineLayout,
                                0, 1, &m_ssrTemporalSets[svgfCurSlot], 0, nullptr);
        vkCmdPushConstants(c, m_ssrTemporalPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(PC), &pc);
        const uint32_t gx = (extent.width  + 15u) / 16u;
        const uint32_t gy = (extent.height + 15u) / 16u;
        vkCmdDispatch(c, gx, gy, 1);
    };
    m_graph.AddPass(std::move(svgfTemporal));

    // --- SVGF Spatial × 3 (step 1 → 2 → 4) ---------------------------------
    // Direction alternates: iter1 reads Temp → writes ColCur (dir 0),
    //                       iter2 reads ColCur → writes Temp (dir 1),
    //                       iter3 reads Temp → writes ColCur (dir 0, final).
    struct SpatialPC {
        int32_t width, height, stepSize, finalIteration;
        float   sigmaLuminance, sigmaNormal, sigmaDepth, _pad0;
    };
    static_assert(sizeof(SpatialPC) == 32, "SVGF spatial PC must be 32 B");

    auto addSpatialPass = [&](int iter, int stepSize, int finalIter,
                              GraphResource srcRes, GraphResource dstRes,
                              uint32_t direction) {
        GraphPass p{};
        p.name = std::string("SVGF_Spatial_iter") + std::to_string(iter);
        p.reads = {
            { srcRes,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
            { ssrMomCur, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
            { gNor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
            { gPos, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        };
        p.writes = {
            { dstRes, VK_IMAGE_LAYOUT_GENERAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
        };
        p.execute = [this, svgfCurSlot, direction, stepSize, finalIter, extent](VkCommandBuffer c) {
            SpatialPC pc{};
            pc.width          = static_cast<int32_t>(extent.width);
            pc.height         = static_cast<int32_t>(extent.height);
            pc.stepSize       = stepSize;
            pc.finalIteration = finalIter;
            pc.sigmaLuminance = 4.0f;
            pc.sigmaNormal    = 0.4f;
            pc.sigmaDepth     = 1.0f;
            pc._pad0          = 0.0f;
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrSpatialPipeline);
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    m_ssrSpatialPipelineLayout,
                                    0, 1, &m_ssrSpatialSets[svgfCurSlot][direction], 0, nullptr);
            vkCmdPushConstants(c, m_ssrSpatialPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(SpatialPC), &pc);
            const uint32_t gx = (extent.width  + 15u) / 16u;
            const uint32_t gy = (extent.height + 15u) / 16u;
            vkCmdDispatch(c, gx, gy, 1);
        };
        m_graph.AddPass(std::move(p));
    };

    addSpatialPass(1, 1, 0, ssrColTemp, ssrColCur,  0);  // Temp  → ColCur
    addSpatialPass(2, 2, 0, ssrColCur,  ssrColTemp, 1);  // ColCur → Temp
    addSpatialPass(3, 4, 1, ssrColTemp, ssrColCur,  0);  // Temp  → ColCur (final, firefly comp)

    // ---- Lighting pass: reads 5 G-Buffer attachments + shadow + SSR -----
    GraphPass lighting{};
    lighting.name = "Lighting";
    lighting.reads = {
        { gPos, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { gNor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { gAlb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { gEmi, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { gMot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { shadow, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        // SVGF denoised result for *this* frame — graph emits GENERAL →
        // SHADER_READ_ONLY barrier after the last spatial pass.
        { ssrColCur, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
    };
    lighting.writes = {
        { sceneCol, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT },
    };
    lighting.execute = [this, frameIndex, extent](VkCommandBuffer c) {
        VkClearValue clearColor{};
        clearColor.color = { { 0.04f, 0.05f, 0.08f, 1.0f } };

        VkRenderingAttachmentInfo colorAtt{};
        colorAtt.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtt.imageView   = m_sceneColor.View();
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.clearValue  = clearColor;

        VkRenderingInfo render{};
        render.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        render.renderArea           = { {0, 0}, extent };
        render.layerCount           = 1;
        render.colorAttachmentCount = 1;
        render.pColorAttachments    = &colorAtt;

        vkCmdBeginRendering(c, &render);

        VkViewport vp{};
        vp.width    = static_cast<float>(extent.width);
        vp.height   = static_cast<float>(extent.height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(c, 0, 1, &vp);
        VkRect2D scissor{ {0, 0}, extent };
        vkCmdSetScissor(c, 0, 1, &scissor);

        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lightingPipeline);
        VkDescriptorSet sets[3] = {
            m_frames[frameIndex].frameDescriptorSet,
            m_bindlessSet,
            m_gbufferSet[frameIndex],
        };
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                                0, 3, sets, 0, nullptr);
        vkCmdDraw(c, 3, 1, 0, 0);  // fullscreen triangle from gl_VertexIndex

        vkCmdEndRendering(c);
    };
    m_graph.AddPass(std::move(lighting));

    // ---- Particle update (compute) + render (alpha-blend over sceneColor) --
    // The update consumes m_particleBuffer in COMPUTE; the render needs the
    // updated buffer visible in VERTEX. We emit memory barriers manually
    // (the graph tracks images only).
    if (m_particlesEnabled && !m_particleEmitters.empty()) {
        uint32_t totalParticles = 0;
        for (const auto& e : m_particleEmitters) totalParticles += uint32_t(e.desc.maxParticles);
        const float dt = m_dt;

        GraphPass partUpdate{};
        partUpdate.name = "ParticleUpdate";
        partUpdate.execute = [this, totalParticles, dt](VkCommandBuffer c) {
            // Host-coherent writes were just done in emitParticles — Vulkan
            // requires no explicit flush, but we still need a HOST→COMPUTE
            // barrier to make them visible to the shader.
            VkBufferMemoryBarrier2 bb{};
            bb.sType        = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            bb.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            bb.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
            bb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            bb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                             | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            bb.buffer = m_particleBuffer.Handle();
            bb.size   = VK_WHOLE_SIZE;
            VkDependencyInfo dep{};
            dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.bufferMemoryBarrierCount = 1;
            dep.pBufferMemoryBarriers    = &bb;
            vkCmdPipelineBarrier2(c, &dep);

            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_particleUpdatePipeline);
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    m_particleUpdatePipelineLayout,
                                    0, 1, &m_particleSsboSet, 0, nullptr);
            struct UpdatePC { float dt; int32_t count; } pc{ dt, int32_t(totalParticles) };
            vkCmdPushConstants(c, m_particleUpdatePipelineLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UpdatePC), &pc);
            vkCmdDispatch(c, (totalParticles + 255u) / 256u, 1, 1);
        };
        m_graph.AddPass(std::move(partUpdate));

        GraphPass partRender{};
        partRender.name = "ParticleRender";
        // Soft particles sample gPosition — declare the read so the graph keeps
        // it in SHADER_READ_ONLY_OPTIMAL during this pass.
        partRender.reads = {
            { gPos, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        };
        // Alpha blend reads the destination, so declare READ too — this
        // forces the graph to emit a RAW barrier vs the lighting pass's
        // pure WRITE.
        partRender.writes = {
            { sceneCol, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT },
        };
        partRender.execute = [this, totalParticles, extent, frameIndex](VkCommandBuffer c) {
            // Compute write → vertex read on the same SSBO.
            VkBufferMemoryBarrier2 bb{};
            bb.sType        = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            bb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            bb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            bb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
            bb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            bb.buffer = m_particleBuffer.Handle();
            bb.size   = VK_WHOLE_SIZE;
            VkDependencyInfo dep{};
            dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.bufferMemoryBarrierCount = 1;
            dep.pBufferMemoryBarriers    = &bb;
            vkCmdPipelineBarrier2(c, &dep);

            VkRenderingAttachmentInfo color{};
            color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView   = m_sceneColor.View();
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;   // preserve sceneColor from lighting
            color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo render{};
            render.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
            render.renderArea           = { {0, 0}, extent };
            render.layerCount           = 1;
            render.colorAttachmentCount = 1;
            render.pColorAttachments    = &color;
            vkCmdBeginRendering(c, &render);

            VkViewport vp{};
            vp.width    = static_cast<float>(extent.width);
            vp.height   = static_cast<float>(extent.height);
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(c, 0, 1, &vp);
            VkRect2D scissor{ {0, 0}, extent };
            vkCmdSetScissor(c, 0, 1, &scissor);

            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_particleRenderPipeline);
            VkDescriptorSet sets[3] = { m_frames[frameIndex].frameDescriptorSet,
                                        m_particleSsboSet, m_gbufferSet[frameIndex] };
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_particleRenderPipelineLayout,
                                    0, 3, sets, 0, nullptr);
            struct ParticlePC {
                float softEnabled, fadeDist, motionBlur, stretchScale;
            } spc{
                m_softParticlesEnabled       ? 1.0f : 0.0f, 0.5f,
                m_particleMotionBlurEnabled  ? 1.0f : 0.0f, m_particleStretchScale,
            };
            vkCmdPushConstants(c, m_particleRenderPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(ParticlePC), &spc);
            // 4 vertices per particle (triangle strip), `totalParticles` instances.
            vkCmdDraw(c, 4, totalParticles, 0, 0);

            vkCmdEndRendering(c);
        };
        m_graph.AddPass(std::move(partRender));
    }

    // ---- TAA pass: resolves jittered sceneColor with reprojected history --
    // MRT — location 0 writes swapchain (LDR-friendly), location 1 writes the
    // ping-pong history slot that feeds *next* frame's input. On the very first
    // frame there is no history yet: sample sceneColor as history so the EMA
    // is a no-op and we don't read undefined memory.
    // Post-process scratch is the TAA output target — read by the tonemap
    // pass next.
    GraphResource postScratch = m_graph.ImportImage(
        "postScratch", m_postScratch.Handle(),
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);

    // E1.A — Tonemap writes here instead of the swapchain. A subsequent
    // pass blits it back to the swapchain while we wire the FluentUI Image
    // widget; later (E1.C) the UI pass will sample it directly into the
    // editor's center viewport panel and the blit goes away.
    GraphResource viewportColor = m_graph.ImportImage(
        "viewportColor", m_viewportColor.Handle(),
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);

    // CAS intermediate. When CAS is enabled the tonemap writes here and the
    // CAS pass reads it before writing m_viewportColor.
    GraphResource tonemapLdrImg = m_graph.ImportImage(
        "tonemapLdr", m_tonemapLdr.Handle(),
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);

    GraphPass taaPass{};
    taaPass.name = "TAA";
    taaPass.reads = {
        { sceneCol, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { gMot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { histIn, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
    };
    taaPass.writes = {
        // HDR linear output — input to the tonemap pass. Replaces the previous
        // direct write to the swapchain (which forced a tonemap-by-truncation).
        { postScratch, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT },
        { histOut, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT },
    };
    taaPass.execute = [this, taaInIdx, taaOutIdx, extent](VkCommandBuffer c) {
        VkRenderingAttachmentInfo color[2]{};
        color[0].sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color[0].imageView   = m_postScratch.View();
        color[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color[0].loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color[0].storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

        color[1].sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color[1].imageView   = m_taaHistory[taaOutIdx].View();
        color[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color[1].loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color[1].storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo render{};
        render.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        render.renderArea           = { {0, 0}, extent };
        render.layerCount           = 1;
        render.colorAttachmentCount = 2;
        render.pColorAttachments    = color;

        vkCmdBeginRendering(c, &render);

        VkViewport vp{};
        vp.width    = static_cast<float>(extent.width);
        vp.height   = static_cast<float>(extent.height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(c, 0, 1, &vp);
        VkRect2D scissor{ {0, 0}, extent };
        vkCmdSetScissor(c, 0, 1, &scissor);

        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_taaPipeline);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_taaPipelineLayout,
                                0, 1, &m_taaSets[taaInIdx], 0, nullptr);
        vkCmdDraw(c, 3, 1, 0, 0);

        vkCmdEndRendering(c);
    };
    m_graph.AddPass(std::move(taaPass));

    // ---- Volumetric fog (F14.D): compute half-res scatter+transmittance ----
    GraphResource volFogImg = m_graph.ImportImage(
        "volFog", m_volFog.Handle(),
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);

    if (m_fogEnabled) {
        const uint32_t fogW = m_volFog.Width();
        const uint32_t fogH = m_volFog.Height();
        const int   fogSteps          = m_fogSteps;
        const float fogDensityFloor   = m_fogDensityFloor;
        const float fogDensityScale   = m_fogDensityScale;
        const float fogHeightFactor   = m_fogHeightFactor;
        const float fogScatterStrength = m_fogScatterStrength;
        const float fogMaxDistance    = m_fogMaxDistance;
        const glm::vec3 fogColor      = m_fogColor;

        GraphPass fogPass{};
        fogPass.name = "VolFog";
        fogPass.reads = {
            { depthImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
            // CSM shadow map — read for god-ray shadowing in the raymarch.
            { shadow, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        };
        fogPass.writes = {
            { volFogImg, VK_IMAGE_LAYOUT_GENERAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
        };
        fogPass.execute = [this, fogW, fogH, fogSteps, fogDensityFloor, fogDensityScale,
                           fogHeightFactor, fogScatterStrength, fogMaxDistance,
                           fogColor, frameIndex](VkCommandBuffer c) {
            const CameraUBO* ubo = reinterpret_cast<const CameraUBO*>(
                m_frames[frameIndex].ubo.Mapped());
            // Layout must match vol_fog.comp's push_constant exactly. GLSL packs
            // a float right after the three ints (no padding), so no _pad here.
            struct FogPC {
                glm::mat4 invViewProj;
                glm::vec4 cameraPos;
                glm::vec4 sunDir;
                glm::vec4 fogColor;
                int32_t   dstWidth, dstHeight, steps;
                float     densityFloor, densityScale, heightFactor, scatterStrength;
                float     maxDistance;
                int32_t   historyValid;
                float     temporalAlpha;
                float     jitter;
            } pc;
            pc.invViewProj     = glm::inverse(ubo->proj * ubo->view);
            pc.cameraPos       = ubo->cameraPos;
            pc.sunDir          = glm::vec4(-glm::vec3(ubo->lightDirection), 0.0f);
            pc.fogColor        = glm::vec4(fogColor, m_godRaysEnabled ? 1.0f : 0.0f);  // .a = god rays toggle
            pc.dstWidth        = static_cast<int32_t>(fogW);
            pc.dstHeight       = static_cast<int32_t>(fogH);
            pc.steps           = fogSteps;
            pc.densityFloor    = fogDensityFloor;
            pc.densityScale    = fogDensityScale;
            pc.heightFactor    = fogHeightFactor;
            pc.scatterStrength = fogScatterStrength;
            pc.maxDistance     = fogMaxDistance;

            // Temporal reprojection: only blend once a valid history exists.
            const bool fogTemporal = m_fogTemporalEnabled;
            pc.historyValid  = (fogTemporal && m_fogHistoryInitialized) ? 1 : 0;
            pc.temporalAlpha = m_fogTemporalAlpha;
            // Golden-ratio low-discrepancy jitter in [0,1) so the first-sample
            // offset decorrelates banding across frames. 0.5 when temporal is off.
            const double gr = (double)m_taaFrameCounter * 0.6180339887498949;
            pc.jitter = fogTemporal ? (float)(gr - std::floor(gr)) : 0.5f;

            // Make the history readable by the raymarch this frame. Always leave
            // it in SHADER_READ_ONLY before the dispatch (even when historyValid
            // is 0, the descriptor still points at it — sampling an image in the
            // wrong layout is UB regardless of whether the result is used).
            if (fogTemporal) {
                m_volFogHistory.CmdTransition(
                    c,
                    m_fogHistoryInitialized ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                            : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    m_fogHistoryInitialized ? VK_PIPELINE_STAGE_2_COPY_BIT
                                            : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                    m_fogHistoryInitialized ? VK_ACCESS_2_TRANSFER_WRITE_BIT
                                            : (VkAccessFlags2)0,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }

            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_volFogPipeline);
            VkDescriptorSet fogSets[2] = { m_volFogSet, m_frames[frameIndex].frameDescriptorSet };
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_volFogPipelineLayout,
                                    0, 2, fogSets, 0, nullptr);
            vkCmdPushConstants(c, m_volFogPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(FogPC), &pc);
            vkCmdDispatch(c, (fogW + 7u) / 8u, (fogH + 7u) / 8u, 1);

            // Copy this frame's blended fog into the history for next frame.
            // The blend already lives in m_volFog, so the accumulation carries
            // forward correctly. m_volFog must be restored to GENERAL on exit
            // (the graph declared the write as GENERAL and the tonemap reads it
            // from there afterwards).
            if (fogTemporal) {
                VkImageMemoryBarrier2 pre[2]{};
                // m_volFog: GENERAL (compute store) -> TRANSFER_SRC for the copy.
                pre[0].sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                pre[0].srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                pre[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                pre[0].dstStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
                pre[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                pre[0].oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
                pre[0].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                pre[0].image         = m_volFog.Handle();
                pre[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                // m_volFogHistory: SHADER_READ (sampled above) -> TRANSFER_DST.
                pre[1].sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                pre[1].srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                pre[1].srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                pre[1].dstStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
                pre[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                pre[1].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                pre[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                pre[1].image         = m_volFogHistory.Handle();
                pre[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                VkDependencyInfo depPre{};
                depPre.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                depPre.imageMemoryBarrierCount = 2;
                depPre.pImageMemoryBarriers    = pre;
                vkCmdPipelineBarrier2(c, &depPre);

                VkImageCopy region{};
                region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.srcOffset      = { 0, 0, 0 };
                region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.dstOffset      = { 0, 0, 0 };
                region.extent         = { fogW, fogH, 1 };
                vkCmdCopyImage(c, m_volFog.Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_volFogHistory.Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);

                // Restore m_volFog to GENERAL — the graph expects the fog write
                // to leave it in GENERAL so the tonemap barrier (GENERAL ->
                // SHADER_READ) is valid. m_volFogHistory stays in TRANSFER_DST;
                // next frame's pre-dispatch barrier transitions it from there.
                m_volFog.CmdTransition(
                    c,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

                m_fogHistoryInitialized = true;
            }
        };
        m_graph.AddPass(std::move(fogPass));
    }

    // ---- Bloom chain: prefilter → downsample × (N-1) → upsample × (N-1) ----
    GraphResource bloomDownImg = m_graph.ImportImage(
        "bloomDown", m_bloomDown.Handle(),
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource bloomUpImg = m_graph.ImportImage(
        "bloomUp", m_bloomUp.Handle(),
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);

    if (m_bloomEnabled && m_bloomMipCount > 1) {
        const uint32_t mip0W = m_bloomDown.Width();
        const uint32_t mip0H = m_bloomDown.Height();
        const uint32_t mips  = m_bloomMipCount;
        const float    bloomThreshold = m_bloomThreshold;
        const float    bloomKnee      = m_bloomKnee;
        const float    upIntensity    = m_bloomUpsampleIntensity;

        GraphPass bloomPass{};
        bloomPass.name = "Bloom";
        bloomPass.reads = {
            { postScratch, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        };
        bloomPass.writes = {
            { bloomDownImg, VK_IMAGE_LAYOUT_GENERAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
            { bloomUpImg, VK_IMAGE_LAYOUT_GENERAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        };
        bloomPass.execute = [this, mip0W, mip0H, mips, bloomThreshold, bloomKnee, upIntensity]
                            (VkCommandBuffer c) {
            // Prefilter: postScratch → bloomDown[0]
            {
                struct PrefPC { float threshold, knee; int32_t w, h; } pc;
                pc.threshold = bloomThreshold;
                pc.knee      = bloomKnee;
                pc.w         = static_cast<int32_t>(mip0W);
                pc.h         = static_cast<int32_t>(mip0H);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_bloomPrefilterPipeline);
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        m_bloomPrefilterPipelineLayout,
                                        0, 1, &m_bloomPrefilterSet, 0, nullptr);
                vkCmdPushConstants(c, m_bloomPrefilterPipelineLayout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PrefPC), &pc);
                const uint32_t gx = (mip0W + 7u) / 8u;
                const uint32_t gy = (mip0H + 7u) / 8u;
                vkCmdDispatch(c, gx, gy, 1);
            }

            auto memBarrier = [&]() {
                VkMemoryBarrier2 mb{};
                mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                mb.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                mb.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                                 | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                VkDependencyInfo dep{};
                dep.sType                = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.memoryBarrierCount   = 1;
                dep.pMemoryBarriers      = &mb;
                vkCmdPipelineBarrier2(c, &dep);
            };

            // Downsample chain: down[i] → down[i+1]
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_bloomDownsamplePipeline);
            uint32_t mw = mip0W, mh = mip0H;
            for (uint32_t i = 1; i < mips; ++i) {
                memBarrier();
                mw = (mw > 1) ? (mw / 2) : 1;
                mh = (mh > 1) ? (mh / 2) : 1;
                struct DnPC { int32_t w, h; } pc{ int32_t(mw), int32_t(mh) };
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        m_bloomDownsamplePipelineLayout,
                                        0, 1, &m_bloomDownsampleSets[i - 1], 0, nullptr);
                vkCmdPushConstants(c, m_bloomDownsamplePipelineLayout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DnPC), &pc);
                vkCmdDispatch(c, (mw + 7u) / 8u, (mh + 7u) / 8u, 1);
            }

            // Upsample chain: deepest first. Writes up[i] from up[i+1] (or down[i+1]
            // on the first iter) + down[i]. Sets are ordered [deepest..mip 0].
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_bloomUpsamplePipeline);
            for (uint32_t k = 0; k < mips - 1; ++k) {
                memBarrier();
                const uint32_t dstMip = (mips - 2) - k;
                const uint32_t dw = std::max(mip0W >> dstMip, 1u);
                const uint32_t dh = std::max(mip0H >> dstMip, 1u);
                struct UpPC { int32_t w, h; float radius, intensity; } pc{
                    int32_t(dw), int32_t(dh), 1.0f, upIntensity,
                };
                vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        m_bloomUpsamplePipelineLayout,
                                        0, 1, &m_bloomUpsampleSets[k], 0, nullptr);
                vkCmdPushConstants(c, m_bloomUpsamplePipelineLayout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(UpPC), &pc);
                vkCmdDispatch(c, (dw + 7u) / 8u, (dh + 7u) / 8u, 1);
            }
        };
        m_graph.AddPass(std::move(bloomPass));
    }

    // ---- Auto-exposure: histogram build + average → m_exposureBuffer ------
    // Two compute passes read the HDR postScratch and write the temporally-
    // adapted average luminance that the tonemap below scales by. Only run when
    // enabled; otherwise the tonemap falls back to manual exp2(exposure).
    if (m_autoExposureEnabled) {
        HistBuildPC hbPC{};
        hbPC.width       = static_cast<int32_t>(extent.width);
        hbPC.height      = static_cast<int32_t>(extent.height);
        hbPC.logMin      = m_autoExposureLogMin;
        hbPC.invLogRange = 1.0f / (m_autoExposureLogMax - m_autoExposureLogMin);

        HistAvgPC haPC{};
        haPC.logMin     = m_autoExposureLogMin;
        haPC.logRange   = m_autoExposureLogMax - m_autoExposureLogMin;
        haPC.dt         = m_dt;
        haPC.tau        = m_autoExposureTau;
        haPC.pixelCount = extent.width * extent.height;
        haPC.minLum     = m_autoExposureMinLum;
        haPC.maxLum     = m_autoExposureMaxLum;

        GraphPass histBuild{};
        histBuild.name  = "Histogram_Build";
        histBuild.reads = {
            { postScratch, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        };
        histBuild.execute = [this, hbPC, extent](VkCommandBuffer c) {
            // Reset the shared histogram at the start of each frame. The buffer is
            // shared across frames in flight, so the previous frame's average pass
            // (which read it) must finish before we clear, and the clear must be
            // visible before this frame's atomicAdds — same dance as the SSR
            // counters. Without this the clear races the prior frame's access.
            {
                VkBufferMemoryBarrier2 pre{};
                pre.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                pre.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                pre.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                pre.dstStageMask  = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                pre.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                pre.buffer        = m_histogramBuffer.Handle();
                pre.size          = VK_WHOLE_SIZE;
                VkDependencyInfo dep{};
                dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.bufferMemoryBarrierCount = 1;
                dep.pBufferMemoryBarriers    = &pre;
                vkCmdPipelineBarrier2(c, &dep);
            }
            m_histogramBuffer.CmdFill(c, 0u);
            {
                VkBufferMemoryBarrier2 post{};
                post.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                post.srcStageMask  = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                post.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                post.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                post.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                post.buffer        = m_histogramBuffer.Handle();
                post.size          = VK_WHOLE_SIZE;
                VkDependencyInfo dep{};
                dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.bufferMemoryBarrierCount = 1;
                dep.pBufferMemoryBarriers    = &post;
                vkCmdPipelineBarrier2(c, &dep);
            }
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_histBuildPipeline);
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_histBuildPipelineLayout,
                                    0, 1, &m_histBuildSet, 0, nullptr);
            vkCmdPushConstants(c, m_histBuildPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(HistBuildPC), &hbPC);
            const uint32_t gx = (extent.width  + 15u) / 16u;
            const uint32_t gy = (extent.height + 15u) / 16u;
            vkCmdDispatch(c, gx, gy, 1);
        };
        m_graph.AddPass(std::move(histBuild));

        GraphPass histAvg{};
        histAvg.name = "Histogram_Average";
        histAvg.execute = [this, haPC](VkCommandBuffer c) {
            // build (atomicAdd writes) → average (reads) on m_histogramBuffer.
            VkBufferMemoryBarrier2 bb{};
            bb.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            bb.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            bb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            bb.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            bb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            bb.buffer        = m_histogramBuffer.Handle();
            bb.size          = VK_WHOLE_SIZE;
            VkDependencyInfo dep{};
            dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.bufferMemoryBarrierCount = 1;
            dep.pBufferMemoryBarriers    = &bb;
            vkCmdPipelineBarrier2(c, &dep);

            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_histAvgPipeline);
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, m_histAvgPipelineLayout,
                                    0, 1, &m_histAvgSet, 0, nullptr);
            vkCmdPushConstants(c, m_histAvgPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(HistAvgPC), &haPC);
            vkCmdDispatch(c, 1, 1, 1);

            // average (writes m_exposureBuffer) → tonemap (fragment reads it).
            VkBufferMemoryBarrier2 eb{};
            eb.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            eb.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            eb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            eb.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            eb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            eb.buffer        = m_exposureBuffer.Handle();
            eb.size          = VK_WHOLE_SIZE;
            VkDependencyInfo dep2{};
            dep2.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep2.bufferMemoryBarrierCount = 1;
            dep2.pBufferMemoryBarriers    = &eb;
            vkCmdPipelineBarrier2(c, &dep2);
        };
        m_graph.AddPass(std::move(histAvg));
    }

    // ---- Tonemap: HDR postScratch + bloom → viewportColor (ACES filmic) ----
    // E1.A: target is the offscreen viewport image, not the swapchain. A
    // BlitToSwapchain pass below copies it back so visual output matches
    // pre-E1.A until the editor's center panel samples viewportColor.
    // When CAS is enabled the tonemap writes the intermediate m_tonemapLdr and
    // the CAS pass below produces m_viewportColor; otherwise the tonemap writes
    // m_viewportColor directly (legacy path, no sharpening).
    const bool casOn = m_casEnabled;

    GraphPass tonemap{};
    tonemap.name = "Tonemap";
    tonemap.reads = {
        { postScratch, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { bloomUpImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        { volFogImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
    };
    tonemap.writes = {
        { (casOn ? tonemapLdrImg : viewportColor), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT },
    };
    tonemap.execute = [this, extent, casOn](VkCommandBuffer c) {
        VkClearValue clearBlack{};
        clearBlack.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

        VkRenderingAttachmentInfo color{};
        color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView   = casOn ? m_tonemapLdr.View() : m_viewportColor.View();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue  = clearBlack;

        VkRenderingInfo render{};
        render.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
        render.renderArea           = { {0, 0}, extent };
        render.layerCount           = 1;
        render.colorAttachmentCount = 1;
        render.pColorAttachments    = &color;

        vkCmdBeginRendering(c, &render);

        VkViewport vp{};
        vp.width    = static_cast<float>(extent.width);
        vp.height   = static_cast<float>(extent.height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(c, 0, 1, &vp);
        VkRect2D scissor{ {0, 0}, extent };
        vkCmdSetScissor(c, 0, 1, &scissor);

        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapPipeline);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapPipelineLayout,
                                0, 1, &m_tonemapSet, 0, nullptr);
        struct TonePC {
            float exposure, bloomIntensity, contrast, saturation;
            float bloomEnabled, fogEnabled, celEnabled, celSteps;
            float autoExposureEnabled, exposureKey;
        } pc{
            m_exposure, m_bloomIntensity, m_contrast, m_saturation,
            (m_bloomEnabled ? 1.0f : 0.0f),
            (m_fogEnabled   ? 1.0f : 0.0f),
            (m_celEnabled   ? 1.0f : 0.0f),
            m_celSteps,
            (m_autoExposureEnabled ? 1.0f : 0.0f),
            m_autoExposureKey,
        };
        vkCmdPushConstants(c, m_tonemapPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(TonePC), &pc);
        vkCmdDraw(c, 3, 1, 0, 0);

        vkCmdEndRendering(c);
    };
    m_graph.AddPass(std::move(tonemap));

    // ---- CAS: sharpen m_tonemapLdr → m_viewportColor (AMD FidelityFX CAS).
    //      Only emitted when enabled; otherwise the tonemap already wrote
    //      m_viewportColor and no sharpening runs.
    if (casOn) {
        GraphPass cas{};
        cas.name = "CAS";
        cas.reads = {
            { tonemapLdrImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        };
        cas.writes = {
            { viewportColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT },
        };
        cas.execute = [this, extent](VkCommandBuffer c) {
            VkRenderingAttachmentInfo color{};
            color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView   = m_viewportColor.View();
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo render{};
            render.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
            render.renderArea           = { {0, 0}, extent };
            render.layerCount           = 1;
            render.colorAttachmentCount = 1;
            render.pColorAttachments    = &color;

            vkCmdBeginRendering(c, &render);
            VkViewport vp{};
            vp.width = (float)extent.width; vp.height = (float)extent.height;
            vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
            vkCmdSetViewport(c, 0, 1, &vp);
            VkRect2D sc{ {0,0}, extent };
            vkCmdSetScissor(c, 0, 1, &sc);
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_casPipeline);
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_casPipelineLayout,
                                    0, 1, &m_casSet, 0, nullptr);
            float sharpness = m_casSharpness;
            vkCmdPushConstants(c, m_casPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(float), &sharpness);
            vkCmdDraw(c, 3, 1, 0, 0);
            vkCmdEndRendering(c);
        };
        m_graph.AddPass(std::move(cas));
    }

    // ---- Debug lines: overlay the queued line segments onto m_viewportColor
    //      (LOAD so the scene is preserved). No depth test — always visible.
    //      Inserted after CAS and before the UI pass.
    if (m_debugLineCount > 0 || m_debugTriCount > 0) {
        GraphPass lines{};
        lines.name = "DebugLines";
        lines.writes = {
            { viewportColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT },
        };
        const uint32_t lineVerts = m_debugLineCount;
        const uint32_t triVerts  = m_debugTriCount;
        lines.execute = [this, extent, frameIndex, lineVerts, triVerts](VkCommandBuffer c) {
            const auto* ubo = reinterpret_cast<const CameraUBO*>(m_frames[frameIndex].ubo.Mapped());
            glm::mat4 viewProj = ubo->proj * ubo->view;  // proj already Y-flipped
            VkRenderingAttachmentInfo color{};
            color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView   = m_viewportColor.View();
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;   // preserve the scene
            color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo render{};
            render.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
            render.renderArea           = { {0,0}, extent };
            render.layerCount           = 1;
            render.colorAttachmentCount = 1;
            render.pColorAttachments    = &color;
            vkCmdBeginRendering(c, &render);
            VkViewport vp{}; vp.width=(float)extent.width; vp.height=(float)extent.height; vp.minDepth=0; vp.maxDepth=1;
            vkCmdSetViewport(c, 0, 1, &vp);
            VkRect2D sc{ {0,0}, extent }; vkCmdSetScissor(c, 0, 1, &sc);
            if (lineVerts > 0) {
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debugLinePipeline);
                vkCmdPushConstants(c, m_debugLinePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(glm::mat4), &viewProj);
                VkBuffer vb = m_debugLineBuffer[frameIndex].Handle(); VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(c, 0, 1, &vb, &off);
                vkCmdDraw(c, lineVerts, 1, 0, 0);
            }
            if (triVerts > 0) {
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, m_debugTriPipeline);
                vkCmdPushConstants(c, m_debugLinePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(glm::mat4), &viewProj);
                VkBuffer tb = m_debugTriBuffer[frameIndex].Handle(); VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(c, 0, 1, &tb, &off);
                vkCmdDraw(c, triVerts, 1, 0, 0);
            }
            vkCmdEndRendering(c);
        };
        m_graph.AddPass(std::move(lines));
    }

    // ---- UI (FluentUI Vulkan backend). Editor chrome paints the entire
    //      swapchain (CLEAR), and a FluentUI::Image samples m_viewportColor
    //      to display the 3D output inside the center viewport panel. If
    //      there is no UI callback installed we still need SOMETHING to
    //      reach the swapchain — handled in the fallback BlitToSwapchain
    //      pass below.
    if (m_uiBackend && m_uiCallback) {
        GraphPass uiPass{};
        uiPass.name = "UI";
        uiPass.reads = {
            { viewportColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
        };
        // READ for the source blend factor (alpha blending samples dst).
        uiPass.writes = {
            { swapImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT },
        };
        uiPass.execute = [this, extent, imageIndex](VkCommandBuffer c) {
            VkClearValue clearBg{};
            // Vortex dark bg0 (#0a0b0d) — chrome panels cover most of this
            // but the background between them shows the editor's deep dark
            // surface, not whatever the previous frame left in the swapchain.
            // The swapchain is an _SRGB format, so the clear value is treated
            // as LINEAR and re-encoded to sRGB on store. We therefore pass the
            // linear equivalent of #0a0b0d (sRGB 0.039/0.043/0.051 -> linear)
            // so the cleared background matches the authored color instead of
            // coming out lighter.
            clearBg.color = { { 0.00302f, 0.00333f, 0.00402f, 1.0f } };

            VkRenderingAttachmentInfo color{};
            color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView   = m_swapchain.ImageView(imageIndex);
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue  = clearBg;

            VkRenderingInfo render{};
            render.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
            render.renderArea           = { {0, 0}, extent };
            render.layerCount           = 1;
            render.colorAttachmentCount = 1;
            render.pColorAttachments    = &color;
            vkCmdBeginRendering(c, &render);

            // Shared mode: hand FluentUI's backend the engine's in-flight
            // command buffer (it records UI draws onto it; no submit/present).
            // Must happen before the callback runs FluentUI::Render(), because
            // the Renderer now calls backend->BeginFrame() inside EndFrame()
            // (i.e. during Render), which records viewport/scissor onto this cmd.
            m_uiBackend->SetFrameCommandBuffer(static_cast<void*>(c));
            m_uiBackend->SetViewport(static_cast<int>(extent.width),
                                     static_cast<int>(extent.height));
            m_uiCallback();

            vkCmdEndRendering(c);
        };
        m_graph.AddPass(std::move(uiPass));
    } else {
        // No UI callback registered (headless / smoke test path) — blit the
        // viewport image straight to the swapchain so something is visible.
        GraphPass blitPass{};
        blitPass.name = "BlitToSwapchain";
        blitPass.reads = {
            { viewportColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT },
        };
        blitPass.writes = {
            { swapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT },
        };
        blitPass.execute = [this, extent, imageIndex](VkCommandBuffer c) {
            VkImageBlit region{};
            region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.srcSubresource.layerCount = 1;
            region.srcOffsets[1] = { static_cast<int32_t>(extent.width),
                                     static_cast<int32_t>(extent.height), 1 };
            region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.dstSubresource.layerCount = 1;
            region.dstOffsets[1] = region.srcOffsets[1];
            vkCmdBlitImage(c,
                m_viewportColor.Handle(),  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                m_swapchain.Image(imageIndex), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &region, VK_FILTER_NEAREST);
        };
        m_graph.AddPass(std::move(blitPass));
    }

    // ---- Present transition --------------------------------------------
    GraphPass present{};
    present.name = "Present";
    present.writes = {
        { swapImg, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
          VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0 },
    };
    present.execute = nullptr;
    m_graph.AddPass(std::move(present));

    m_graph.Execute(cmd);

    vkEndCommandBuffer(cmd);
}

bool VulkanContext::BeginFrame(SDL_Window* window,
                               const glm::mat4& view, const glm::mat4& proj,
                               const glm::vec3& cameraPos,
                               const DirectionalLight& light) {
    auto& f = m_frames[m_frameIndex];
    vkWaitForFences(m_device.Handle(), 1, &f.inFlight, VK_TRUE, UINT64_MAX);

    VkResult r = vkAcquireNextImageKHR(m_device.Handle(), m_swapchain.Handle(),
                                       UINT64_MAX, f.imageAvailable,
                                       VK_NULL_HANDLE, &m_pendingImageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain(window);
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "[Vulkan] vkAcquireNextImageKHR failed: %d\n", r);
        return false;
    }

    // Compute frame delta time (capped) for particles + any other timed work.
    auto now = std::chrono::steady_clock::now();
    if (!m_haveLastTime) {
        m_dt = 1.0f / 60.0f;
        m_haveLastTime = true;
    } else {
        m_dt = std::chrono::duration<float>(now - m_lastTime).count();
        if (m_dt > 0.1f) m_dt = 0.1f;
    }
    m_lastTime = now;

    // Clear last frame's debug lines — callers re-add theirs each frame between
    // BeginFrame and EndFrame.
    m_debugLineCount = 0;
    m_debugTriCount  = 0;

    // Re-pack the punctual-light SSBO for THIS frame from the CPU source of
    // truth. Per-frame buffer: the one we write is not in GPU use (its fence
    // was waited in BeginFrame), so editing lights live can't race.
    writeLightBuffer(m_lightBuffer[m_frameIndex]);

    updateUniformBuffer(m_frameIndex, view, proj, cameraPos, light);

    // CPU-side particle emission writes to the host-coherent SSBO; the GPU
    // update pass picks up the new slots in the same frame.
    if (m_particlesEnabled) emitParticles(m_dt);

    // Advance atlas sprite animations (no toggle — sprites with frameRate=0 have
    // no entry and cost nothing).
    updateSprites(m_dt);

    m_drawQueue.clear();
    m_frameOpen = true;
    return true;
}

void VulkanContext::SubmitDraw(uint32_t meshIndex, const glm::mat4& worldMatrix) {
    if (!m_frameOpen) return;
    if (meshIndex >= m_meshes.size()) return;
    m_drawQueue.push_back({ meshIndex, worldMatrix });
}

void VulkanContext::EndFrame(SDL_Window* window) {
    if (!m_frameOpen) return;
    m_frameOpen = false;

    auto& f = m_frames[m_frameIndex];
    vkResetFences(m_device.Handle(), 1, &f.inFlight);
    vkResetCommandBuffer(f.cmd, 0);
    recordCommandBuffer(f.cmd, m_pendingImageIndex);

    VkSemaphore renderFinished = m_renderFinishedPerImage[m_pendingImageIndex];

    VkSemaphoreSubmitInfo waitSem{};
    waitSem.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitSem.semaphore = f.imageAvailable;
    waitSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalSem{};
    signalSem.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalSem.semaphore = renderFinished;
    signalSem.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = f.cmd;

    VkSubmitInfo2 submit{};
    submit.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount   = 1;
    submit.pWaitSemaphoreInfos      = &waitSem;
    submit.commandBufferInfoCount   = 1;
    submit.pCommandBufferInfos      = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos    = &signalSem;

    if (vkQueueSubmit2(m_device.GraphicsQueue(), 1, &submit, f.inFlight) != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] vkQueueSubmit2 failed\n");
        return;
    }

    VkSwapchainKHR swap = m_swapchain.Handle();
    VkPresentInfoKHR present{};
    present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = &renderFinished;
    present.swapchainCount     = 1;
    present.pSwapchains        = &swap;
    present.pImageIndices      = &m_pendingImageIndex;

    VkResult r = vkQueuePresentKHR(m_device.PresentQueue(), &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain(window);
    } else if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[Vulkan] vkQueuePresentKHR failed: %d\n", r);
    }

    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
}

}  // namespace pokemotor::vk
