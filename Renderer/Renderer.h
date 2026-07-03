// Renderer/Renderer.h — shell del renderer + pass principal forward (Fase 5a).
// Sync (frames in flight) + grafo + depth buffer + pipeline forward que ilumina
// la geometría con el sol. Separado del VulkanContext (recursos).
#pragma once

#include "Core/Math.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/Lighting/Light.h"
#include "Renderer/Vulkan/BufferVk.h"
#include "Renderer/Vulkan/ImageVk.h"
#include "Renderer/Vulkan/MeshVk.h"
#include "Renderer/Vulkan/Texture.h"
#include "Renderer/2D/Sprite.h"
#include "Renderer/2D/SpriteBatch.h"
#include "Renderer/2D/TextBatch.h"
#include "Renderer/Text/MsdfFont.h"
#include "Scene/Components.h"          // DrawItem

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pk {

class VulkanContext;
class AssetManager;

// Datos por frame para el set 0 (cámara + sol + ambiente). Debe coincidir con el
// uniform Camera de mesh.vert/.frag.
struct alignas(16) CameraUBO {
    Mat4 viewProj{ 1.0f };
    Vec4 camPos{ 0, 0, 0, 1 };
    Vec4 camRight{ 1, 0, 0, 0 };   // ejes de cámara en mundo (para billboards HD-2D)
    Vec4 camUp{ 0, 1, 0, 0 };
    Vec4 sunDir{ -0.4f, -1.0f, -0.3f, 0 };
    Vec4 sunColor{ 1.0f, 0.97f, 0.9f, 0 };
    Vec4 ambient{ 0.12f, 0.13f, 0.16f, 0 };
    Mat4 lightViewProj{ 1.0f };   // espacio de luz del sol (shadow map)
};

// Luz en formato GPU (std430). 64 bytes, debe coincidir con mesh.frag.
struct alignas(16) GpuLight {
    Vec4 posType;   // xyz posición, w = tipo (0 point, 1 spot)
    Vec4 dirRange;  // xyz dirección (spot), w = range
    Vec4 color;     // rgb = color * intensidad
    Vec4 spot;      // x = cos(inner), y = cos(outer)
};

// Parámetros de post-proceso (editables en vivo desde el editor por referencia).
struct PostSettings {
    float exposure       = 0.0f;   // EV
    float bloomThreshold = 1.0f;
    float bloomKnee      = 0.6f;
    float bloomIntensity = 0.5f;
    float dofFocusCenter = 0.5f;   // tilt-shift: banda nítida en el centro
    float dofFocusRange  = 0.18f;
    float dofMaxRadius   = 0.006f;
};

// Modo de render: el forward 3D / HD-2D (escena + post) o el camino 2D clásico
// (SpriteBatch ortográfico → target lowRes → upscale nearest). Cada modo es un
// grafo distinto; el motor solo cambia el flag.
enum class RenderMode { Forward3D, Sprite2D };

class Renderer {
public:
    bool init(VulkanContext* ctx, AssetManager* assets);
    void shutdown();

    void setRenderMode(RenderMode m) { m_mode = m; }

    // --- Camino 2D clásico ---
    void setSprites(const std::vector<Sprite>& sprites) { m_sprites2D = sprites; }
    // Añade más sprites a los del frame (p.ej. el render-feed del ECS, encima de los
    // tiles/jugador que pone el modo). El SpriteBatch los ordena por capa.
    void appendSprites(const std::vector<Sprite>& sprites) {
        m_sprites2D.insert(m_sprites2D.end(), sprites.begin(), sprites.end());
    }
    void set2DCamera(const Mat4& viewProj) { m_ortho2D = viewProj; }
    // Camino HD: los sprites Smooth (+ UI + texto) se dibujan a resolución completa y se
    // componen sobre el lowRes (tiles Pixel). false = todo por el lowRes (look retro puro).
    void setHD2D(bool enabled) { m_hd2D = enabled; }
    bool hd2D() const { return m_hd2D; }

    // Sprites en espacio de PANTALLA (UI del juego): como setSprites pero la posición y
    // el tamaño van en píxeles del target lowRes (no en mundo). Se dibujan SOBRE los
    // sprites de mundo y BAJO el texto. Los emite el UIRenderer (UI/), screen-space.
    void setUISprites(const std::vector<Sprite>& sprites) { m_uiSprites2D = sprites; }

    // Texto de UI en espacio de pantalla (píxeles del target lowRes, +Y abajo). Se
    // dibuja sobre los sprites del mismo frame. uiFont() permite medir/centrar.
    void setTexts(const std::vector<TextItem>& texts) { m_texts2D = texts; }
    const MsdfFont& uiFont() const { return m_uiFont; }

    // El Engine rellena estos cada frame antes de drawFrame().
    void setCamera(const Mat4& viewProj, const Vec3& camPos,
                   const Vec3& camRight, const Vec3& camUp);
    void setSun(const Vec3& dir, const Vec3& color, const Vec3& ambient);
    void setLights(const std::vector<Light>& lights);          // puntuales / spot
    void setDrawItems(const std::vector<DrawItem>& items) { m_drawItems = items; }

    // Sprite HD-2D (billboard iluminado): textura del AssetManager + pos/tamaño.
    void setSprite(TextureHandle tex, const Vec3& pos, const Vec2& size);

    PostSettings& postSettings() { return m_post; }   // edición en vivo (editor)

    // Callback opcional de UI (editor): se invoca como un pass del grafo, después
    // del tonemap, sobre la swapchain (cmd, view, extent). Vacío = sin UI.
    void setUICallback(std::function<void(VkCommandBuffer, VkImageView, VkExtent2D)> cb) {
        m_uiCallback = std::move(cb);
    }

    void drawFrame();
    void onResize() { m_swapchainDirty = true; }

private:
    bool createCommandResources();
    bool createSyncObjects();
    void destroySyncObjects();
    bool createPerImageSemaphores();
    void destroyPerImageSemaphores();
    bool createDepth();
    void destroyDepth();
    bool createShadow();
    void destroyShadow();
    bool createSceneDescriptors();
    bool createMaterialResources();
    VkDescriptorSet allocMaterialSet(VkImageView albedoView, VkSampler sampler);
    VkDescriptorSet materialSetFor(VkImageView albedoView);   // cache por image view
    bool createForwardPipeline();
    bool createShadowPipeline();
    bool createSpritePipeline();
    bool createSceneColor();
    void destroySceneColor();
    bool createBloomTargets();
    void destroyBloomTargets();
    bool createPostResources();   // sampler + layouts + pool + sets
    void updatePostSets();        // (re)escribe sets cuando cambian los targets
    bool createFullscreenPipeline(const char* fragSpv, VkDescriptorSetLayout setLayout,
                                  uint32_t pushSize, VkFormat colorFormat,
                                  VkPipelineLayout& outLayout, VkPipeline& outPipeline,
                                  bool blendPremult = false);
    bool createPostPipelines();   // prefilter + blur + tonemap
    void computeSunShadowMatrix();
    bool recreateSwapchain();
    void recordCommands(VkCommandBuffer cmd, uint32_t imageIndex);
    VkShaderModule loadShader(const char* spvPath);

    // --- Camino 2D clásico ---
    bool create2DResources();   // sampler nearest + layout/pool + lowRes + vbo
    bool create2DPipelines();   // sprite2d + upscale + text2d
    void destroy2D();
    bool createHDColor();        // target full-res del camino Smooth (se recrea con el swapchain)
    void destroyHDColor();
    void updateHDComposeSets();  // (re)escribe los sets de composición a los hdColor vigentes
    VkDescriptorSet tex2DSetFor(VkImageView view, FilterMode filter);  // cache por view; sampler según filter
    VkDescriptorSet textSetFor(VkImageView view);     // cache por view (sampler linear, MSDF)
    void recordSprite2D(GraphResource swap, VkExtent2D extent);
    // Helpers de dibujo 2D (asumen un rendering ya iniciado). filterMask: bit0=Pixel, bit1=Smooth.
    void drawWorldRuns(VkCommandBuffer cmd, VkBuffer vb, uint32_t vbVerts,
                       VkImageView whiteView, unsigned filterMask);
    void drawUIRuns(VkCommandBuffer cmd, VkBuffer uvb, uint32_t uvbVerts, VkImageView whiteView);
    void drawTextRun(VkCommandBuffer cmd, VkBuffer tvb, uint32_t tvbVerts, VkImageView atlasView);

    VulkanContext* m_ctx    = nullptr;
    AssetManager*  m_assets = nullptr;
    std::vector<DrawItem> m_drawItems;   // lista a dibujar (de Scene::renderables)
    static constexpr uint32_t kFramesInFlight = 2;

    VkCommandPool                m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<VkSemaphore>     m_imageAvailable;
    std::vector<VkFence>         m_inFlight;
    std::vector<VkSemaphore>     m_renderFinished;
    uint32_t                     m_currentFrame  = 0;
    bool                         m_swapchainDirty = false;

    std::vector<ImageVk> m_depth;   // uno por frame-in-flight (evita carrera entre frames)

    static constexpr uint32_t kMaxLights = 32;

    VkDescriptorSetLayout        m_cameraSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool             m_descriptorPool  = VK_NULL_HANDLE;
    std::vector<BufferVk>        m_cameraUBO;   // [kFramesInFlight]
    std::vector<BufferVk>        m_lightSSBO;   // [kFramesInFlight]
    std::vector<VkDescriptorSet> m_cameraSet;   // [kFramesInFlight]
    std::vector<GpuLight>        m_cpuLights;
    uint32_t                     m_lightCount = 0;

    VkPipelineLayout m_forwardLayout   = VK_NULL_HANDLE;
    VkPipeline       m_forwardPipeline = VK_NULL_HANDLE;

    // Shadow map del sol (uno por frame-in-flight; tamaño fijo, no depende del swapchain).
    static constexpr uint32_t kShadowSize = 2048;
    std::vector<ImageVk> m_shadow;
    VkSampler            m_shadowSampler   = VK_NULL_HANDLE;
    VkPipelineLayout     m_shadowLayout    = VK_NULL_HANDLE;
    VkPipeline           m_shadowPipeline  = VK_NULL_HANDLE;

    // Materiales (set 1): un descriptor set por textura de albedo (cacheado por view).
    static constexpr uint32_t kMaxMaterials = 128;
    VkDescriptorSetLayout m_materialSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_materialPool      = VK_NULL_HANDLE;
    VkSampler             m_materialSampler   = VK_NULL_HANDLE;
    std::unordered_map<VkImageView, VkDescriptorSet> m_materialSetCache;

    // Sprite HD-2D (billboard iluminado, Fase 5e).
    VkPipelineLayout m_spriteLayout   = VK_NULL_HANDLE;
    VkPipeline       m_spritePipeline = VK_NULL_HANDLE;
    VkSampler        m_spriteSampler  = VK_NULL_HANDLE;   // NEAREST (pixel-art)
    TextureHandle    m_spriteTex;                          // del AssetManager
    VkDescriptorSet  m_spriteSet      = VK_NULL_HANDLE;
    bool             m_hasSprite      = false;
    Vec3             m_spritePos{ 0.0f };
    Vec2             m_spriteSize{ 1.0f };
    float            m_spriteAlphaClip = 0.5f;

    // Post-proceso (Fase 6): escena → HDR → DoF + bloom → tonemap → swapchain.
    std::vector<ImageVk> m_sceneColor;   // HDR (RGBA16F) full-res, uno por frame
    std::vector<ImageVk> m_dofColor;     // HDR full-res tras DoF, uno por frame
    std::vector<ImageVk> m_bloomTex;     // half-res, uno por frame
    std::vector<ImageVk> m_bloomTemp;    // half-res, uno por frame
    VkExtent2D           m_bloomExtent{ 0, 0 };
    VkSampler            m_postSampler = VK_NULL_HANDLE;  // linear clamp

    VkDescriptorSetLayout m_postSetLayout    = VK_NULL_HANDLE;  // 1 sampler (prefilter/blur)
    VkDescriptorSetLayout m_tonemapSetLayout = VK_NULL_HANDLE;  // 2 samplers (hdr + bloom)
    VkDescriptorPool      m_postPool         = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_dofSet;        // entrada = scene
    std::vector<VkDescriptorSet> m_prefilterSet;  // entrada = scene
    std::vector<VkDescriptorSet> m_blurHSet;      // entrada = bloomTex
    std::vector<VkDescriptorSet> m_blurVSet;      // entrada = bloomTemp
    std::vector<VkDescriptorSet> m_tonemapSet;    // hdr = dofColor, bloom = bloomTex

    VkPipelineLayout m_dofLayout              = VK_NULL_HANDLE;
    VkPipeline       m_dofPipeline            = VK_NULL_HANDLE;
    VkPipelineLayout m_bloomPrefilterLayout   = VK_NULL_HANDLE;
    VkPipeline       m_bloomPrefilterPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_bloomBlurLayout        = VK_NULL_HANDLE;
    VkPipeline       m_bloomBlurPipeline      = VK_NULL_HANDLE;
    VkPipelineLayout m_tonemapLayout          = VK_NULL_HANDLE;
    VkPipeline       m_tonemapPipeline        = VK_NULL_HANDLE;

    PostSettings m_post;

    std::function<void(VkCommandBuffer, VkImageView, VkExtent2D)> m_uiCallback;

    // --- Camino 2D clásico (Plan2D, Paso 1) ---
    RenderMode            m_mode = RenderMode::Forward3D;
    std::vector<Sprite>   m_sprites2D;
    std::vector<Sprite>   m_uiSprites2D;   // sprites screen-space (UI del juego), proyectados con m_uiProj
    Mat4                  m_ortho2D{ 1.0f };
    SpriteBatch           m_batch;
    SpriteBatch           m_uiBatch;       // batch aparte para los sprites de UI (screen-space)
    static constexpr uint32_t kLowResW = 480;   // resolución interna 16:9
    static constexpr uint32_t kLowResH = 270;
    std::vector<ImageVk>  m_lowRes;             // [kFramesInFlight] target lowRes
    VkSampler             m_sampler2D = VK_NULL_HANDLE;   // NEAREST clamp (FilterMode::Pixel)
    VkSampler             m_smoothSampler = VK_NULL_HANDLE;  // LINEAR + mips trilinear (FilterMode::Smooth)
    VkDescriptorSetLayout m_set2DLayout = VK_NULL_HANDLE; // 1 combined sampler (frag)
    VkDescriptorPool      m_pool2D      = VK_NULL_HANDLE;
    std::unordered_map<VkImageView, VkDescriptorSet> m_tex2DCache;
    VkPipelineLayout      m_sprite2DLayout   = VK_NULL_HANDLE;
    VkPipeline            m_sprite2DPipeline = VK_NULL_HANDLE;
    VkPipelineLayout      m_upscaleLayout    = VK_NULL_HANDLE;
    VkPipeline            m_upscalePipeline  = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_upscaleSet;   // [kFramesInFlight] → lowRes[i]
    // Camino HD (dos targets): los sprites Smooth + UI + texto se dibujan a resolución
    // completa (m_hdColor) y se componen ENCIMA del upscale del lowRes (tiles Pixel). El
    // lowRes es fijo (480×270); m_hdColor se recrea con el swapchain (como m_sceneColor).
    bool                  m_hd2D = true;          // dos targets activados (Smooth full-res)
    std::vector<ImageVk>  m_hdColor;              // [kFramesInFlight] target full-res
    VkPipelineLayout      m_composeHDLayout   = VK_NULL_HANDLE;
    VkPipeline            m_composeHDPipeline = VK_NULL_HANDLE;   // upscale.frag + blend premult
    std::vector<VkDescriptorSet> m_hdComposeSet; // [kFramesInFlight] → hdColor[i] (sampler smooth)
    std::vector<BufferVk>        m_spriteVB;     // [kFramesInFlight] vértices dinámicos
    std::vector<VkDeviceSize>    m_spriteVBCap;  // [kFramesInFlight] capacidad en bytes
    std::vector<BufferVk>        m_uiSpriteVB;   // [kFramesInFlight] vértices de sprites UI
    std::vector<VkDeviceSize>    m_uiSpriteVBCap;

    // Texto de UI (MSDF): fuente compartida + pipeline propio + sampler LINEAR (el MSDF
    // necesita interpolación) y su buffer dinámico por frame. Reusa m_set2DLayout/m_pool2D.
    MsdfFont              m_uiFont;
    std::vector<TextItem> m_texts2D;
    TextBatch             m_textBatch;
    Mat4                  m_uiProj{ 1.0f };   // ortográfica de pantalla (lowRes, +Y abajo)
    VkSampler             m_textSampler = VK_NULL_HANDLE;   // LINEAR clamp
    std::unordered_map<VkImageView, VkDescriptorSet> m_textSetCache;
    VkPipelineLayout      m_text2DLayout   = VK_NULL_HANDLE;
    VkPipeline            m_text2DPipeline = VK_NULL_HANDLE;
    std::vector<BufferVk>     m_textVB;      // [kFramesInFlight] vértices de glifos
    std::vector<VkDeviceSize> m_textVBCap;   // [kFramesInFlight] capacidad en bytes

    CameraUBO   m_cameraData;
    RenderGraph m_graph;
};

}  // namespace pk
