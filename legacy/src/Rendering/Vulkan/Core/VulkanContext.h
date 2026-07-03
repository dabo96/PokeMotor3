#pragma once

#include "VulkanAllocator.h"
#include "VulkanDevice.h"
#include "VulkanInstance.h"
#include "VulkanSwapchain.h"

#include "BufferVk.h"
#include "ImageVk.h"
#include "MeshVk.h"
#include "RenderGraph.h"

#include "Rendering/Vulkan/VkComponents.h"

// FluentUI render backend interface (FluentUI::RenderBackend, VulkanSharedContext).
#include "core/RenderBackend.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Window;

namespace pokemotor::vk {

// Forward declare so users can opt into the UI without paying for the full
// header include here. Actual class in Rendering/Vulkan/UI/VulkanBackend.h.
class VulkanBackend;

constexpr uint32_t kFramesInFlight     = 2;
// Bindless sampler2D array capacity. Shader declares u_textures[kMaxBindlessTextures];
// extra slots beyond loaded textures are flagged PARTIALLY_BOUND and unused.
constexpr uint32_t kMaxBindlessTextures = 256;

// Cascade shadow map config (FASE 9). Three 2048² D32 layers — frustum-fit
// improvements (variance, normal offset bias, fade) come in a later phase.
constexpr uint32_t kCascadeCount  = 3;
constexpr uint32_t kShadowMapSize = 2048;

// IBL (FASE 10). Environment cubemap + irradiance + prefiltered specular + BRDF LUT,
// all stored as 6-layer 2D arrays (no VIEW_TYPE_CUBE — manual face addressing
// in shaders). Real HDR cubemap loading replaces env_procedural.comp later.
constexpr uint32_t kEnvCubeSize        = 256;
constexpr uint32_t kEnvCubeMips        = 9;   // floor(log2(256)) + 1 — full chain for GGX prefilter mip selection
constexpr uint32_t kIrradianceSize     = 64;
constexpr uint32_t kPrefilteredSize    = 256;
constexpr uint32_t kPrefilteredMips    = 8;   // 8 levels of 256 → down to 2px; lighting.frag assumes 8
constexpr uint32_t kBrdfLutSize        = 512;

// TAA (FASE 11). Halton 2,3 jitter cycled over kHaltonSamples frames.
constexpr uint32_t kHaltonSamples      = 16;

// SSR (FASE 13). 64 octahedral direction bins on an 8×8 grid; packet trace
// workgroups are 256-wide so each workgroup processes one packet of nearby
// rays after sorting. Hi-Z mip chain depth is computed per-resolution.
constexpr uint32_t kSSRBins            = 64;
constexpr uint32_t kSSRPacketSize      = 256;

// Per frame-in-flight resources. The render-finished semaphore is intentionally
// NOT here: it lives in m_renderFinishedPerImage (indexed by swapchain image)
// to satisfy VUID-vkQueueSubmit2-semaphore-03868 — present may keep the
// signaled semaphore in use until the image is re-acquired, which can outlive
// a frame-in-flight slot.
struct FrameSync {
    VkSemaphore     imageAvailable    = VK_NULL_HANDLE;
    VkFence         inFlight          = VK_NULL_HANDLE;
    VkCommandPool   pool              = VK_NULL_HANDLE;
    VkCommandBuffer cmd               = VK_NULL_HANDLE;
    BufferVk        ubo;
    VkDescriptorSet frameDescriptorSet = VK_NULL_HANDLE;  // set=0 (UBO)
};

class VulkanContext {
public:
    bool Initialize(SDL_Window* window, bool enableValidation);
    void Shutdown();

    // Per-frame drawing API. Caller owns the camera/light, builds a frame by
    // calling BeginFrame → SubmitDraw* → EndFrame.
    //   - BeginFrame: snapshot camera/light into UBO, clear queue.
    //   - SubmitDraw(meshIndex, worldMatrix): append a draw item; material index
    //     is read from the mesh's stored material.
    //   - EndFrame: record cmd buffer + queue submit + present.
    bool BeginFrame(SDL_Window* window,
                    const glm::mat4& view, const glm::mat4& proj,
                    const glm::vec3& cameraPos,
                    const DirectionalLight& light);
    void SubmitDraw(uint32_t meshIndex, const glm::mat4& worldMatrix);
    void EndFrame(SDL_Window* window);
    void WaitIdle();

    // Scene-load support: reset the per-scene object lists so they can be
    // re-registered from a loaded file. Shared GPU resources (meshes, textures)
    // are NOT touched. Call WaitIdle() first if a frame might be in flight.
    void ClearLights();
    void ClearParticleEmitters();
    void ClearSprites();

    uint32_t MeshCount() const { return static_cast<uint32_t>(m_meshes.size()); }

    // Local AABB of mesh `i`, with its per-mesh localTransform applied (so the
    // caller only needs to apply the entity's world matrix). Returns false if
    // the index is out of range or the mesh has no AABB. The result is an AABB
    // OF the transformed corners (slightly loose for rotated localTransforms —
    // fine for click picking).
    bool MeshAABB(uint32_t i, glm::vec3& outMin, glm::vec3& outMax) const;

    // Per-triangle ray pick against mesh `i`'s retained CPU geometry. `world` is
    // the entity world matrix (mesh localTransform applied internally). Seed
    // tHit with the caller's running-nearest distance (in rayDir units); on a
    // strictly-closer hit it's updated and true is returned. Use MeshAABB as a
    // cheap broad-phase before calling this.
    bool RaycastMesh(uint32_t i, const glm::mat4& world,
                     const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                     float& tHit) const;

    float AspectRatio() const;

    // Registers a CPU-side mesh (proc. geometry, debug shapes, etc.). Uploads
    // its VBO/IBO immediately. Returns its index in the mesh table, ready to
    // hand to RenderComponentVk.firstMesh. Proc meshes have no MR texture, so
    // metallic/roughness come from these factors. Defaults are dielectric matte
    // (metallic 0, roughness 0.9) — NOT the glTF default of 1.0/1.0, which would
    // make a plain proc mesh a rough mirror that reflects the env cubemap.
    uint32_t RegisterMesh(const MeshCPU& cpu,
                          const glm::vec4& baseColor = glm::vec4(1.0f),
                          float metallic  = 0.0f,
                          float roughness = 0.9f);

    // Particle emitter spec (subset of the OpenGL CommonComponents.h variant).
    struct ParticleEmitterDesc {
        glm::vec3 position{0.0f};
        glm::vec3 direction{0.0f, 1.0f, 0.0f};
        float spread       = 0.5f;
        float minSpeed     = 1.0f;
        float maxSpeed     = 3.0f;
        float minLifetime  = 0.5f;
        float maxLifetime  = 2.0f;
        float startSize    = 0.1f;
        float endSize      = 0.0f;
        glm::vec4 startColor{1.0f};
        glm::vec3 gravity{0.0f, -9.81f, 0.0f};
        float emitRate     = 50.0f;   // particles per second
        int   maxParticles = 1000;
    };

    // Adds an emitter to the host-side emitter table. CPU-side emission
    // pushes new particles into the SSBO each frame at emitRate.
    uint32_t RegisterParticleEmitter(const ParticleEmitterDesc& desc);

    // Rewrites an existing emitter's params (e.g. from its owning entity each
    // frame). Position + spawn params update; maxParticles/baseSlot stay fixed
    // (slots are allocated at register time) and in-flight particles keep going.
    void UpdateParticleEmitter(uint32_t index, const ParticleEmitterDesc& desc);

    // Punctual lights uploaded to an SSBO read by the lighting pass (no per-light
    // shadows yet — range/cone attenuation only). Returns the light index, or
    // UINT32_MAX if the light buffer is full.
    uint32_t RegisterPointLight(const glm::vec3& position, const glm::vec3& color,
                                float intensity, float range);
    uint32_t RegisterSpotLight(const glm::vec3& position, const glm::vec3& direction,
                               const glm::vec3& color, float intensity, float range,
                               float innerDegrees, float outerDegrees);
    // Sphere area light — soft specular via representative-point (Karis), diffuse
    // from the sphere centre. radius = emitter sphere radius (bigger = softer).
    // Shares the punctual SSBO + m_pointLightsEnabled toggle.
    uint32_t RegisterAreaLight(const glm::vec3& position, const glm::vec3& color,
                               float intensity, float range, float radius);

    // Editable description of a punctual light (mirror of the GpuLight packed in
    // the SSBO). The UI reads/writes these; UpdateLight re-packs into the SSBO.
    struct LightDesc {
        int       type         = 0;     // 0 = point, 1 = spot, 2 = area sphere
        glm::vec3 position     = glm::vec3(0.0f);
        glm::vec3 color        = glm::vec3(1.0f);
        float     intensity    = 1.0f;
        float     range        = 5.0f;
        glm::vec3 direction    = glm::vec3(0.0f, -1.0f, 0.0f);  // spot
        float     innerDegrees = 18.0f;                          // spot
        float     outerDegrees = 28.0f;                          // spot
        float     radius       = 0.5f;                           // area
    };
    uint32_t LightCount() const;
    LightDesc GetLight(uint32_t index) const;
    void      UpdateLight(uint32_t index, const LightDesc& desc);

    // Queues a world-space line segment for the debug-line overlay pass. The
    // queue is cleared each frame in BeginFrame, so callers re-add their lines
    // every frame. Drawn as an overlay over m_viewportColor with no depth test.
    void AddDebugLine(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color);

    // Queues a world-space solid triangle for the debug overlay (same overlay
    // as AddDebugLine; cleared each frame in BeginFrame). Used for gizmo
    // arrowhead cones.
    void AddDebugTriangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                          const glm::vec3& color);

    // Live pointers to the render-effect toggles/params so the editor UI can
    // edit them in place. Pointers target this VulkanContext's members — valid
    // for its lifetime. Grouped by effect for the settings panel.
    struct RenderSettingsRefs {
        // Shadows
        bool*      pcssEnabled;
        float*     pcssLightSize;
        bool*      csmFrustumFit;
        bool*      spriteShadows;
        // Material AA
        bool*      specularAA;
        // Reflections (SSR)
        bool*      ssrHalfRes;
        // Bloom
        bool*      bloomEnabled;
        float*     bloomThreshold;
        float*     bloomKnee;
        float*     bloomIntensity;
        float*     bloomUpsampleIntensity;
        // Volumetric fog
        bool*      fogEnabled;
        bool*      godRays;
        bool*      fogTemporal;
        float*     fogTemporalAlpha;
        int32_t*   fogSteps;
        float*     fogDensityFloor;
        float*     fogDensityScale;
        float*     fogHeightFactor;
        float*     fogScatterStrength;
        float*     fogMaxDistance;
        glm::vec3* fogColor;
        // Tonemap / grading
        float*     exposure;
        float*     contrast;
        float*     saturation;
        bool*      celEnabled;
        float*     celSteps;
        // Auto-exposure
        bool*      autoExposureEnabled;
        float*     autoExposureKey;
        float*     autoExposureTau;
        float*     autoExposureMinLum;
        float*     autoExposureMaxLum;
        // Sharpening (CAS)
        bool*      casEnabled;
        float*     casSharpness;
        // Particles
        bool*      particlesEnabled;
        bool*      softParticles;
        bool*      particleMotionBlur;
        float*     particleStretchScale;
        // Lights
        bool*      pointLightsEnabled;
    };
    RenderSettingsRefs GetRenderSettingsRefs();

    // Sprite descriptor — minimal subset of the OpenGL SpriteComponent.
    // Texture must already live in the bindless table (load via
    // RegisterSpriteTexture). Atlas UVs default to the full image [0..1].
    struct SpriteDesc {
        glm::mat4 model{1.0f};
        glm::vec4 color{1.0f};
        glm::vec4 uvOffsetScale{0.0f, 0.0f, 1.0f, 1.0f};
        float     alphaClip      = 0.5f;
        float     metallic       = 0.0f;
        float     roughness      = 0.5f;
        uint32_t  textureIndex   = 0;   // bindless slot

        // Atlas animation (optional). frameRate <= 0 → static sprite (no anim);
        // the sprite then just uses uvOffsetScale as given. When animating, the
        // grid (sheetCols × sheetRows) + frame range drive uvOffsetScale per frame.
        uint32_t  sheetCols      = 1;
        uint32_t  sheetRows      = 1;
        uint32_t  frameStart     = 0;
        uint32_t  frameCount     = 1;
        float     frameRate      = 0.0f;  // frames/sec; 0 = static
        bool      looping        = true;
    };

    // Load a sprite texture into the bindless table — typically SRGB for
    // perceptual color. Returns the slot suitable for SpriteDesc.textureIndex.
    uint32_t RegisterSpriteTexture(const std::string& path);

    // Push a sprite into the SSBO. Returns its instance index.
    uint32_t RegisterSprite(const SpriteDesc& desc);

    // Rewrites a sprite instance's full row (model + colour + material) from a
    // desc, e.g. to reflect its owning entity each frame. Same host write path
    // as updateSprites; leaves animation-driven uvOffsetScale to updateSprites.
    void UpdateSprite(uint32_t index, const SpriteDesc& desc);

    // Per-frame UI callback. If set, called inside the "UI" pass (after
    // tonemap, before present), already inside vkCmdBeginRendering and with the
    // FluentUI backend's command buffer set. Use it to issue FluentUI::Render().
    using UIDrawCallback = std::function<void()>;
    void SetUICallback(UIDrawCallback cb) { m_uiCallback = std::move(cb); }

    // Build a VulkanSharedContext describing this engine's Vulkan handles so
    // FluentUI's own Vulkan backend can run in shared mode (dynamic rendering,
    // no swapchain of its own). Pass the result to
    // FluentUI::CreateContext(window, RenderBackendType::Vulkan, &shared).
    FluentUI::VulkanSharedContext GetUISharedContext() const;

    // Register the FluentUI backend that the UI pass drives each frame — the
    // instance FluentUI::CreateContext built, fetched via FluentUI::GetBackend().
    // Does NOT take ownership; FluentUI::DestroyContext() deletes it.
    void                     SetUIBackend(FluentUI::RenderBackend* backend) { m_uiBackend = backend; }
    FluentUI::RenderBackend* UIBackend() { return m_uiBackend; }

    // E1.B/C — offscreen viewport target that the editor center panel
    // samples via FluentUI::Image. Lives as long as the swapchain (it is
    // (re)created in createPostResources). Caller registers its view via
    // VulkanBackend::RegisterExternalTexture once at startup; the render
    // graph emits the barrier into SHADER_READ_ONLY_OPTIMAL each frame.
    const ImageVk& ViewportImage() const { return m_viewportColor; }

private:
    bool createSurface(SDL_Window* window);
    bool createDepthResources();
    bool createGBuffer();
    bool createGBufferSampler();
    bool createGBufferDescriptors();
    bool createGeometryPipeline();
    bool createLightingPipeline();
    bool createHiZResources();
    bool createHiZPipeline();
    void destroyHiZResources();
    bool createSSRResources();
    bool createSSRPipelines();
    bool createSSRDescriptors();
    void writeSSRTraceDescriptors();
    void writeSSRUpsampleDescriptors();
    void destroySSRResources();
    bool createPostResources();
    bool createTonemapPipeline();
    bool createTonemapDescriptors();
    void writeTonemapDescriptors();
    bool createCASPipeline();
    bool createCASDescriptors();
    void writeCASDescriptors();
    bool createDebugLineResources();
    bool createDebugLinePipeline();
    bool createDebugTriPipeline();
    void destroyDebugLineResources();
    bool createAutoExposureResources();
    bool createAutoExposurePipelines();
    bool createAutoExposureDescriptors();
    void writeAutoExposureDescriptors();
    bool createBloomResources();
    bool createBloomPipelines();
    bool createBloomDescriptors();
    void writeBloomDescriptors();
    void destroyBloomResources();
    bool createVolFogResources();
    bool createVolFogPipeline();
    bool createVolFogDescriptors();
    void writeVolFogDescriptors();
    void destroyVolFogResources();
    bool createParticleResources();
    bool createParticlePipelines();
    bool createParticleDescriptors();
    void destroyParticleResources();
    bool createLightResources();
    void destroyLightResources();
    bool createSpriteResources();
    bool createSpritePipeline();
    bool createSpriteShadowPipeline();
    bool createSpriteDescriptors();
    void destroySpriteResources();
    // Advances atlas sprite animations and rewrites their uvOffsetScale in the
    // mapped sprite SSBO. Called once per frame in BeginFrame.
    void updateSprites(float dt);
    void destroyPostResources();
    bool createShadowResources();
    bool createShadowPipeline();
    void destroyShadowResources();
    bool createIBLResources();
    bool createIBLPipelines();
    bool precomputeIBL();
    void destroyIBLResources();
    bool createTAAResources();
    bool createTAAPipeline();
    bool createTAADescriptors();
    void destroyTAAResources();
    void writeTAADescriptors();
    bool loadModel(const char* path);
    bool createFallbackTexture();
    bool createSampler();
    bool createDescriptors();
    bool createPerFrameResources();
    bool createPerImageSemaphores();
    void destroyDepthResources();
    void destroyGBuffer();
    void destroyPerImageSemaphores();
    bool recreateSwapchain(SDL_Window* window);
    void loadPipelineCache();
    void savePipelineCache();
    VkShaderModule loadShaderModule(const char* spvPath);
    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex);
    void updateUniformBuffer(uint32_t frameIndex,
                             const glm::mat4& view, const glm::mat4& proj,
                             const glm::vec3& cameraPos,
                             const DirectionalLight& light);
    // Loads (or returns cached index for) a 2D image file. Returns 0 on failure
    // (which is the fallback texture index).
    // Default format SRGB for baseColor. Pass VK_FORMAT_R8G8B8A8_UNORM for
    // data textures (metallicRoughness, normal map) where the bytes must
    // not be gamma-corrected on sample.
    uint32_t loadOrGetTexture(const std::string& path,
                              VkFormat format = VK_FORMAT_R8G8B8A8_SRGB);

    struct DrawItem {
        uint32_t meshIndex;
        glm::mat4 worldMatrix;
    };

    VulkanInstance   m_instance;
    VkSurfaceKHR     m_surface = VK_NULL_HANDLE;
    VulkanDevice     m_device;
    VulkanSwapchain  m_swapchain;
    VulkanAllocator  m_allocator;

    ImageVk          m_depth;

    // G-Buffer attachments — geometry pass writes, lighting pass samples.
    ImageVk          m_gPosition;  // RGBA16F: xyz pos, w metallic
    ImageVk          m_gNormal;    // RGBA16F: xyz normal, w roughness
    ImageVk          m_gAlbedo;    // RGBA8:   rgb albedo, a AO
    ImageVk          m_gEmissive;  // RGBA16F: rgb emissive, a sssStrength
    ImageVk          m_gMotion;    // RG16F:   xy motion vector
    ImageVk          m_hiZ;        // R32F mip chain: mip 0 = depth copy, mip N+1 = min reduce of mip N
    ImageVk          m_ssrResult;  // RGBA16F: raw SSR (RGB reflected color, A confidence) — FASE 13 trace output
    ImageVk          m_ssrResultHalf;  // RGBA16F half-res: trace output when half-res SSR is enabled (upsampled into m_ssrResult)
    ImageVk          m_ssrColor[2];     // SVGF denoised color, ping-pong frame to frame (history)
    ImageVk          m_ssrColorTemp;    // SVGF intra-frame scratch buffer for A-Trous iterations
    ImageVk          m_ssrMoments[2];   // SVGF moments (luma mean/sqMean/historyLen/variance), ping-pong
    ImageVk          m_shadowMap;  // D32 array (kCascadeCount layers) — CSM
    ImageVk          m_envEquirect;         // RGBA32F, equirectangular HDRI source
    ImageVk          m_envCubemap;          // RGBA16F, 6 layers, filled from the equirect HDRI
    ImageVk          m_irradianceCubemap;   // RGBA16F, 6 layers, diffuse integral of env
    ImageVk          m_prefilteredCubemap;  // RGBA16F, 6 layers, kPrefilteredMips, GGX
    ImageVk          m_brdfLut;             // RG16F, split-sum BRDF table
    ImageVk          m_sceneColor;          // RGBA16F — lighting output, input to TAA
    ImageVk          m_taaHistory[2];       // RGBA16F ping-pong history buffers
    ImageVk          m_postScratch;         // RGBA16F HDR — TAA output, tonemap input (FASE 14)
    ImageVk          m_viewportColor;       // Swap-format LDR — tonemap output, sampled by editor UI center panel (E1)
    ImageVk          m_tonemapLdr;          // Swap-format LDR — tonemap output when CAS is on, sampled by the CAS pass
    ImageVk          m_bloomDown;           // RGBA16F mip chain — prefilter + downsample target
    ImageVk          m_bloomUp;             // RGBA16F mip chain — upsample accumulation target
    std::vector<VkImageView> m_bloomDownMipViews;  // per-mip storage views
    std::vector<VkImageView> m_bloomUpMipViews;    // per-mip storage views
    std::vector<VkImageView> m_bloomDownSampleViews; // per-mip single-mip sampling views
    std::vector<VkImageView> m_bloomUpSampleViews;   // per-mip single-mip sampling views
    uint32_t                 m_bloomMipCount = 0;
    ImageVk          m_volFog;              // RGBA16F half-res — scatter + transmittance (F14.D)
    ImageVk          m_volFogHistory;       // RGBA16F half-res — previous frame's fog for temporal reprojection
    bool             m_fogHistoryInitialized = false;  // false until the first frame has filled the history

    // Per-mip array-views of the prefiltered cubemap (one storage view per mip).
    std::array<VkImageView, kPrefilteredMips> m_prefilteredMipViews{};

    // Single-mip (mip 0) storage view of the env cubemap. The env equirect→cube
    // compute writes mip 0 through this; the full mip chain (m_envCubemap.View())
    // is sampled by the GGX prefilter. A storage image view must target one mip,
    // so this can't be the same all-mip view used for sampling.
    VkImageView m_envCubemapMip0View = VK_NULL_HANDLE;

    // Per-mip storage views of the Hi-Z chain (one single-mip view per level).
    // Used as writeonly image2D targets; the multi-mip m_hiZ.View() is used for
    // sampler reads (textureLod) in the SSR trace shader.
    std::vector<VkImageView> m_hiZMipViews;
    uint32_t                 m_hiZMipCount = 0;

    // Per-cascade write views into the same shadow image (one layer each).
    std::array<VkImageView, kCascadeCount> m_shadowLayerViews{};

    VkSampler        m_gbufferSampler = VK_NULL_HANDLE;
    VkSampler        m_depthSampler   = VK_NULL_HANDLE;  // nearest+clamp, used by hiz_generate.comp
    VkSampler        m_hiZSampler     = VK_NULL_HANDLE;  // linear+clamp+nearest-mip, SSR trace textureLod
    VkSampler        m_shadowSampler  = VK_NULL_HANDLE;  // linear+compareOp LESS — hardware PCF
    VkSampler        m_shadowDepthSampler = VK_NULL_HANDLE;  // nearest+clamp, raw depth for PCSS blocker search
    VkSampler        m_iblSampler     = VK_NULL_HANDLE;  // linear+clamp, samples cubemap arrays

    VkPipelineCache       m_pipelineCache       = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout      = VK_NULL_HANDLE;
    VkPipeline            m_geometryPipeline    = VK_NULL_HANDLE;
    VkPipeline            m_lightingPipeline    = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_frameSetLayout      = VK_NULL_HANDLE;  // set=0 — UBO per-frame
    VkDescriptorSetLayout m_bindlessSetLayout   = VK_NULL_HANDLE;  // set=1 — material textures
    VkDescriptorSetLayout m_gbufferSetLayout    = VK_NULL_HANDLE;  // set=2 — G-Buffer samplers
    VkDescriptorPool      m_descriptorPool      = VK_NULL_HANDLE;
    VkDescriptorSet       m_bindlessSet         = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> m_gbufferSet = {};  // per-frame: binding 9 (SVGF SSR) is rewritten each frame, single set would race

    // Hi-Z compute (FASE 8 seed + FASE 13 downsample chain). The seed pipeline
    // copies depth into m_hiZ mip 0; the downsample pipeline min-reduces 2×2
    // from mip N to mip N+1. Both share the same {sampler2D, storage image}
    // descriptor layout, so we keep one set layout and allocate one set per
    // dispatch (seed = 1, downsample = m_hiZMipCount - 1).
    VkPipelineLayout      m_hizPipelineLayout   = VK_NULL_HANDLE;
    VkPipeline            m_hizPipeline         = VK_NULL_HANDLE;  // seed (depth → mip 0)
    VkPipeline            m_hizDownsamplePipeline = VK_NULL_HANDLE; // mip N → mip N+1
    VkDescriptorSetLayout m_hizSetLayout        = VK_NULL_HANDLE;
    VkDescriptorSet       m_hizSet              = VK_NULL_HANDLE;  // seed: depth → mip 0
    std::vector<VkDescriptorSet> m_hizDownsampleSets;               // [i] reads mip i, writes mip i+1

    // SSR (FASE 13). Three compute pipelines: ray sort (used twice — mode 0 count,
    // mode 1 scatter), prefix scan, packet trace (indirect dispatch). The sort
    // and prefix pipelines share a descriptor layout; trace has its own (different
    // descriptor types).
    VkPipelineLayout      m_ssrSortPipelineLayout    = VK_NULL_HANDLE;
    VkPipeline            m_ssrSortPipeline          = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ssrSortSetLayout         = VK_NULL_HANDLE;
    VkDescriptorSet       m_ssrSortSet               = VK_NULL_HANDLE;

    VkPipelineLayout      m_ssrPrefixPipelineLayout  = VK_NULL_HANDLE;
    VkPipeline            m_ssrPrefixPipeline        = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ssrPrefixSetLayout       = VK_NULL_HANDLE;
    VkDescriptorSet       m_ssrPrefixSet             = VK_NULL_HANDLE;

    VkPipelineLayout      m_ssrTracePipelineLayout   = VK_NULL_HANDLE;
    VkPipeline            m_ssrTracePipeline         = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ssrTraceSetLayout        = VK_NULL_HANDLE;
    VkDescriptorSet       m_ssrTraceSets[2]          = { VK_NULL_HANDLE, VK_NULL_HANDLE };  // ping-pong per TAA history slot

    // SVGF temporal + spatial (FASE 13.E). One pipeline each — direction of
    // ping-pong is encoded by binding the right descriptor set per dispatch.
    VkPipelineLayout      m_ssrTemporalPipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_ssrTemporalPipeline       = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ssrTemporalSetLayout      = VK_NULL_HANDLE;
    // Two temporal sets — one per "curSlot" direction. curSlot is the SVGF
    // ping-pong slot we'll write this frame; prevSlot = 1 - curSlot is the
    // history input. m_ssrTemporalSets[curSlot] wires the right slots.
    VkDescriptorSet       m_ssrTemporalSets[2]       = { VK_NULL_HANDLE, VK_NULL_HANDLE };

    VkPipelineLayout      m_ssrSpatialPipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_ssrSpatialPipeline       = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ssrSpatialSetLayout      = VK_NULL_HANDLE;
    // Four spatial sets — two ping-pong directions × two curSlot choices.
    // Index: [curSlot][direction]; direction 0 = read Temp / write Color[curSlot],
    //                              direction 1 = read Color[curSlot] / write Temp.
    VkDescriptorSet       m_ssrSpatialSets[2][2]     = {};

    // Half-res SSR upsample (joint-bilateral reconstruction to full-res).
    VkPipelineLayout      m_ssrUpsamplePipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_ssrUpsamplePipeline       = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ssrUpsampleSetLayout      = VK_NULL_HANDLE;
    VkDescriptorSet       m_ssrUpsampleSet            = VK_NULL_HANDLE;
    // Half-res SSR trace + joint-bilateral upsample (~4× fewer rays traced).
    // OFF by default: the ¼ ray budget left visible salt-and-pepper noise in the
    // carts' brushed-metal reflections that the SVGF denoiser couldn't fully
    // clean, especially against the higher-contrast real HDRI. Full-res trace is
    // 4× the samples → much cleaner. Flip to true to trade quality for perf.
    bool                  m_ssrHalfResEnabled         = false;
    // Tracks the half-res toggle across frames: when it flips, the render graph's
    // persisted layout for m_ssrResult/m_ssrResultHalf goes stale (different passes
    // touch them), so they must be ForgetImage'd that frame. Init to the toggle's
    // default so it doesn't fire spuriously on frame 0.
    bool                  m_ssrHalfResPrev            = false;

    // SVGF history validity counter — set false after recreateSwapchain so
    // the temporal pass uses the first frame's seed moments.
    uint32_t              m_svgfFrameCounter         = 0;

    // Tonemap (FASE 14.A + composites bloom from F14.B + grading from F14.C).
    // Fullscreen pass: reads m_postScratch (HDR) + bloom mip 0 (HDR) → writes
    // swapchain (sRGB-aware). ACES filmic + exposure/contrast/saturation.
    VkPipelineLayout      m_tonemapPipelineLayout    = VK_NULL_HANDLE;
    VkPipeline            m_tonemapPipeline          = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_tonemapSetLayout         = VK_NULL_HANDLE;
    VkDescriptorSet       m_tonemapSet               = VK_NULL_HANDLE;

    // CAS (AMD Contrast Adaptive Sharpening) — dedicated fullscreen pass after
    // tonemap. Reads m_tonemapLdr (sRGB-linearised), sharpens, writes
    // m_viewportColor. Replaces the unsharp mask that lived inside tonemap.frag.
    VkPipelineLayout      m_casPipelineLayout        = VK_NULL_HANDLE;
    VkPipeline            m_casPipeline              = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_casSetLayout             = VK_NULL_HANDLE;
    VkDescriptorSet       m_casSet                   = VK_NULL_HANDLE;

    // Debug-line overlay — line-list pipeline drawn over m_viewportColor after
    // CAS and before the UI pass, with no depth test (always visible). The
    // host-coherent vertex buffer is refilled each frame via AddDebugLine().
    // ONE buffer PER frame-in-flight: the CPU refills frame N's buffer while the
    // GPU may still be reading frame N-1's, so a single shared buffer would race.
    std::array<BufferVk, kFramesInFlight> m_debugLineBuffer;
    uint32_t              m_debugLineCapacity        = 0;          // max vertices
    uint32_t              m_debugLineCount           = 0;          // vertices used this frame
    VkPipeline            m_debugLinePipeline        = VK_NULL_HANDLE;
    VkPipelineLayout      m_debugLinePipelineLayout  = VK_NULL_HANDLE;

    // Debug triangle overlay — solid (TRIANGLE_LIST) primitives drawn in the
    // same DebugLines pass, reusing m_debugLinePipelineLayout and the
    // debug_line shaders. Refilled each frame via AddDebugTriangle(). Per-frame
    // for the same race reason as m_debugLineBuffer.
    std::array<BufferVk, kFramesInFlight> m_debugTriBuffer;
    uint32_t   m_debugTriCapacity = 0;
    uint32_t   m_debugTriCount    = 0;
    VkPipeline m_debugTriPipeline = VK_NULL_HANDLE;   // reuses m_debugLinePipelineLayout

    // Auto-exposure (histogram-based, Tardif/Lottes style). Two compute passes
    // build a 256-bin log-luminance histogram of the HDR scene and reduce it to
    // a temporally-adapted average luminance. The tonemap reads m_exposureBuffer
    // (a single float) and scales by exposureKey / avgLum. Both SSBOs persist
    // across frames and are resolution-independent (never recreated on resize).
    BufferVk              m_histogramBuffer;          // 256 uint bins
    BufferVk              m_exposureBuffer;           // 1 float adapted avg luminance

    VkPipelineLayout      m_histBuildPipelineLayout  = VK_NULL_HANDLE;
    VkPipeline            m_histBuildPipeline        = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_histBuildSetLayout       = VK_NULL_HANDLE;
    VkDescriptorSet       m_histBuildSet             = VK_NULL_HANDLE;

    VkPipelineLayout      m_histAvgPipelineLayout    = VK_NULL_HANDLE;
    VkPipeline            m_histAvgPipeline          = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_histAvgSetLayout         = VK_NULL_HANDLE;
    VkDescriptorSet       m_histAvgSet               = VK_NULL_HANDLE;

    // Bloom (FASE 14.B). Three compute pipelines, all share neither set layout
    // nor pipeline layout — each has distinct binding shapes.
    VkPipelineLayout      m_bloomPrefilterPipelineLayout  = VK_NULL_HANDLE;
    VkPipeline            m_bloomPrefilterPipeline        = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_bloomPrefilterSetLayout       = VK_NULL_HANDLE;
    VkDescriptorSet       m_bloomPrefilterSet             = VK_NULL_HANDLE;

    VkPipelineLayout      m_bloomDownsamplePipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_bloomDownsamplePipeline       = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_bloomDownsampleSetLayout      = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_bloomDownsampleSets;   // [i] reads down[i] writes down[i+1]

    VkPipelineLayout      m_bloomUpsamplePipelineLayout   = VK_NULL_HANDLE;
    VkPipeline            m_bloomUpsamplePipeline         = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_bloomUpsampleSetLayout        = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_bloomUpsampleSets;     // [i] reads down/up[i+1]+down[i] writes up[i]

    // Post-process feature toggles (F14). Hardcoded defaults until F16
    // FluentUI brings real toggles. Cast to float for the push constant.
    // E1 debug: all toggleable post-process OFF by default so the user can
    // isolate the angle-dependent wash-out (re-enable one effect at a time).
    bool                  m_bloomEnabled                  = false;
    float                 m_bloomThreshold                = 1.0f;
    float                 m_bloomKnee                     = 0.5f;
    float                 m_bloomIntensity                = 0.5f;
    float                 m_bloomUpsampleIntensity        = 0.6f;
    float                 m_exposure                      = 0.0f;
    float                 m_contrast                      = 1.0f;
    float                 m_saturation                    = 1.0f;
    bool                  m_fogEnabled                    = false;  // off by default (validated temporal reprojection 2026-06-13)
    bool                  m_godRaysEnabled                = true;   // fog receives CSM shadows
    bool                  m_fogTemporalEnabled            = true;   // accumulate fog across frames (reproject + blend)
    float                 m_fogTemporalAlpha              = 0.9f;   // history weight when temporal blending
    int32_t               m_fogSteps                      = 24;
    float                 m_fogDensityFloor               = 0.005f;
    float                 m_fogDensityScale               = 0.04f;
    float                 m_fogHeightFactor               = 0.15f;
    float                 m_fogScatterStrength            = 1.5f;
    float                 m_fogMaxDistance                = 80.0f;
    glm::vec3             m_fogColor                      = glm::vec3(0.55f, 0.62f, 0.72f);
    bool                  m_celEnabled                    = false;
    float                 m_celSteps                      = 4.0f;
    // CAS sharpening: dedicated post-tonemap pass. When off, no sharpening runs.
    bool                  m_casEnabled                    = true;
    float                 m_casSharpness                  = 0.5f;   // 0 = soft, 1 = sharp

    // Auto-exposure tuning. When enabled, the tonemap scales by
    // m_autoExposureKey / adaptedAvgLuminance (target middle-gray), with the
    // average eased over m_autoExposureTau seconds for eye-adaptation. The
    // histogram spans [log2 minLum .. log2 maxLum] and the result is clamped to
    // [m_autoExposureMinLum .. m_autoExposureMaxLum].
    // OFF by default: with the real HDRI the scene luminance stays high and the
    // auto-exposure crushes the image to a constant dark (and the carts' metal
    // reads muddy). Fixed manual exposure (m_exposure) is predictable. Flip to
    // true to re-enable adaptive exposure.
    bool                  m_autoExposureEnabled           = false;
    float                 m_autoExposureKey               = 0.18f;   // target middle-gray
    float                 m_autoExposureTau               = 1.1f;    // adaptation seconds
    float                 m_autoExposureMinLum            = 0.02f;
    float                 m_autoExposureMaxLum            = 8.0f;
    float                 m_autoExposureLogMin            = -8.0f;
    float                 m_autoExposureLogMax            = 4.0f;

    // Volumetric fog (F14.D) — compute pipeline + descriptor set + storage view.
    VkPipelineLayout      m_volFogPipelineLayout          = VK_NULL_HANDLE;
    VkPipeline            m_volFogPipeline                = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_volFogSetLayout               = VK_NULL_HANDLE;
    VkDescriptorSet       m_volFogSet                     = VK_NULL_HANDLE;

    // Particles (F15.A) — SSBO + compute physics + billboard graphics.
    BufferVk              m_particleBuffer;                // SSBO of Particle
    uint32_t              m_particleCapacity              = 0;
    VkPipelineLayout      m_particleUpdatePipelineLayout  = VK_NULL_HANDLE;
    VkPipeline            m_particleUpdatePipeline        = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_particleSsboSetLayout         = VK_NULL_HANDLE;
    VkDescriptorSet       m_particleSsboSet               = VK_NULL_HANDLE;
    VkPipelineLayout      m_particleRenderPipelineLayout  = VK_NULL_HANDLE;
    VkPipeline            m_particleRenderPipeline        = VK_NULL_HANDLE;
    bool                  m_particlesEnabled              = true;
    bool                  m_softParticlesEnabled          = true;  // depth-fade vs scene geometry
    // Motion blur: stretch each billboard along its screen-projected velocity so
    // fast particles read as speed streaks. stretchScale = world units of stretch
    // per unit speed (capped at 4× the base size in the shader).
    bool                  m_particleMotionBlurEnabled     = true;
    float                 m_particleStretchScale          = 0.04f;

    // Punctual lights — SSBO {uvec4 header; GpuLight[]} read by lighting.frag
    // (set 2, binding 10). Host-coherent; written on RegisterPoint/SpotLight.
    struct GpuLight {
        glm::vec4 posRange;        // xyz position, w range
        glm::vec4 colorIntensity;  // rgb color, w intensity
        glm::vec4 dirType;         // xyz spot direction, w type (0=point,1=spot)
        glm::vec4 spotCos;         // x cos(inner), y cos(outer)
    };
    std::array<BufferVk, kFramesInFlight> m_lightBuffer;  // per-frame: CPU re-packs each frame from m_lightDescs, single buffer would race
    uint32_t              m_lightCapacity                 = 0;
    uint32_t              m_lightCount                    = 0;
    bool                  m_pointLightsEnabled            = true;
    // CPU-side mirror of the punctual lights, same index as the SSBO slots.
    // Editable from the UI through GetLight/UpdateLight.
    std::vector<LightDesc> m_lightDescs;
    GpuLight packLight(const LightDesc& desc) const;        // LightDesc → GpuLight
    void     writeLightBuffer(BufferVk& buf) const;         // header + all lights from m_lightDescs

    // PCSS soft shadows (Lote C+) — blocker search + variable-radius PCF over the
    // CSM. Driven through cascadeSplits.w in the frame UBO: 0 disables PCSS and
    // falls back to the fixed 3×3 PCF. m_pcssLightSize is the light's angular
    // size in shadow-UV units; bigger = softer/wider penumbras.
    bool                  m_pcssEnabled                   = true;
    float                 m_pcssLightSize                 = 0.02f;

    // CSM frustum-fit: fit each cascade's ortho to the camera sub-frustum (bounding
    // sphere for constant size + texel snapping for stability) instead of a fixed
    // origin-centred box — much better shadow-map resolution use. Off = legacy box.
    bool                  m_csmFrustumFitEnabled          = true;

    // Geometric specular antialiasing (Tokuyoshi & Kaplanyan 2019) — the geometry
    // pass widens roughness by the screen-space normal variance to kill specular
    // shimmer on high-curvature / minified surfaces. Driven via PushConstants.specularAA.
    bool                  m_specularAAEnabled             = true;

    // Sprites (F15.B) — quad mesh + SpriteInstance SSBO + G-Buffer pipeline.
    BufferVk              m_spriteQuadVertices;
    BufferVk              m_spriteQuadIndices;
    BufferVk              m_spriteInstanceBuffer;          // SSBO of SpriteInstance
    uint32_t              m_spriteCapacity                = 0;
    uint32_t              m_spriteCount                   = 0;
    VkDescriptorSetLayout m_spriteSsboSetLayout           = VK_NULL_HANDLE;
    VkDescriptorSet       m_spriteSsboSet                 = VK_NULL_HANDLE;
    VkPipelineLayout      m_spritePipelineLayout          = VK_NULL_HANDLE;
    VkPipeline            m_spritePipeline                = VK_NULL_HANDLE;
    // Alpha-tested billboard shadow casting into the CSM (reuses bindless +
    // sprite SSBO sets; depth-only). Toggle is intrinsic to sprites.
    VkPipelineLayout      m_spriteShadowPipelineLayout    = VK_NULL_HANDLE;
    VkPipeline            m_spriteShadowPipeline          = VK_NULL_HANDLE;
    bool                  m_spriteShadowsEnabled          = true;

    // CPU-side atlas animation state, parallel to the sprite SSBO. Only sprites
    // registered with frameRate>0 get an entry; updateSprites() advances them
    // and rewrites their uvOffsetScale in the mapped SSBO each frame.
    struct SpriteAnim {
        uint32_t spriteIndex;
        uint32_t sheetCols, sheetRows;
        uint32_t frameStart, frameCount;
        float    frameRate;
        bool     looping;
        float    time;
    };
    std::vector<SpriteAnim> m_spriteAnims;

    // SSBOs for ray sort + indirect dispatch. All device-local; reset to zero
    // at the start of each SSR run via vkCmdFillBuffer.
    BufferVk              m_ssrRayCount;          // 1 uint
    BufferVk              m_ssrBinCounters;       // kSSRBins uints
    BufferVk              m_ssrBinOffsets;        // kSSRBins uints
    BufferVk              m_ssrScatterCounters;   // kSSRBins uints
    BufferVk              m_ssrSortedRays;        // width*height uints (worst case)
    BufferVk              m_ssrIndirectArgs;      // 3 uints (numGroupsXYZ)

    // Shadow CSM (FASE 9). Independent layout — push constant is large enough
    // (mat4 lightSpace + mat4 model) that we want a dedicated 128-byte range.
    VkPipelineLayout      m_shadowPipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_shadowPipeline       = VK_NULL_HANDLE;

    // IBL precompute (FASE 10.1). Runs once at init time, never per-frame.
    VkPipelineLayout      m_envPipelineLayout         = VK_NULL_HANDLE;
    VkPipeline            m_envPipeline               = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_envSetLayout              = VK_NULL_HANDLE;
    VkDescriptorSet       m_envSet                    = VK_NULL_HANDLE;
    VkPipelineLayout      m_irradiancePipelineLayout  = VK_NULL_HANDLE;
    VkPipeline            m_irradiancePipeline        = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_irradianceSetLayout       = VK_NULL_HANDLE;
    VkDescriptorSet       m_irradianceSet             = VK_NULL_HANDLE;

    VkPipelineLayout      m_prefilterPipelineLayout   = VK_NULL_HANDLE;
    VkPipeline            m_prefilterPipeline         = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_prefilterSetLayout        = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kPrefilteredMips> m_prefilterSets{};

    VkPipelineLayout      m_brdfLutPipelineLayout     = VK_NULL_HANDLE;
    VkPipeline            m_brdfLutPipeline           = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_brdfLutSetLayout          = VK_NULL_HANDLE;
    VkDescriptorSet       m_brdfLutSet                = VK_NULL_HANDLE;

    // TAA (FASE 11). Two descriptor sets — one per "current history" direction.
    VkPipelineLayout      m_taaPipelineLayout         = VK_NULL_HANDLE;
    VkPipeline            m_taaPipeline               = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_taaSetLayout              = VK_NULL_HANDLE;
    VkDescriptorSet       m_taaSets[2]                = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkSampler             m_taaSampler                = VK_NULL_HANDLE;
    uint32_t              m_taaFrameCounter           = 0;
    glm::mat4             m_prevViewProj              = glm::mat4(1.0f);
    glm::vec2             m_prevJitter                = glm::vec2(0.0f);

    std::vector<MeshVk> m_meshes;

    // Particle emitters live entirely host-side; CPU emits new particles per
    // frame and writes them into a staging region of m_particleBuffer (host-
    // visible). Compute updates and graphics reads then run on GPU.
    struct ParticleEmitterState {
        ParticleEmitterDesc desc;
        float               accumulator = 0.0f;
        uint32_t            spawnCursor = 0;  // next slot to write inside the emitter's range
        uint32_t            baseSlot    = 0;  // first slot in m_particleBuffer
    };
    std::vector<ParticleEmitterState> m_particleEmitters;
    void emitParticles(float dt);  // CPU emission tick

    // Texture cache: index 0 is the fallback (awesomeface). Each texture's
    // index is written into the bindless set at the matching dstArrayElement.
    std::vector<ImageVk>                      m_textures;
    std::unordered_map<std::string, uint32_t> m_texturePathToIndex;

    VkSampler m_sampler = VK_NULL_HANDLE;

    std::array<FrameSync, kFramesInFlight> m_frames{};
    std::vector<VkSemaphore> m_renderFinishedPerImage;
    uint32_t m_frameIndex = 0;

    // Frame state set by BeginFrame, drained in EndFrame.
    std::vector<DrawItem> m_drawQueue;
    RenderGraph           m_graph;
    uint32_t              m_pendingImageIndex = 0;
    bool                  m_frameOpen         = false;

    std::string m_pipelineCachePath = "pipeline_cache.bin";

    // FluentUI backend (F16). Lifetime owned by FluentUI::Context (which
    // deletes it in DestroyContext). We just keep a non-owning pointer so the
    // render-graph UI pass can hand it the frame command buffer.
    FluentUI::RenderBackend* m_uiBackend = nullptr;
    UIDrawCallback m_uiCallback;

    // Per-frame time delta used by particles (CPU emit + GPU update). Computed
    // in BeginFrame from a steady_clock pair. Capped to 0.1 s so a window
    // freeze doesn't dump thousands of particles on the next tick.
    float m_dt = 0.0f;
    bool  m_haveLastTime = false;
    std::chrono::steady_clock::time_point m_lastTime;
};

}  // namespace pokemotor::vk
