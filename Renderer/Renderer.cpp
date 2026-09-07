#include "Renderer/Renderer.h"

#include "Core/Log.h"
#include "Core/Project.h"   // resolveAppFile: atlas MSDF junto al exe
#include "Assets/AssetManager.h"
#include "Renderer/Vulkan/VulkanContext.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace pk {

namespace {
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kHDRFormat   = VK_FORMAT_R16G16B16A16_SFLOAT;  // color de escena (post)

// Debe coincidir con el bloque push_constant de mesh.vert/.frag.
struct PushConstants {
    Mat4 model;
    Vec4 baseColor;
    Vec4 matParams;  // x = metallic, y = roughness
};

// Debe coincidir con el push_constant de sprite.vert/.frag.
struct SpritePush {
    Vec4 center;  // xyz centro, w ancho
    Vec4 misc;    // x alto, y alphaClip
};

// Push de los passes de post (coinciden con sus .frag).
struct TonemapPush        { float exposure; float bloomIntensity; };
struct BloomPrefilterPush { float threshold; float knee; };
struct BloomBlurPush      { Vec2  dir; };
struct DofPush            { float focusCenter; float focusRange; float maxRadius; };

// Inicia un rendering fullscreen a un color attachment (loadOp DONT_CARE: el
// triángulo lo sobrescribe entero) y fija viewport/scissor al tamaño dado.
void beginFullscreen(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent) {
    VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView   = view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea           = { { 0, 0 }, extent };
    ri.layerCount           = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &color;
    vkCmdBeginRendering(cmd, &ri);

    VkViewport vp{};
    vp.width    = static_cast<float>(extent.width);
    vp.height   = static_cast<float>(extent.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{ { 0, 0 }, extent };
    vkCmdSetScissor(cmd, 0, 1, &sc);
}
}  // namespace

bool Renderer::init(VulkanContext* ctx, AssetManager* assets) {
    m_ctx = ctx;
    m_assets = assets;
    if (!createCommandResources())  return false;
    if (!createSyncObjects())       return false;
    if (!createPerImageSemaphores())return false;
    if (!createDepth())             return false;
    if (!createShadow())            return false;
    if (!createSceneColor())        return false;
    if (!createBloomTargets())      return false;
    if (!createSceneDescriptors())  return false;
    if (!createMaterialResources()) return false;
    if (!createForwardPipeline())   return false;
    if (!createShadowPipeline())    return false;
    if (!createSpritePipeline())    return false;
    if (!createPostResources())     return false;
    if (!createPostPipelines())     return false;
    if (!create2DResources())       return false;
    if (!create2DPipelines())       return false;

    // Fuente MSDF de la UI del juego. El atlas lo copia el CMake junto al exe, así que se
    // resuelve con resolveAppFile (recurso de la APP): con la ruta relativa, el
    // AssetManager la pasaba por resolveRead y en Debug la mandaba al árbol de FUENTES
    // —donde no hay assets/fonts/— y la textura no cargaba. Si no carga, el texto
    // simplemente no se dibuja (no es fatal para el resto del render).
    const std::string atlasJson = Project::instance().resolveAppFile("assets/fonts/atlas.json");
    const std::string atlasPng  = Project::instance().resolveAppFile("assets/fonts/atlas.png");
    if (!m_uiFont.load(*m_assets, atlasJson, atlasPng))
        LOG_WARN("Renderer: la fuente MSDF de UI no cargó; el texto de juego no se verá.");
    // Proyección de pantalla para la UI: (0,0) arriba-izq, (lowRes) abajo-der, +Y abajo
    // (misma convención que la cámara 2D / NDC de Vulkan).
    m_uiProj = glm::ortho(0.0f, static_cast<float>(kLowResW),
                          0.0f, static_cast<float>(kLowResH), -1.0f, 1.0f);

    LOG_INFO("Renderer listo (forward + 2D, frames in flight = %u).", kFramesInFlight);
    return true;
}

void Renderer::setCamera(const Mat4& viewProj, const Vec3& camPos,
                         const Vec3& camRight, const Vec3& camUp) {
    m_cameraData.viewProj = viewProj;
    m_cameraData.camPos   = Vec4(camPos, 1.0f);
    m_cameraData.camRight = Vec4(camRight, 0.0f);
    m_cameraData.camUp    = Vec4(camUp, 0.0f);
}

void Renderer::setSun(const Vec3& dir, const Vec3& color, const Vec3& ambient) {
    m_cameraData.sunDir   = Vec4(dir, 0.0f);
    m_cameraData.sunColor = Vec4(color, 0.0f);
    m_cameraData.ambient  = Vec4(ambient, 0.0f);
}

void Renderer::setLights(const std::vector<Light>& lights) {
    m_cpuLights.clear();
    for (const Light& l : lights) {
        if (m_cpuLights.size() >= kMaxLights) break;
        GpuLight g{};
        g.posType  = Vec4(l.position, l.type == Light::Spot ? 1.0f : 0.0f);
        Vec3 dir   = glm::length(l.direction) > 1e-5f ? glm::normalize(l.direction)
                                                      : Vec3(0.0f, -1.0f, 0.0f);
        g.dirRange = Vec4(dir, l.range);
        g.color    = Vec4(l.color * l.intensity, 0.0f);
        g.spot     = Vec4(std::cos(glm::radians(l.innerDeg)),
                          std::cos(glm::radians(l.outerDeg)), 0.0f, 0.0f);
        m_cpuLights.push_back(g);
    }
    m_lightCount = static_cast<uint32_t>(m_cpuLights.size());
}

void Renderer::computeSunShadowMatrix() {
    // Shadow map único que cubre la escena alrededor del origen (Fase 5c).
    // Las cascadas (CSM) son la evolución siguiente; aquí basta una caja ortho.
    Vec3 dir    = glm::normalize(Vec3(m_cameraData.sunDir));
    Vec3 center(0.0f, 0.5f, 0.0f);
    float radius = 22.0f;  // cubre el plano (±20)
    Vec3 up = (std::abs(dir.y) > 0.99f) ? Vec3(0.0f, 0.0f, 1.0f) : Vec3(0.0f, 1.0f, 0.0f);
    Vec3 lightPos  = center - dir * (radius * 1.5f);
    Mat4 lightView = glm::lookAt(lightPos, center, up);
    // ortho con FORCE_DEPTH_ZERO_TO_ONE → z en [0,1]. Sin Y-flip: el shadow map
    // es su propio espacio y se muestrea con la misma matriz (consistente).
    Mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, 0.1f, radius * 3.0f);
    m_cameraData.lightViewProj = lightProj * lightView;
}

bool Renderer::createCommandResources() {
    VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_ctx->graphicsFamily();
    if (vkCreateCommandPool(m_ctx->device(), &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        LOG_ERROR("vkCreateCommandPool falló");
        return false;
    }
    m_commandBuffers.resize(kFramesInFlight);
    VkCommandBufferAllocateInfo alloc{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    alloc.commandPool        = m_commandPool;
    alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = kFramesInFlight;
    if (vkAllocateCommandBuffers(m_ctx->device(), &alloc, m_commandBuffers.data()) != VK_SUCCESS) {
        LOG_ERROR("vkAllocateCommandBuffers falló");
        return false;
    }
    return true;
}

bool Renderer::createSyncObjects() {
    m_imageAvailable.resize(kFramesInFlight);
    m_inFlight.resize(kFramesInFlight);
    VkSemaphoreCreateInfo semInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (vkCreateSemaphore(m_ctx->device(), &semInfo, nullptr, &m_imageAvailable[i]) != VK_SUCCESS ||
            vkCreateFence(m_ctx->device(), &fenceInfo, nullptr, &m_inFlight[i]) != VK_SUCCESS) {
            LOG_ERROR("Creación de sincronización falló");
            return false;
        }
    }
    return true;
}

void Renderer::destroySyncObjects() {
    for (auto s : m_imageAvailable) if (s) vkDestroySemaphore(m_ctx->device(), s, nullptr);
    for (auto f : m_inFlight)       if (f) vkDestroyFence(m_ctx->device(), f, nullptr);
    m_imageAvailable.clear();
    m_inFlight.clear();
}

bool Renderer::createPerImageSemaphores() {
    uint32_t count = m_ctx->swapchain().ImageCount();
    m_renderFinished.resize(count);
    VkSemaphoreCreateInfo semInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < count; ++i) {
        if (vkCreateSemaphore(m_ctx->device(), &semInfo, nullptr, &m_renderFinished[i]) != VK_SUCCESS) {
            LOG_ERROR("vkCreateSemaphore (render finished) falló");
            return false;
        }
    }
    return true;
}

void Renderer::destroyPerImageSemaphores() {
    for (auto s : m_renderFinished) if (s) vkDestroySemaphore(m_ctx->device(), s, nullptr);
    m_renderFinished.clear();
}

bool Renderer::createDepth() {
    VkExtent2D e = m_ctx->swapchain().Extent();
    ImageCreateParams p;
    p.width  = e.width;
    p.height = e.height;
    p.format = kDepthFormat;
    p.usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    p.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    m_depth.resize(kFramesInFlight);
    for (auto& d : m_depth) {
        if (!d.Create(m_ctx->device(), m_ctx->allocator(), p)) {
            LOG_ERROR("createDepth falló");
            return false;
        }
    }
    return true;
}

void Renderer::destroyDepth() { m_depth.clear(); }

bool Renderer::createShadow() {
    ImageCreateParams p;
    p.width  = kShadowSize;
    p.height = kShadowSize;
    p.format = kDepthFormat;
    p.usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    p.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    m_shadow.resize(kFramesInFlight);
    for (auto& s : m_shadow) {
        if (!s.Create(m_ctx->device(), m_ctx->allocator(), p)) {
            LOG_ERROR("createShadow falló");
            return false;
        }
    }

    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (vkCreateSampler(m_ctx->device(), &si, nullptr, &m_shadowSampler) != VK_SUCCESS) {
        LOG_ERROR("vkCreateSampler (shadow) falló");
        return false;
    }
    return true;
}

void Renderer::destroyShadow() {
    m_shadow.clear();
    if (m_shadowSampler) {
        vkDestroySampler(m_ctx->device(), m_shadowSampler, nullptr);
        m_shadowSampler = VK_NULL_HANDLE;
    }
}

bool Renderer::createSceneDescriptors() {
    VkDevice dev = m_ctx->device();

    // Set 0: binding 0 = CameraUBO, binding 1 = SSBO de luces, binding 2 = shadow map.
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 3;
    li.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_cameraSetLayout) != VK_SUCCESS) {
        LOG_ERROR("vkCreateDescriptorSetLayout falló");
        return false;
    }

    VkDescriptorPoolSize poolSizes[3]{};
    poolSizes[0] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kFramesInFlight };
    poolSizes[1] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         kFramesInFlight };
    poolSizes[2] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight };
    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets       = kFramesInFlight;
    pi.poolSizeCount = 3;
    pi.pPoolSizes    = poolSizes;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        LOG_ERROR("vkCreateDescriptorPool falló");
        return false;
    }

    m_cameraUBO.resize(kFramesInFlight);
    m_lightSSBO.resize(kFramesInFlight);
    m_cameraSet.resize(kFramesInFlight);
    std::vector<VkDescriptorSetLayout> layouts(kFramesInFlight, m_cameraSetLayout);
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool     = m_descriptorPool;
    ai.descriptorSetCount = kFramesInFlight;
    ai.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(dev, &ai, m_cameraSet.data()) != VK_SUCCESS) {
        LOG_ERROR("vkAllocateDescriptorSets falló");
        return false;
    }

    const VkDeviceSize lightBufSize = 16 + kMaxLights * sizeof(GpuLight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!m_cameraUBO[i].CreateHostCoherent(m_ctx->allocator(), sizeof(CameraUBO),
                                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
            return false;
        if (!m_lightSSBO[i].CreateHostCoherent(m_ctx->allocator(), lightBufSize,
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
            return false;

        VkDescriptorBufferInfo uboInfo{ m_cameraUBO[i].Handle(), 0, sizeof(CameraUBO) };
        VkDescriptorBufferInfo ssboInfo{ m_lightSSBO[i].Handle(), 0, VK_WHOLE_SIZE };
        VkDescriptorImageInfo  shadowInfo{ m_shadowSampler, m_shadow[i].View(),
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet writes[3]{};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = m_cameraSet[i];
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo     = &uboInfo;
        writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[1].dstSet          = m_cameraSet[i];
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo     = &ssboInfo;
        writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[2].dstSet          = m_cameraSet[i];
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo      = &shadowInfo;
        vkUpdateDescriptorSets(dev, 3, writes, 0, nullptr);
    }
    return true;
}

bool Renderer::createMaterialResources() {
    VkDevice dev = m_ctx->device();

    // Set 1: binding 0 = textura de albedo (combined image sampler).
    VkDescriptorSetLayoutBinding b{};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 1;
    li.pBindings    = &b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_materialSetLayout) != VK_SUCCESS) {
        LOG_ERROR("vkCreateDescriptorSetLayout (material) falló");
        return false;
    }

    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxMaterials };
    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets       = kMaxMaterials;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_materialPool) != VK_SUCCESS) {
        LOG_ERROR("vkCreateDescriptorPool (material) falló");
        return false;
    }

    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod       = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(dev, &si, nullptr, &m_materialSampler) != VK_SUCCESS) {
        LOG_ERROR("vkCreateSampler (material) falló");
        return false;
    }

    return true;   // la textura blanca por defecto vive en el AssetManager
}

VkDescriptorSet Renderer::materialSetFor(VkImageView albedoView) {
    auto it = m_materialSetCache.find(albedoView);
    if (it != m_materialSetCache.end()) return it->second;
    VkDescriptorSet set = allocMaterialSet(albedoView, m_materialSampler);
    if (set != VK_NULL_HANDLE) m_materialSetCache[albedoView] = set;
    return set;
}

VkDescriptorSet Renderer::allocMaterialSet(VkImageView albedoView, VkSampler sampler) {
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool     = m_materialPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &m_materialSetLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_ctx->device(), &ai, &set) != VK_SUCCESS) {
        LOG_ERROR("vkAllocateDescriptorSets (material) falló (¿kMaxMaterials excedido?)");
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo ii{ sampler, albedoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet          = set;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &ii;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
    return set;
}

bool Renderer::createForwardPipeline() {
    VkShaderModule vert = loadShader("Shaders/SPV/mesh.vert.spv");
    VkShaderModule frag = loadShader("Shaders/SPV/mesh.frag.spv");
    if (!vert || !frag) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vert; stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

    VkVertexInputBindingDescription bindDesc{ 0, sizeof(Vertex3D), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex3D, pos)) };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex3D, normal)) };
    attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,    static_cast<uint32_t>(offsetof(Vertex3D, uv)) };
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &bindDesc;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAsm{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_BACK_BIT;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depth.depthTestEnable  = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1;
    blend.pAttachments    = &blendAtt;

    VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates    = dynStates;

    VkPushConstantRange pushRange{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(PushConstants) };
    VkDescriptorSetLayout setLayouts[2] = { m_cameraSetLayout, m_materialSetLayout };
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount         = 2;
    layoutInfo.pSetLayouts            = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_forwardLayout) != VK_SUCCESS) {
        LOG_ERROR("vkCreatePipelineLayout falló");
        return false;
    }

    VkFormat colorFormat = kHDRFormat;  // la escena se dibuja a HDR (post-proceso)
    VkPipelineRenderingCreateInfo renderingInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    renderingInfo.colorAttachmentCount    = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    renderingInfo.depthAttachmentFormat   = kDepthFormat;

    VkGraphicsPipelineCreateInfo pipeInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipeInfo.pNext               = &renderingInfo;
    pipeInfo.stageCount          = 2;
    pipeInfo.pStages             = stages;
    pipeInfo.pVertexInputState   = &vertexInput;
    pipeInfo.pInputAssemblyState = &inputAsm;
    pipeInfo.pViewportState      = &viewport;
    pipeInfo.pRasterizationState = &raster;
    pipeInfo.pMultisampleState   = &ms;
    pipeInfo.pDepthStencilState  = &depth;
    pipeInfo.pColorBlendState    = &blend;
    pipeInfo.pDynamicState       = &dynamic;
    pipeInfo.layout              = m_forwardLayout;
    pipeInfo.renderPass          = VK_NULL_HANDLE;

    VkResult r = vkCreateGraphicsPipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &pipeInfo,
                                           nullptr, &m_forwardPipeline);
    vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
    vkDestroyShaderModule(m_ctx->device(), frag, nullptr);
    if (r != VK_SUCCESS) {
        LOG_ERROR("vkCreateGraphicsPipelines (forward) falló");
        return false;
    }
    return true;
}

bool Renderer::createShadowPipeline() {
    VkShaderModule vert = loadShader("Shaders/SPV/shadow.vert.spv");
    if (!vert) return false;

    VkPipelineShaderStageCreateInfo stage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vert;
    stage.pName = "main";

    // Solo posición (la normal del vértice se ignora en el depth-only).
    VkVertexInputBindingDescription bindDesc{ 0, sizeof(Vertex3D), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attr{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                            static_cast<uint32_t>(offsetof(Vertex3D, pos)) };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &bindDesc;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions    = &attr;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // Cull FRONT en el shadow pass: renderizamos las caras traseras (respecto a la
    // luz). Así el self-shadowing/acne queda DENTRO de la geometría y podemos usar
    // un bias mínimo → la sombra se pega al objeto (sin "peter panning").
    rs.cullMode    = VK_CULL_MODE_FRONT_BIT;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    rs.depthBiasEnable         = VK_TRUE;
    rs.depthBiasConstantFactor = 0.75f;
    rs.depthBiasSlopeFactor    = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 0;  // depth-only, sin color

    VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates    = dynStates;

    VkPushConstantRange pr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4) };
    VkPipelineLayoutCreateInfo li{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges    = &pr;
    if (vkCreatePipelineLayout(m_ctx->device(), &li, nullptr, &m_shadowLayout) != VK_SUCCESS) {
        LOG_ERROR("vkCreatePipelineLayout (shadow) falló");
        return false;
    }

    VkPipelineRenderingCreateInfo renderingInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    renderingInfo.colorAttachmentCount  = 0;
    renderingInfo.depthAttachmentFormat = kDepthFormat;

    VkGraphicsPipelineCreateInfo pipeInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipeInfo.pNext               = &renderingInfo;
    pipeInfo.stageCount          = 1;
    pipeInfo.pStages             = &stage;
    pipeInfo.pVertexInputState   = &vi;
    pipeInfo.pInputAssemblyState = &ia;
    pipeInfo.pViewportState      = &vp;
    pipeInfo.pRasterizationState = &rs;
    pipeInfo.pMultisampleState   = &ms;
    pipeInfo.pDepthStencilState  = &ds;
    pipeInfo.pColorBlendState    = &cb;
    pipeInfo.pDynamicState       = &dy;
    pipeInfo.layout              = m_shadowLayout;
    pipeInfo.renderPass          = VK_NULL_HANDLE;

    VkResult r = vkCreateGraphicsPipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &pipeInfo,
                                           nullptr, &m_shadowPipeline);
    vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
    if (r != VK_SUCCESS) {
        LOG_ERROR("vkCreateGraphicsPipelines (shadow) falló");
        return false;
    }
    return true;
}

bool Renderer::createSpritePipeline() {
    // Sampler NEAREST (pixel-art crujiente, sin difuminar).
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter    = VK_FILTER_NEAREST;
    si.minFilter    = VK_FILTER_NEAREST;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_ctx->device(), &si, nullptr, &m_spriteSampler) != VK_SUCCESS) {
        LOG_ERROR("vkCreateSampler (sprite) falló");
        return false;
    }

    VkShaderModule vert = loadShader("Shaders/SPV/sprite.vert.spv");
    VkShaderModule frag = loadShader("Shaders/SPV/sprite.frag.spv");
    if (!vert || !frag) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vert; stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

    // Sin vertex input: el quad se genera en el shader (gl_VertexIndex).
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

    VkPipelineInputAssemblyStateCreateInfo inputAsm{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;   // billboard de doble cara
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depth.depthTestEnable  = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;   // alpha-clip → escribe depth, ordena con la geometría
    depth.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1;
    blend.pAttachments    = &blendAtt;

    VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates    = dynStates;

    VkPushConstantRange pushRange{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(SpritePush) };
    VkDescriptorSetLayout setLayouts[2] = { m_cameraSetLayout, m_materialSetLayout };
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount         = 2;
    layoutInfo.pSetLayouts            = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &m_spriteLayout) != VK_SUCCESS) {
        LOG_ERROR("vkCreatePipelineLayout (sprite) falló");
        return false;
    }

    VkFormat colorFormat = kHDRFormat;  // la escena se dibuja a HDR (post-proceso)
    VkPipelineRenderingCreateInfo renderingInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    renderingInfo.colorAttachmentCount    = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    renderingInfo.depthAttachmentFormat   = kDepthFormat;

    VkGraphicsPipelineCreateInfo pipeInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipeInfo.pNext               = &renderingInfo;
    pipeInfo.stageCount          = 2;
    pipeInfo.pStages             = stages;
    pipeInfo.pVertexInputState   = &vertexInput;
    pipeInfo.pInputAssemblyState = &inputAsm;
    pipeInfo.pViewportState      = &viewport;
    pipeInfo.pRasterizationState = &raster;
    pipeInfo.pMultisampleState   = &ms;
    pipeInfo.pDepthStencilState  = &depth;
    pipeInfo.pColorBlendState    = &blend;
    pipeInfo.pDynamicState       = &dynamic;
    pipeInfo.layout              = m_spriteLayout;
    pipeInfo.renderPass          = VK_NULL_HANDLE;

    VkResult r = vkCreateGraphicsPipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &pipeInfo,
                                           nullptr, &m_spritePipeline);
    vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
    vkDestroyShaderModule(m_ctx->device(), frag, nullptr);
    if (r != VK_SUCCESS) {
        LOG_ERROR("vkCreateGraphicsPipelines (sprite) falló");
        return false;
    }
    return true;
}

void Renderer::setSprite(TextureHandle tex, const Vec3& pos, const Vec2& size) {
    Texture* t = m_assets ? m_assets->getTexture(tex) : nullptr;
    if (!t) { m_hasSprite = false; return; }
    // El descriptor set solo cambia con la textura. Reservarlo cada frame agotaría
    // el pool en ~128 frames (pos/tamaño ya van por push constant, son baratos).
    if (m_spriteSet == VK_NULL_HANDLE || tex != m_spriteTex) {
        m_spriteTex = tex;
        m_spriteSet = allocMaterialSet(t->view(), m_spriteSampler);   // sampler NEAREST
    }
    m_spritePos  = pos;
    m_spriteSize = size;
    m_hasSprite  = (m_spriteSet != VK_NULL_HANDLE);
}

bool Renderer::createSceneColor() {
    VkExtent2D e = m_ctx->swapchain().Extent();
    ImageCreateParams p;
    p.width  = e.width;
    p.height = e.height;
    p.format = kHDRFormat;
    p.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    m_sceneColor.resize(kFramesInFlight);
    m_dofColor.resize(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!m_sceneColor[i].Create(m_ctx->device(), m_ctx->allocator(), p) ||
            !m_dofColor[i].Create(m_ctx->device(), m_ctx->allocator(), p)) {
            LOG_ERROR("createSceneColor falló");
            return false;
        }
    }
    return true;
}

void Renderer::destroySceneColor() {
    m_sceneColor.clear();
    m_dofColor.clear();
}

bool Renderer::createBloomTargets() {
    VkExtent2D e = m_ctx->swapchain().Extent();
    m_bloomExtent = { (e.width  > 1 ? e.width  / 2 : 1u),
                      (e.height > 1 ? e.height / 2 : 1u) };
    ImageCreateParams p;
    p.width  = m_bloomExtent.width;
    p.height = m_bloomExtent.height;
    p.format = kHDRFormat;
    p.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    m_bloomTex.resize(kFramesInFlight);
    m_bloomTemp.resize(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!m_bloomTex[i].Create(m_ctx->device(), m_ctx->allocator(), p) ||
            !m_bloomTemp[i].Create(m_ctx->device(), m_ctx->allocator(), p)) {
            LOG_ERROR("createBloomTargets falló");
            return false;
        }
    }
    return true;
}

void Renderer::destroyBloomTargets() {
    m_bloomTex.clear();
    m_bloomTemp.clear();
}

bool Renderer::createPostResources() {
    VkDevice dev = m_ctx->device();

    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &si, nullptr, &m_postSampler) != VK_SUCCESS) {
        LOG_ERROR("vkCreateSampler (post) falló");
        return false;
    }

    // Layouts: 1 sampler (prefilter/blur) y 2 samplers (tonemap = hdr + bloom).
    auto makeLayout = [&](uint32_t count, VkDescriptorSetLayout& out) -> bool {
        std::vector<VkDescriptorSetLayoutBinding> bs(count);
        for (uint32_t i = 0; i < count; ++i) {
            bs[i] = {};
            bs[i].binding         = i;
            bs[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bs[i].descriptorCount = 1;
            bs[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = count;
        li.pBindings    = bs.data();
        return vkCreateDescriptorSetLayout(dev, &li, nullptr, &out) == VK_SUCCESS;
    };
    if (!makeLayout(1, m_postSetLayout) || !makeLayout(2, m_tonemapSetLayout)) {
        LOG_ERROR("createPostResources: set layouts fallaron");
        return false;
    }

    // Pool: dof + prefilter + blurH + blurV (1 sampler c/u) + tonemap (2) por frame.
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight * 6 };
    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets       = kFramesInFlight * 5;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_postPool) != VK_SUCCESS) {
        LOG_ERROR("vkCreateDescriptorPool (post) falló");
        return false;
    }

    auto allocSets = [&](VkDescriptorSetLayout layout, std::vector<VkDescriptorSet>& out) -> bool {
        out.resize(kFramesInFlight);
        std::vector<VkDescriptorSetLayout> layouts(kFramesInFlight, layout);
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool     = m_postPool;
        ai.descriptorSetCount = kFramesInFlight;
        ai.pSetLayouts        = layouts.data();
        return vkAllocateDescriptorSets(dev, &ai, out.data()) == VK_SUCCESS;
    };
    if (!allocSets(m_postSetLayout, m_dofSet) ||
        !allocSets(m_postSetLayout, m_prefilterSet) ||
        !allocSets(m_postSetLayout, m_blurHSet) ||
        !allocSets(m_postSetLayout, m_blurVSet) ||
        !allocSets(m_tonemapSetLayout, m_tonemapSet)) {
        LOG_ERROR("createPostResources: alloc de sets falló");
        return false;
    }

    updatePostSets();
    return true;
}

void Renderer::updatePostSets() {
    VkDevice dev = m_ctx->device();
    auto write1 = [&](VkDescriptorSet set, VkImageView view) {
        VkDescriptorImageInfo ii{ m_postSampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    };
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        write1(m_dofSet[i],       m_sceneColor[i].View());   // DoF lee la escena nítida
        write1(m_prefilterSet[i], m_sceneColor[i].View());   // bloom desde la escena nítida
        write1(m_blurHSet[i],     m_bloomTex[i].View());
        write1(m_blurVSet[i],     m_bloomTemp[i].View());

        // tonemap: hdr = escena tras DoF; bloom = bloomTex.
        VkDescriptorImageInfo hdr{ m_postSampler, m_dofColor[i].View(),  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo blm{ m_postSampler, m_bloomTex[i].View(),  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet ws[2]{};
        ws[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        ws[0].dstSet = m_tonemapSet[i]; ws[0].dstBinding = 0; ws[0].descriptorCount = 1;
        ws[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ws[0].pImageInfo = &hdr;
        ws[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        ws[1].dstSet = m_tonemapSet[i]; ws[1].dstBinding = 1; ws[1].descriptorCount = 1;
        ws[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ws[1].pImageInfo = &blm;
        vkUpdateDescriptorSets(dev, 2, ws, 0, nullptr);
    }
}

bool Renderer::createFullscreenPipeline(const char* fragSpv, VkDescriptorSetLayout setLayout,
                                        uint32_t pushSize, VkFormat colorFormat,
                                        VkPipelineLayout& outLayout, VkPipeline& outPipeline,
                                        bool blendPremult) {
    VkShaderModule vert = loadShader("Shaders/SPV/fullscreen.vert.spv");
    VkShaderModule frag = loadShader(fragSpv);
    if (!vert || !frag) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vert; stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo inputAsm{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAsm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1; viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blendPremult) {   // composición "over" premultiplicada (HD encima del upscale)
        blendAtt.blendEnable         = VK_TRUE;
        blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAtt.colorBlendOp        = VK_BLEND_OP_ADD;
        blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAtt.alphaBlendOp        = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1; blend.pAttachments = &blendAtt;
    VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynStates;

    VkPushConstantRange pushRange{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize };
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts    = &setLayout;
    if (pushSize > 0) { layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &pushRange; }
    if (vkCreatePipelineLayout(m_ctx->device(), &layoutInfo, nullptr, &outLayout) != VK_SUCCESS) {
        LOG_ERROR("createFullscreenPipeline: layout falló (%s)", fragSpv);
        return false;
    }

    VkPipelineRenderingCreateInfo renderingInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    renderingInfo.colorAttachmentCount    = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo pipeInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipeInfo.pNext               = &renderingInfo;
    pipeInfo.stageCount          = 2;
    pipeInfo.pStages             = stages;
    pipeInfo.pVertexInputState   = &vertexInput;
    pipeInfo.pInputAssemblyState = &inputAsm;
    pipeInfo.pViewportState      = &viewport;
    pipeInfo.pRasterizationState = &raster;
    pipeInfo.pMultisampleState   = &ms;
    pipeInfo.pColorBlendState    = &blend;
    pipeInfo.pDynamicState       = &dynamic;
    pipeInfo.layout              = outLayout;
    pipeInfo.renderPass          = VK_NULL_HANDLE;

    VkResult r = vkCreateGraphicsPipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &pipeInfo,
                                           nullptr, &outPipeline);
    vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
    vkDestroyShaderModule(m_ctx->device(), frag, nullptr);
    if (r != VK_SUCCESS) {
        LOG_ERROR("createFullscreenPipeline: pipeline falló (%s)", fragSpv);
        return false;
    }
    return true;
}

bool Renderer::createPostPipelines() {
    if (!createFullscreenPipeline("Shaders/SPV/dof.frag.spv", m_postSetLayout,
                                  sizeof(DofPush), kHDRFormat,
                                  m_dofLayout, m_dofPipeline))
        return false;
    if (!createFullscreenPipeline("Shaders/SPV/bloom_prefilter.frag.spv", m_postSetLayout,
                                  sizeof(BloomPrefilterPush), kHDRFormat,
                                  m_bloomPrefilterLayout, m_bloomPrefilterPipeline))
        return false;
    if (!createFullscreenPipeline("Shaders/SPV/bloom_blur.frag.spv", m_postSetLayout,
                                  sizeof(BloomBlurPush), kHDRFormat,
                                  m_bloomBlurLayout, m_bloomBlurPipeline))
        return false;
    if (!createFullscreenPipeline("Shaders/SPV/tonemap.frag.spv", m_tonemapSetLayout,
                                  sizeof(TonemapPush), m_ctx->swapchain().ImageFormat(),
                                  m_tonemapLayout, m_tonemapPipeline))
        return false;
    return true;
}

VkShaderModule Renderer::loadShader(const char* spvPath) {
    std::ifstream f(spvPath, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        LOG_ERROR("No se pudo abrir el shader: %s", spvPath);
        return VK_NULL_HANDLE;
    }
    std::streamsize size = f.tellg();
    f.seekg(0);
    std::vector<char> code(static_cast<size_t>(size));
    f.read(code.data(), size);

    VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_ctx->device(), &info, nullptr, &mod) != VK_SUCCESS) {
        LOG_ERROR("vkCreateShaderModule falló: %s", spvPath);
        return VK_NULL_HANDLE;
    }
    return mod;
}

VkExtent2D Renderer::uiExtent() const {
    // Swapchain sucia: este frame la recrea ANTES de grabar (ver drawFrame), así que
    // el attachment será del tamaño nuevo. Devolverlo evita que la UI se maquete un
    // frame con el tamaño viejo y deje una franja sin pintar tras un resize.
    if (m_swapchainDirty) {
        uint32_t w = 0, h = 0;
        m_ctx->drawableSize(w, h);
        if (w > 0 && h > 0) return VkExtent2D{ w, h };
    }
    return m_ctx->swapchain().Extent();
}

bool Renderer::recreateSwapchain() {
    uint32_t w = 0, h = 0;
    m_ctx->drawableSize(w, h);
    if (w == 0 || h == 0) return false;

    vkDeviceWaitIdle(m_ctx->device());
    destroyPerImageSemaphores();
    destroyDepth();
    destroySceneColor();
    destroyBloomTargets();
    destroyHDColor();
    if (!m_ctx->recreateSwapchain(w, h)) return false;
    if (!createDepth())        return false;
    if (!createSceneColor())   return false;
    if (!createBloomTargets()) return false;
    if (!createHDColor())      return false;   // target full-res del 2D (sigue al swapchain)
    updatePostSets();   // los sets apuntan a los nuevos targets
    updateHDComposeSets();
    if (!createPerImageSemaphores()) return false;
    m_swapchainDirty = false;
    return true;
}

bool Renderer::create2DResources() {
    VkDevice dev = m_ctx->device();

    // Sampler NEAREST clamp: píxeles crujientes (upscale + sprites pixel-art).
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter    = VK_FILTER_NEAREST;
    si.minFilter    = VK_FILTER_NEAREST;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &si, nullptr, &m_sampler2D) != VK_SUCCESS) {
        LOG_ERROR("create2DResources: sampler falló");
        return false;
    }

    // Sampler LINEAR clamp para el atlas MSDF: el texto necesita interpolar las
    // distancias (con NEAREST se ven escalonadas). Mismo layout que el de sprites.
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(dev, &si, nullptr, &m_textSampler) != VK_SUCCESS) {
        LOG_ERROR("create2DResources: sampler de texto falló");
        return false;
    }

    // Sampler SMOOTH (FilterMode::Smooth): LINEAR mag/min + mipmaps trilineales para
    // arte HD (sprites detallados, p.ej. Pokémon). maxLod sin tope = usa toda la cadena
    // de mips (evita shimmer al achicar). Anisotropía apagada (no se habilita la feature
    // del device; activarla requeriría comprobar samplerAnisotropy y el maxAnisotropy).
    si.magFilter  = VK_FILTER_LINEAR;
    si.minFilter  = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.minLod     = 0.0f;
    si.maxLod     = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(dev, &si, nullptr, &m_smoothSampler) != VK_SUCCESS) {
        LOG_ERROR("create2DResources: sampler smooth falló");
        return false;
    }
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;   // restaura el estado base
    si.maxLod     = 0.0f;

    // Set layout: 1 combined image sampler en el fragment (sprites + upscale).
    VkDescriptorSetLayoutBinding b{};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 1; li.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &m_set2DLayout) != VK_SUCCESS) {
        LOG_ERROR("create2DResources: set layout falló");
        return false;
    }

    // Pool: sets de textura sprite (cache) + upscale por frame + sets de texto MSDF.
    constexpr uint32_t kMaxTex2D  = 64;
    constexpr uint32_t kMaxText2D = 8;   // sets de atlas MSDF (uno por fuente)
    // + 2*kFramesInFlight: sets de upscale (lowRes) y de composición (hdColor), uno por frame.
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             kMaxTex2D + kMaxText2D + 2 * kFramesInFlight };
    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = kMaxTex2D + kMaxText2D + 2 * kFramesInFlight;
    pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &m_pool2D) != VK_SUCCESS) {
        LOG_ERROR("create2DResources: pool falló");
        return false;
    }

    // Target lowRes: NO depende del swapchain → no se recrea al redimensionar.
    ImageCreateParams p;
    p.width  = kLowResW;
    p.height = kLowResH;
    p.format = m_ctx->swapchain().ImageFormat();
    p.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    m_lowRes.resize(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!m_lowRes[i].Create(dev, m_ctx->allocator(), p)) {
            LOG_ERROR("create2DResources: lowRes falló");
            return false;
        }
    }

    // Sets de upscale (uno por frame) apuntando a su lowRes (fijo).
    m_upscaleSet.resize(kFramesInFlight);
    std::vector<VkDescriptorSetLayout> layouts(kFramesInFlight, m_set2DLayout);
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool     = m_pool2D;
    ai.descriptorSetCount = kFramesInFlight;
    ai.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(dev, &ai, m_upscaleSet.data()) != VK_SUCCESS) {
        LOG_ERROR("create2DResources: upscale sets fallaron");
        return false;
    }
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorImageInfo ii{ m_sampler2D, m_lowRes[i].View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = m_upscaleSet[i]; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    // Target HD full-res (camino Smooth) + sus sets de composición (sampler smooth/linear).
    // El target se recrea con el swapchain; los sets se reescriben en updateHDComposeSets.
    if (!createHDColor()) return false;
    m_hdComposeSet.resize(kFramesInFlight);
    std::vector<VkDescriptorSetLayout> hlayouts(kFramesInFlight, m_set2DLayout);
    VkDescriptorSetAllocateInfo hai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    hai.descriptorPool     = m_pool2D;
    hai.descriptorSetCount = kFramesInFlight;
    hai.pSetLayouts        = hlayouts.data();
    if (vkAllocateDescriptorSets(dev, &hai, m_hdComposeSet.data()) != VK_SUCCESS) {
        LOG_ERROR("create2DResources: compose sets fallaron");
        return false;
    }
    updateHDComposeSets();

    // Vertex buffers dinámicos (uno por frame); se crean al primer uso.
    m_spriteVB.resize(kFramesInFlight);
    m_spriteVBCap.assign(kFramesInFlight, 0);
    m_uiSpriteVB.resize(kFramesInFlight);
    m_uiSpriteVBCap.assign(kFramesInFlight, 0);
    m_textVB.resize(kFramesInFlight);
    m_textVBCap.assign(kFramesInFlight, 0);
    return true;
}

bool Renderer::create2DPipelines() {
    const VkFormat fmt = m_ctx->swapchain().ImageFormat();

    // Upscale: fullscreen nearest lowRes → swapchain (sin push constants).
    if (!createFullscreenPipeline("Shaders/SPV/upscale.frag.spv", m_set2DLayout,
                                  0, fmt, m_upscaleLayout, m_upscalePipeline))
        return false;

    // Composición HD: mismo fullscreen que el upscale (muestrea un color target con su
    // sampler) pero con blend PREMULTIPLICADO → dibuja el target full-res (Smooth + UI +
    // texto) ENCIMA del upscale del lowRes. Reusa upscale.frag (saca texture(tex,uv) tal
    // cual, ya premultiplicado al dibujar sobre un target transparente).
    if (!createFullscreenPipeline("Shaders/SPV/upscale.frag.spv", m_set2DLayout,
                                  0, fmt, m_composeHDLayout, m_composeHDPipeline,
                                  /*blendPremult=*/true))
        return false;

    // Sprite2D: vértices (pos2/uv2/color4), push = mat4 viewProj (VS), set0 = sampler.
    VkShaderModule vert = loadShader("Shaders/SPV/sprite2d.vert.spv");
    VkShaderModule frag = loadShader("Shaders/SPV/sprite2d.frag.spv");
    if (!vert || !frag) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vert; stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

    VkVertexInputBindingDescription bind{ 0, sizeof(Vertex2D), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT,       static_cast<uint32_t>(offsetof(Vertex2D, pos)) };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT,       static_cast<uint32_t>(offsetof(Vertex2D, uv)) };
    attrs[2] = { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(Vertex2D, color)) };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 1; vi.pVertexBindingDescriptions   = &bind;
    vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Blend PREMULTIPLICADO para sprites: src ya viene con r/g/b *= a (premultiplicado al
    // cargar las texturas Smooth). Va emparejado con ese premultiplicado y evita los halos
    // oscuros del filtrado lineal en bordes anti-aliados. NOTA: las texturas Pixel NO se
    // premultiplican; con alpha 0/1 (pixel-art) el resultado es idéntico al alpha straight,
    // así que comparten este pipeline sin artefactos visibles.
    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable         = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp        = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.alphaBlendOp        = VK_BLEND_OP_ADD;
    ba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsc{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dsc.dynamicStateCount = 2; dsc.pDynamicStates = dyn;

    VkPushConstantRange pc{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4) };
    VkPipelineLayoutCreateInfo plc{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plc.setLayoutCount = 1; plc.pSetLayouts = &m_set2DLayout;
    plc.pushConstantRangeCount = 1; plc.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(m_ctx->device(), &plc, nullptr, &m_sprite2DLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
        vkDestroyShaderModule(m_ctx->device(), frag, nullptr);
        LOG_ERROR("create2DPipelines: layout falló");
        return false;
    }

    VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.pNext = &ri; gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
    gp.pDynamicState = &dsc; gp.layout = m_sprite2DLayout;
    VkResult r = vkCreateGraphicsPipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &gp, nullptr, &m_sprite2DPipeline);
    vkDestroyShaderModule(m_ctx->device(), vert, nullptr);
    vkDestroyShaderModule(m_ctx->device(), frag, nullptr);
    if (r != VK_SUCCESS) { LOG_ERROR("create2DPipelines: pipeline falló"); return false; }

    // Text2D: mismo Vertex2D/blend/estado que sprite2d, pero shaders MSDF y un push
    // de {mat4 viewProj, float pxRange} visible en VS (matriz) y FS (pxRange).
    VkShaderModule tvert = loadShader("Shaders/SPV/text2d.vert.spv");
    VkShaderModule tfrag = loadShader("Shaders/SPV/text2d.frag.spv");
    if (!tvert || !tfrag) {
        if (tvert) vkDestroyShaderModule(m_ctx->device(), tvert, nullptr);
        if (tfrag) vkDestroyShaderModule(m_ctx->device(), tfrag, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo tstages[2]{};
    tstages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    tstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   tstages[0].module = tvert; tstages[0].pName = "main";
    tstages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    tstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; tstages[1].module = tfrag; tstages[1].pName = "main";

    VkPushConstantRange tpc{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(Mat4) + sizeof(float) };
    VkPipelineLayoutCreateInfo tplc{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    tplc.setLayoutCount = 1; tplc.pSetLayouts = &m_set2DLayout;
    tplc.pushConstantRangeCount = 1; tplc.pPushConstantRanges = &tpc;
    if (vkCreatePipelineLayout(m_ctx->device(), &tplc, nullptr, &m_text2DLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_ctx->device(), tvert, nullptr);
        vkDestroyShaderModule(m_ctx->device(), tfrag, nullptr);
        LOG_ERROR("create2DPipelines: layout de texto falló");
        return false;
    }
    // El texto MSDF emite alpha STRAIGHT (vColor.a * opacity), NO premultiplicado: necesita
    // su propio blend SRC_ALPHA / ONE_MINUS_SRC_ALPHA (el sprite2d ahora es premultiplicado).
    VkPipelineColorBlendAttachmentState tba = ba;
    tba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    VkPipelineColorBlendStateCreateInfo tcb = cb;
    tcb.pAttachments = &tba;

    VkGraphicsPipelineCreateInfo tgp = gp;   // reusa vi/ia/vp/rs/ms/dsc/ri
    tgp.pStages          = tstages;
    tgp.pColorBlendState = &tcb;             // blend straight (no premultiplicado)
    tgp.layout           = m_text2DLayout;
    VkResult tr = vkCreateGraphicsPipelines(m_ctx->device(), VK_NULL_HANDLE, 1, &tgp, nullptr, &m_text2DPipeline);
    vkDestroyShaderModule(m_ctx->device(), tvert, nullptr);
    vkDestroyShaderModule(m_ctx->device(), tfrag, nullptr);
    if (tr != VK_SUCCESS) { LOG_ERROR("create2DPipelines: pipeline de texto falló"); return false; }
    return true;
}

VkDescriptorSet Renderer::tex2DSetFor(VkImageView view, FilterMode filter) {
    // Cache por view: cada textura tiene un view único y un único FilterMode, así que
    // el sampler queda fijado al crear el set (no hace falta indexar por sampler).
    auto it = m_tex2DCache.find(view);
    if (it != m_tex2DCache.end()) return it->second;
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_pool2D; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_set2DLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_ctx->device(), &ai, &set) != VK_SUCCESS) {
        LOG_ERROR("tex2DSetFor: alloc falló (¿demasiadas texturas 2D?)");
        return VK_NULL_HANDLE;
    }
    const VkSampler sampler = (filter == FilterMode::Smooth) ? m_smoothSampler : m_sampler2D;
    VkDescriptorImageInfo ii{ sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
    m_tex2DCache[view] = set;
    return set;
}

VkDescriptorSet Renderer::textSetFor(VkImageView view) {
    auto it = m_textSetCache.find(view);
    if (it != m_textSetCache.end()) return it->second;
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_pool2D; ai.descriptorSetCount = 1; ai.pSetLayouts = &m_set2DLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_ctx->device(), &ai, &set) != VK_SUCCESS) {
        LOG_ERROR("textSetFor: alloc falló");
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo ii{ m_textSampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &w, 0, nullptr);
    m_textSetCache[view] = set;
    return set;
}

void Renderer::drawWorldRuns(VkCommandBuffer cmd, VkBuffer vb, uint32_t vbVerts,
                             VkImageView whiteView, unsigned filterMask) {
    if (vbVerts == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sprite2DPipeline);
    vkCmdPushConstants(cmd, m_sprite2DLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &m_ortho2D);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
    for (const SpriteRun& run : m_batch.runs()) {
        Texture* t = (run.texture.valid() && m_assets) ? m_assets->getTexture(run.texture) : nullptr;
        const FilterMode f   = t ? t->filter() : FilterMode::Pixel;
        const unsigned   bit = (f == FilterMode::Pixel) ? 0x1u : 0x2u;   // bit0=Pixel, bit1=Smooth
        if (!(filterMask & bit)) continue;
        VkImageView v = t ? t->view() : whiteView;
        VkDescriptorSet set = tex2DSetFor(v ? v : whiteView, f);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sprite2DLayout, 0, 1, &set, 0, nullptr);
        vkCmdDraw(cmd, run.vertexCount, 1, run.firstVertex, 0);
    }
}

void Renderer::drawUIRuns(VkCommandBuffer cmd, VkBuffer uvb, uint32_t uvbVerts, VkImageView whiteView) {
    if (uvbVerts == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sprite2DPipeline);
    vkCmdPushConstants(cmd, m_sprite2DLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &m_uiProj);
    VkDeviceSize uoff = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &uvb, &uoff);
    for (const SpriteRun& run : m_uiBatch.runs()) {
        Texture* t = (run.texture.valid() && m_assets) ? m_assets->getTexture(run.texture) : nullptr;
        VkImageView v = t ? t->view() : whiteView;
        FilterMode  f = t ? t->filter() : FilterMode::Pixel;
        VkDescriptorSet set = tex2DSetFor(v ? v : whiteView, f);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sprite2DLayout, 0, 1, &set, 0, nullptr);
        vkCmdDraw(cmd, run.vertexCount, 1, run.firstVertex, 0);
    }
}

void Renderer::drawTextRun(VkCommandBuffer cmd, VkBuffer tvb, uint32_t tvbVerts, VkImageView atlasView) {
    if (tvbVerts == 0) return;
    struct TextPush { Mat4 vp; float pxRange; } push{ m_uiProj, m_uiFont.distanceRange() };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_text2DPipeline);
    vkCmdPushConstants(cmd, m_text2DLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(TextPush), &push);
    VkDeviceSize toff = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &tvb, &toff);
    VkDescriptorSet tset = textSetFor(atlasView);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_text2DLayout, 0, 1, &tset, 0, nullptr);
    vkCmdDraw(cmd, tvbVerts, 1, 0, 0);
}

void Renderer::recordSprite2D(GraphResource swap, VkExtent2D extent) {
    // 1) Construye el batch (CPU) y sube los vértices al buffer de este frame.
    m_batch.clear();
    for (const Sprite& s : m_sprites2D) m_batch.add(s);
    m_batch.build();

    const std::vector<Vertex2D>& verts = m_batch.vertices();
    const VkDeviceSize need = verts.size() * sizeof(Vertex2D);
    if (need > m_spriteVBCap[m_currentFrame]) {
        m_spriteVB[m_currentFrame].Destroy();
        VkDeviceSize cap = need * 2;   // holgura para crecer sin recrear cada frame
        if (cap < 4096) cap = 4096;
        if (!m_spriteVB[m_currentFrame].CreateHostCoherent(m_ctx->allocator(), cap,
                                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            LOG_ERROR("recordSprite2D: vertex buffer falló");
            m_spriteVBCap[m_currentFrame] = 0;
            return;
        }
        m_spriteVBCap[m_currentFrame] = cap;
    }
    if (need > 0)
        std::memcpy(m_spriteVB[m_currentFrame].Mapped(), verts.data(), need);

    // 1a) Sprites de UI (screen-space): batch propio, mismo pipeline pero proyección de
    //     pantalla (m_uiProj). Se dibujan sobre el mundo y bajo el texto.
    m_uiBatch.clear();
    for (const Sprite& s : m_uiSprites2D) m_uiBatch.add(s);
    m_uiBatch.build();
    const std::vector<Vertex2D>& uverts = m_uiBatch.vertices();
    const VkDeviceSize uneed = uverts.size() * sizeof(Vertex2D);
    if (uneed > m_uiSpriteVBCap[m_currentFrame]) {
        m_uiSpriteVB[m_currentFrame].Destroy();
        VkDeviceSize cap = uneed * 2;
        if (cap < 4096) cap = 4096;
        if (!m_uiSpriteVB[m_currentFrame].CreateHostCoherent(m_ctx->allocator(), cap,
                                                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            LOG_ERROR("recordSprite2D: vertex buffer de UI falló");
            m_uiSpriteVBCap[m_currentFrame] = 0;
        } else {
            m_uiSpriteVBCap[m_currentFrame] = cap;
        }
    }
    if (uneed > 0 && m_uiSpriteVBCap[m_currentFrame] >= uneed)
        std::memcpy(m_uiSpriteVB[m_currentFrame].Mapped(), uverts.data(), uneed);

    VkImageView whiteView = VK_NULL_HANDLE;
    if (m_assets)
        if (Texture* w = m_assets->getTexture(m_assets->whiteTexture()))
            whiteView = w->view();

    // 1b) Texto MSDF (CPU) y subida de su buffer de glifos (buffer propio por frame).
    m_textBatch.clear();
    if (m_uiFont.valid())
        for (const TextItem& t : m_texts2D) m_textBatch.add(t);
    m_textBatch.build(m_uiFont);
    const std::vector<Vertex2D>& tverts = m_textBatch.vertices();
    const VkDeviceSize tneed = tverts.size() * sizeof(Vertex2D);
    if (tneed > m_textVBCap[m_currentFrame]) {
        m_textVB[m_currentFrame].Destroy();
        VkDeviceSize cap = tneed * 2;   // holgura para crecer sin recrear cada frame
        if (cap < 4096) cap = 4096;
        if (!m_textVB[m_currentFrame].CreateHostCoherent(m_ctx->allocator(), cap,
                                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            LOG_ERROR("recordSprite2D: vertex buffer de texto falló");
            m_textVBCap[m_currentFrame] = 0;
        } else {
            m_textVBCap[m_currentFrame] = cap;
        }
    }
    if (tneed > 0 && m_textVBCap[m_currentFrame] >= tneed)
        std::memcpy(m_textVB[m_currentFrame].Mapped(), tverts.data(), tneed);

    VkImageView atlasView = VK_NULL_HANDLE;
    if (m_uiFont.valid() && m_assets)
        if (Texture* a = m_assets->getTexture(m_uiFont.atlas()))
            atlasView = a->view();
    VkBuffer       tvb      = m_textVB[m_currentFrame].Handle();
    const uint32_t tvbVerts = (atlasView && m_textVBCap[m_currentFrame] >= tneed)
                                  ? static_cast<uint32_t>(tverts.size()) : 0;

    // 2) Importa el lowRes (su uso previo fue el muestreo del upscale del frame -2).
    ImageVk& lr = m_lowRes[m_currentFrame];
    GraphResource lowRes = m_graph.importImage(
        "lowRes", lr.Handle(), lr.View(), VkExtent2D{ kLowResW, kLowResH },
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED);

    VkBuffer       vb      = m_spriteVB[m_currentFrame].Handle();
    const uint32_t vbVerts = static_cast<uint32_t>(verts.size());
    VkBuffer       uvb      = m_uiSpriteVB[m_currentFrame].Handle();
    const uint32_t uvbVerts = (m_uiSpriteVBCap[m_currentFrame] >= uneed)
                                  ? static_cast<uint32_t>(uverts.size()) : 0;

    // Target HD (full-res) — solo cuando el camino HD está activo. Los sprites Smooth + UI
    // + texto se dibujan aquí a resolución completa y se componen luego sobre el upscale.
    const bool hd = m_hd2D && !m_hdColor.empty();
    GraphResource hdColor = kInvalidResource;
    if (hd) {
        ImageVk& hdImg = m_hdColor[m_currentFrame];
        hdColor = m_graph.importImage(
            "hdColor", hdImg.Handle(), hdImg.View(), extent,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED);
    }

    // 3) Pass sprite2d → lowRes: SOLO los runs Pixel del mundo (tiles, look retro 480×270).
    //    Con el camino HD apagado dibuja TODO aquí (mundo + UI + texto), como antes.
    m_graph.addPass("sprite2d",
        [this, lowRes, vb, vbVerts, uvb, uvbVerts, whiteView, tvb, tvbVerts, atlasView, hd](RenderGraphBuilder& b) {
        b.writeColorAttachment(lowRes);
        return [this, lowRes, vb, vbVerts, uvb, uvbVerts, whiteView, tvb, tvbVerts, atlasView, hd](RenderPassContext& ctx) {
            VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            color.imageView   = ctx.view(lowRes);
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = { { 0.05f, 0.05f, 0.08f, 1.0f } };

            VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea = { { 0, 0 }, { kLowResW, kLowResH } };
            ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &color;
            vkCmdBeginRendering(ctx.cmd, &ri);

            VkViewport vp{};
            vp.width = static_cast<float>(kLowResW); vp.height = static_cast<float>(kLowResH);
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
            VkRect2D sc{ { 0, 0 }, { kLowResW, kLowResH } };
            vkCmdSetScissor(ctx.cmd, 0, 1, &sc);

            // Pixel siempre por lowRes; Smooth solo si NO hay camino HD (si no, van al target HD).
            drawWorldRuns(ctx.cmd, vb, vbVerts, whiteView, hd ? 0x1u : 0x3u);
            if (!hd) {
                drawUIRuns(ctx.cmd, uvb, uvbVerts, whiteView);
                drawTextRun(ctx.cmd, tvb, tvbVerts, atlasView);
            }
            vkCmdEndRendering(ctx.cmd);
        };
    });

    // 3b) Pass sprite2d_hd → hdColor (full-res): sprites Smooth + UI + texto, SIN la reducción
    //     a lowRes. Clear transparente para que el lowRes (tiles) se vea por detrás al componer.
    if (hd) {
        m_graph.addPass("sprite2d_hd",
            [this, hdColor, extent, vb, vbVerts, uvb, uvbVerts, whiteView, tvb, tvbVerts, atlasView](RenderGraphBuilder& b) {
            b.writeColorAttachment(hdColor);
            return [this, hdColor, extent, vb, vbVerts, uvb, uvbVerts, whiteView, tvb, tvbVerts, atlasView](RenderPassContext& ctx) {
                VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
                color.imageView   = ctx.view(hdColor);
                color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
                color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
                color.clearValue.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };   // transparente (premultiplicado)

                VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
                ri.renderArea = { { 0, 0 }, extent };
                ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &color;
                vkCmdBeginRendering(ctx.cmd, &ri);

                VkViewport vp{};
                vp.width = static_cast<float>(extent.width); vp.height = static_cast<float>(extent.height);
                vp.maxDepth = 1.0f;
                vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
                VkRect2D sc{ { 0, 0 }, extent };
                vkCmdSetScissor(ctx.cmd, 0, 1, &sc);

                drawWorldRuns(ctx.cmd, vb, vbVerts, whiteView, 0x2u);   // solo Smooth
                drawUIRuns(ctx.cmd, uvb, uvbVerts, whiteView);
                drawTextRun(ctx.cmd, tvb, tvbVerts, atlasView);
                vkCmdEndRendering(ctx.cmd);
            };
        });
    }

    // 4) Upscale lowRes → swapchain (nearest).
    VkDescriptorSet upSet = m_upscaleSet[m_currentFrame];
    m_graph.addPass("upscale", [this, lowRes, swap, extent, upSet](RenderGraphBuilder& b) {
        b.readSampled(lowRes);
        b.writeColorAttachment(swap);
        return [this, extent, upSet, swap](RenderPassContext& ctx) {
            beginFullscreen(ctx.cmd, ctx.view(swap), extent);
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_upscalePipeline);
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_upscaleLayout, 0, 1, &upSet, 0, nullptr);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
            vkCmdEndRendering(ctx.cmd);
        };
    });

    // 4b) Composición: target HD (Smooth + UI + texto, full-res) ENCIMA del upscale, con blend
    //     premultiplicado. loadOp LOAD para preservar lo que dejó el upscale (los tiles).
    if (hd) {
        VkDescriptorSet hdSet = m_hdComposeSet[m_currentFrame];
        m_graph.addPass("compose_hd", [this, hdColor, swap, extent, hdSet](RenderGraphBuilder& b) {
            b.readSampled(hdColor);
            b.writeColorAttachment(swap);
            return [this, extent, hdSet, swap](RenderPassContext& ctx) {
                VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
                color.imageView   = ctx.view(swap);
                color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;     // conserva el upscale (tiles)
                color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

                VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
                ri.renderArea = { { 0, 0 }, extent };
                ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &color;
                vkCmdBeginRendering(ctx.cmd, &ri);

                VkViewport vp{};
                vp.width = static_cast<float>(extent.width); vp.height = static_cast<float>(extent.height);
                vp.maxDepth = 1.0f;
                vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
                VkRect2D sc{ { 0, 0 }, extent };
                vkCmdSetScissor(ctx.cmd, 0, 1, &sc);

                vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_composeHDPipeline);
                vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_composeHDLayout, 0, 1, &hdSet, 0, nullptr);
                vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
                vkCmdEndRendering(ctx.cmd);
            };
        });
    }

    // 5) Editor UI encima (igual que en el modo 3D).
    if (m_uiCallback) {
        m_graph.addPass("editor", [this, swap, extent](RenderGraphBuilder& b) {
            b.writeColorAttachment(swap);
            return [this, swap, extent](RenderPassContext& ctx) {
                m_uiCallback(ctx.cmd, ctx.view(swap), extent);
            };
        });
    }
}

void Renderer::destroy2D() {
    VkDevice dev = m_ctx->device();
    m_tex2DCache.clear();
    m_textSetCache.clear();   // los sets se liberan con el pool
    if (m_text2DPipeline)   vkDestroyPipeline(dev, m_text2DPipeline, nullptr);
    if (m_text2DLayout)     vkDestroyPipelineLayout(dev, m_text2DLayout, nullptr);
    if (m_sprite2DPipeline) vkDestroyPipeline(dev, m_sprite2DPipeline, nullptr);
    if (m_sprite2DLayout)   vkDestroyPipelineLayout(dev, m_sprite2DLayout, nullptr);
    if (m_upscalePipeline)  vkDestroyPipeline(dev, m_upscalePipeline, nullptr);
    if (m_upscaleLayout)    vkDestroyPipelineLayout(dev, m_upscaleLayout, nullptr);
    if (m_composeHDPipeline) vkDestroyPipeline(dev, m_composeHDPipeline, nullptr);
    if (m_composeHDLayout)   vkDestroyPipelineLayout(dev, m_composeHDLayout, nullptr);
    if (m_pool2D)           vkDestroyDescriptorPool(dev, m_pool2D, nullptr);
    if (m_set2DLayout)      vkDestroyDescriptorSetLayout(dev, m_set2DLayout, nullptr);
    if (m_sampler2D)        vkDestroySampler(dev, m_sampler2D, nullptr);
    if (m_smoothSampler)    vkDestroySampler(dev, m_smoothSampler, nullptr);
    if (m_textSampler)      vkDestroySampler(dev, m_textSampler, nullptr);
    m_text2DPipeline = VK_NULL_HANDLE;   m_text2DLayout = VK_NULL_HANDLE;
    m_sprite2DPipeline = VK_NULL_HANDLE; m_sprite2DLayout = VK_NULL_HANDLE;
    m_upscalePipeline  = VK_NULL_HANDLE; m_upscaleLayout  = VK_NULL_HANDLE;
    m_composeHDPipeline = VK_NULL_HANDLE; m_composeHDLayout = VK_NULL_HANDLE;
    m_pool2D = VK_NULL_HANDLE; m_set2DLayout = VK_NULL_HANDLE; m_sampler2D = VK_NULL_HANDLE;
    m_smoothSampler = VK_NULL_HANDLE;
    m_textSampler = VK_NULL_HANDLE;
    m_lowRes.clear();        // ImageVk destructores liberan VMA
    m_hdColor.clear();
    m_spriteVB.clear();      // BufferVk destructores liberan VMA
    m_spriteVBCap.clear();
    m_uiSpriteVB.clear();
    m_uiSpriteVBCap.clear();
    m_textVB.clear();
    m_textVBCap.clear();
}

bool Renderer::createHDColor() {
    VkExtent2D e = m_ctx->swapchain().Extent();
    ImageCreateParams p;
    p.width  = e.width;
    p.height = e.height;
    p.format = m_ctx->swapchain().ImageFormat();   // mismo formato que lowRes/swapchain (pipelines compatibles)
    p.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    m_hdColor.resize(kFramesInFlight);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (!m_hdColor[i].Create(m_ctx->device(), m_ctx->allocator(), p)) {
            LOG_ERROR("createHDColor falló");
            return false;
        }
    }
    return true;
}

void Renderer::destroyHDColor() { m_hdColor.clear(); }

void Renderer::updateHDComposeSets() {
    VkDevice dev = m_ctx->device();
    for (uint32_t i = 0; i < kFramesInFlight && i < m_hdComposeSet.size(); ++i) {
        VkDescriptorImageInfo ii{ m_smoothSampler, m_hdColor[i].View(),
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = m_hdComposeSet[i]; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }
}

void Renderer::recordCommands(VkCommandBuffer cmd, uint32_t imageIndex) {
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImage     image  = m_ctx->swapchain().Image(imageIndex);
    VkImageView view   = m_ctx->swapchain().ImageView(imageIndex);
    VkExtent2D  extent = m_ctx->swapchain().Extent();

    m_graph.reset();
    GraphResource swap = m_graph.importImage(
        "swapchain", image, view, extent, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // Camino 2D clásico: grafo propio (sprite2d → lowRes → upscale → swapchain).
    if (m_mode == RenderMode::Sprite2D) {
        recordSprite2D(swap, extent);
        m_graph.compile();
        m_graph.execute(cmd);
        vkEndCommandBuffer(cmd);
        return;
    }

    // Color de escena HDR: el forward escribe aquí; el tonemap lo lee. Su uso
    // previo (frame anterior) fue el muestreo del tonemap (FRAGMENT_SHADER).
    ImageVk& hdrImg = m_sceneColor[m_currentFrame];
    GraphResource scene = m_graph.importImage(
        "sceneColor", hdrImg.Handle(), hdrImg.View(), extent, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED);

    ImageVk& depthImg = m_depth[m_currentFrame];
    GraphResource depth = m_graph.importImage(
        "depth", depthImg.Handle(), depthImg.View(), extent, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        // El "estado previo" es el store de profundidad del último uso de ESTA
        // imagen (hace kFramesInFlight frames, ya garantizado por la fence). Lo
        // declaramos para que la barrera de carga ordene tras él (sync-validation).
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED);   // sin transición final (se limpia cada frame)

    ImageVk& shadowImg = m_shadow[m_currentFrame];
    // El uso previo de ESTA imagen fue el muestreo en el forward del frame
    // anterior (FRAGMENT_SHADER); lo declaramos para que la barrera ordene tras él.
    GraphResource shadow = m_graph.importImage(
        "shadow", shadowImg.Handle(), shadowImg.View(),
        VkExtent2D{ kShadowSize, kShadowSize }, VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED);

    ImageVk& dofImg = m_dofColor[m_currentFrame];
    GraphResource dofColor = m_graph.importImage(
        "dofColor", dofImg.Handle(), dofImg.View(), extent, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED);

    ImageVk& bloomTexImg = m_bloomTex[m_currentFrame];
    ImageVk& bloomTmpImg = m_bloomTemp[m_currentFrame];
    GraphResource bloomTex = m_graph.importImage(
        "bloomTex", bloomTexImg.Handle(), bloomTexImg.View(), m_bloomExtent, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED);
    GraphResource bloomTmp = m_graph.importImage(
        "bloomTemp", bloomTmpImg.Handle(), bloomTmpImg.View(), m_bloomExtent, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED);

    VkDescriptorSet set          = m_cameraSet[m_currentFrame];
    VkDescriptorSet tonemapSet   = m_tonemapSet[m_currentFrame];
    VkDescriptorSet prefilterSet = m_prefilterSet[m_currentFrame];
    VkDescriptorSet blurHSet     = m_blurHSet[m_currentFrame];
    VkDescriptorSet blurVSet     = m_blurVSet[m_currentFrame];
    VkDescriptorSet dofSet       = m_dofSet[m_currentFrame];
    VkExtent2D      bloomExtent  = m_bloomExtent;

    // Shadow pass: profundidad de la escena desde el sol (el grafo lo ordena
    // antes del forward, que lee el shadow map).
    m_graph.addPass("shadow", [this, shadow](RenderGraphBuilder& b) {
        b.writeDepthAttachment(shadow);
        return [this, shadow](RenderPassContext& ctx) {
            VkRenderingAttachmentInfo d{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            d.imageView   = ctx.view(shadow);
            d.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            d.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
            d.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
            d.clearValue.depthStencil = { 1.0f, 0 };

            VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea       = { { 0, 0 }, { kShadowSize, kShadowSize } };
            ri.layerCount       = 1;
            ri.pDepthAttachment = &d;
            vkCmdBeginRendering(ctx.cmd, &ri);

            VkViewport vp{};
            vp.width = static_cast<float>(kShadowSize);
            vp.height = static_cast<float>(kShadowSize);
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
            VkRect2D sciss{ { 0, 0 }, { kShadowSize, kShadowSize } };
            vkCmdSetScissor(ctx.cmd, 0, 1, &sciss);

            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
            const Mat4 vpL = m_cameraData.lightViewProj;
            for (const DrawItem& it : m_drawItems) {
                MeshVk* mesh = m_assets ? m_assets->getMesh(it.mesh) : nullptr;
                if (!mesh) continue;
                Mat4 mvp = vpL * it.transform;
                vkCmdPushConstants(ctx.cmd, m_shadowLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &mvp);
                mesh->draw(ctx.cmd);
            }

            vkCmdEndRendering(ctx.cmd);
        };
    });

    m_graph.addPass("forward", [this, scene, depth, shadow, extent, set](RenderGraphBuilder& b) {
        b.writeColorAttachment(scene);
        b.writeDepthAttachment(depth);
        b.readSampled(shadow);   // el grafo transiciona el shadow map a SHADER_READ
        return [this, scene, depth, extent, set](RenderPassContext& ctx) {
            VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            color.imageView   = ctx.view(scene);
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = { { 0.06f, 0.07f, 0.10f, 1.0f } };

            VkRenderingAttachmentInfo depthAtt{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            depthAtt.imageView   = ctx.view(depth);
            depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAtt.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAtt.clearValue.depthStencil = { 1.0f, 0 };

            VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea           = { { 0, 0 }, extent };
            ri.layerCount           = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments    = &color;
            ri.pDepthAttachment     = &depthAtt;

            vkCmdBeginRendering(ctx.cmd, &ri);

            VkViewport vp{};
            vp.width  = static_cast<float>(extent.width);
            vp.height = static_cast<float>(extent.height);
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
            VkRect2D scissor{ { 0, 0 }, extent };
            vkCmdSetScissor(ctx.cmd, 0, 1, &scissor);

            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_forwardPipeline);
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_forwardLayout, 0, 1, &set, 0, nullptr);

            const VkShaderStageFlags pcStages =
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

            // Textura blanca por defecto (materiales sin albedo).
            VkImageView whiteView = VK_NULL_HANDLE;
            if (m_assets) {
                if (Texture* w = m_assets->getTexture(m_assets->whiteTexture()))
                    whiteView = w->view();
            }

            for (const DrawItem& it : m_drawItems) {
                MeshVk* mesh = m_assets ? m_assets->getMesh(it.mesh) : nullptr;
                if (!mesh) continue;

                Vec4  baseColor(1.0f);
                float metallic = 0.0f, rough = 0.9f;
                VkImageView albedoView = whiteView;
                if (Material* mat = m_assets->getMaterial(it.material)) {
                    baseColor = mat->baseColor;
                    metallic  = mat->metallic;
                    rough     = mat->roughness;
                    if (Texture* t = m_assets->getTexture(mat->albedo))
                        albedoView = t->view();
                }

                VkDescriptorSet matSet = materialSetFor(albedoView ? albedoView : whiteView);
                vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_forwardLayout, 1, 1, &matSet, 0, nullptr);
                PushConstants pc{};
                pc.model     = it.transform;
                pc.baseColor = baseColor;
                pc.matParams = Vec4(metallic, rough, 0.0f, 0.0f);
                vkCmdPushConstants(ctx.cmd, m_forwardLayout, pcStages, 0, sizeof(PushConstants), &pc);
                mesh->draw(ctx.cmd);
            }

            // Sprite HD-2D (billboard) en el mismo pass forward: alpha-clip, escribe
            // depth → se ordena con la geometría 3D.
            if (m_hasSprite) {
                vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_spritePipeline);
                vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_spriteLayout, 0, 1, &set, 0, nullptr);
                vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_spriteLayout, 1, 1, &m_spriteSet, 0, nullptr);
                SpritePush sp{};
                sp.center = Vec4(m_spritePos, m_spriteSize.x);
                sp.misc   = Vec4(m_spriteSize.y, m_spriteAlphaClip, 0.0f, 0.0f);
                vkCmdPushConstants(ctx.cmd, m_spriteLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(SpritePush), &sp);
                vkCmdDraw(ctx.cmd, 6, 1, 0, 0);
            }

            vkCmdEndRendering(ctx.cmd);
        };
    });

    // DoF / tilt-shift: scene (HDR) → dofColor. Banda central nítida, bordes borrosos.
    m_graph.addPass("dof", [this, scene, dofColor, extent, dofSet](RenderGraphBuilder& b) {
        b.readSampled(scene);
        b.writeColorAttachment(dofColor);
        return [this, dofColor, extent, dofSet](RenderPassContext& ctx) {
            beginFullscreen(ctx.cmd, ctx.view(dofColor), extent);
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_dofPipeline);
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_dofLayout, 0, 1, &dofSet, 0, nullptr);
            DofPush pc{ m_post.dofFocusCenter, m_post.dofFocusRange, m_post.dofMaxRadius };
            vkCmdPushConstants(ctx.cmd, m_dofLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
            vkCmdEndRendering(ctx.cmd);
        };
    });

    // Bloom: prefilter (scene → bloomTex) → blurH (bloomTex → bloomTemp) →
    // blurV (bloomTemp → bloomTex). Todo a media resolución.
    m_graph.addPass("bloom_prefilter", [this, scene, bloomTex, bloomExtent, prefilterSet](RenderGraphBuilder& b) {
        b.readSampled(scene);
        b.writeColorAttachment(bloomTex);
        return [this, bloomTex, bloomExtent, prefilterSet](RenderPassContext& ctx) {
            beginFullscreen(ctx.cmd, ctx.view(bloomTex), bloomExtent);
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomPrefilterPipeline);
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_bloomPrefilterLayout, 0, 1, &prefilterSet, 0, nullptr);
            BloomPrefilterPush pc{ m_post.bloomThreshold, m_post.bloomKnee };
            vkCmdPushConstants(ctx.cmd, m_bloomPrefilterLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
            vkCmdEndRendering(ctx.cmd);
        };
    });
    m_graph.addPass("bloom_blurH", [this, bloomTex, bloomTmp, bloomExtent, blurHSet](RenderGraphBuilder& b) {
        b.readSampled(bloomTex);
        b.writeColorAttachment(bloomTmp);
        return [this, bloomTmp, bloomExtent, blurHSet](RenderPassContext& ctx) {
            beginFullscreen(ctx.cmd, ctx.view(bloomTmp), bloomExtent);
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomBlurPipeline);
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_bloomBlurLayout, 0, 1, &blurHSet, 0, nullptr);
            BloomBlurPush pc{ Vec2(1.0f / float(bloomExtent.width), 0.0f) };
            vkCmdPushConstants(ctx.cmd, m_bloomBlurLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
            vkCmdEndRendering(ctx.cmd);
        };
    });
    m_graph.addPass("bloom_blurV", [this, bloomTmp, bloomTex, bloomExtent, blurVSet](RenderGraphBuilder& b) {
        b.readSampled(bloomTmp);
        b.writeColorAttachment(bloomTex);
        return [this, bloomTex, bloomExtent, blurVSet](RenderPassContext& ctx) {
            beginFullscreen(ctx.cmd, ctx.view(bloomTex), bloomExtent);
            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bloomBlurPipeline);
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_bloomBlurLayout, 0, 1, &blurVSet, 0, nullptr);
            BloomBlurPush pc{ Vec2(0.0f, 1.0f / float(bloomExtent.height)) };
            vkCmdPushConstants(ctx.cmd, m_bloomBlurLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
            vkCmdEndRendering(ctx.cmd);
        };
    });

    // Tonemap: HDR (tras DoF) + bloom → swapchain. El grafo lo ordena al final.
    m_graph.addPass("tonemap", [this, dofColor, bloomTex, swap, extent, tonemapSet](RenderGraphBuilder& b) {
        b.readSampled(dofColor);
        b.readSampled(bloomTex);
        b.writeColorAttachment(swap);
        return [this, swap, extent, tonemapSet](RenderPassContext& ctx) {
            VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            color.imageView   = ctx.view(swap);
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // el fullscreen sobrescribe
            color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea           = { { 0, 0 }, extent };
            ri.layerCount           = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments    = &color;
            vkCmdBeginRendering(ctx.cmd, &ri);

            VkViewport vp{};
            vp.width    = static_cast<float>(extent.width);
            vp.height   = static_cast<float>(extent.height);
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(ctx.cmd, 0, 1, &vp);
            VkRect2D sciss{ { 0, 0 }, extent };
            vkCmdSetScissor(ctx.cmd, 0, 1, &sciss);

            vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapPipeline);
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_tonemapLayout, 0, 1, &tonemapSet, 0, nullptr);
            TonemapPush tp{ m_post.exposure, m_post.bloomIntensity };
            vkCmdPushConstants(ctx.cmd, m_tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(TonemapPush), &tp);
            vkCmdDraw(ctx.cmd, 3, 1, 0, 0);

            vkCmdEndRendering(ctx.cmd);
        };
    });

    // Editor UI (FluentUI): pass final sobre la swapchain (después del tonemap).
    if (m_uiCallback) {
        m_graph.addPass("editor", [this, swap, extent](RenderGraphBuilder& b) {
            b.writeColorAttachment(swap);
            return [this, swap, extent](RenderPassContext& ctx) {
                m_uiCallback(ctx.cmd, ctx.view(swap), extent);
            };
        });
    }

    m_graph.compile();
    m_graph.execute(cmd);

    vkEndCommandBuffer(cmd);
}

void Renderer::drawFrame() {
    VkDevice dev = m_ctx->device();

    if (m_swapchainDirty) {
        if (!recreateSwapchain()) return;
    }

    vkWaitForFences(dev, 1, &m_inFlight[m_currentFrame], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(dev, m_ctx->swapchain().Handle(), UINT64_MAX,
                                         m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { m_swapchainDirty = true; return; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        LOG_ERROR("vkAcquireNextImageKHR falló (%d)", acq);
        return;
    }

    vkResetFences(dev, 1, &m_inFlight[m_currentFrame]);

    computeSunShadowMatrix();  // matriz de luz del sol para el shadow map

    // Sube los datos de cámara/sol de este frame (la fence ya garantizó que el
    // UBO de este frame-in-flight no está en uso).
    std::memcpy(m_cameraUBO[m_currentFrame].Mapped(), &m_cameraData, sizeof(CameraUBO));

    // Sube el buffer de luces: [uint count][pad12][GpuLight...] (std430).
    uint8_t* lp = static_cast<uint8_t*>(m_lightSSBO[m_currentFrame].Mapped());
    std::memcpy(lp, &m_lightCount, sizeof(uint32_t));
    if (m_lightCount > 0)
        std::memcpy(lp + 16, m_cpuLights.data(), m_lightCount * sizeof(GpuLight));

    VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
    recordCommands(cmd, imageIndex);

    VkSemaphoreSubmitInfo waitSem{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    waitSem.semaphore = m_imageAvailable[m_currentFrame];
    waitSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalSem{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signalSem.semaphore = m_renderFinished[imageIndex];
    signalSem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submit.waitSemaphoreInfoCount   = 1;
    submit.pWaitSemaphoreInfos      = &waitSem;
    submit.commandBufferInfoCount   = 1;
    submit.pCommandBufferInfos      = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos    = &signalSem;
    if (vkQueueSubmit2(m_ctx->graphicsQueue(), 1, &submit, m_inFlight[m_currentFrame]) != VK_SUCCESS) {
        LOG_ERROR("vkQueueSubmit2 falló");
        return;
    }

    VkSwapchainKHR swapchain = m_ctx->swapchain().Handle();
    VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = &m_renderFinished[imageIndex];
    present.swapchainCount     = 1;
    present.pSwapchains        = &swapchain;
    present.pImageIndices      = &imageIndex;
    VkResult pr = vkQueuePresentKHR(m_ctx->presentQueue(), &present);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        m_swapchainDirty = true;
    } else if (pr != VK_SUCCESS) {
        LOG_ERROR("vkQueuePresentKHR falló (%d)", pr);
    }

    m_currentFrame = (m_currentFrame + 1) % kFramesInFlight;
}

void Renderer::shutdown() {
    if (!m_ctx) return;
    vkDeviceWaitIdle(m_ctx->device());

    destroy2D();

    // Las mallas/texturas las posee el AssetManager (se liberan en su shutdown).
    m_materialSetCache.clear();   // los descriptor sets se liberan con el pool
    if (m_materialPool)      vkDestroyDescriptorPool(m_ctx->device(), m_materialPool, nullptr);
    if (m_materialSetLayout) vkDestroyDescriptorSetLayout(m_ctx->device(), m_materialSetLayout, nullptr);
    if (m_materialSampler)   vkDestroySampler(m_ctx->device(), m_materialSampler, nullptr);
    m_materialPool      = VK_NULL_HANDLE;
    m_materialSetLayout = VK_NULL_HANDLE;
    m_materialSampler   = VK_NULL_HANDLE;

    if (m_forwardPipeline) vkDestroyPipeline(m_ctx->device(), m_forwardPipeline, nullptr);
    if (m_forwardLayout)   vkDestroyPipelineLayout(m_ctx->device(), m_forwardLayout, nullptr);
    if (m_shadowPipeline)  vkDestroyPipeline(m_ctx->device(), m_shadowPipeline, nullptr);
    if (m_shadowLayout)    vkDestroyPipelineLayout(m_ctx->device(), m_shadowLayout, nullptr);
    if (m_spritePipeline)  vkDestroyPipeline(m_ctx->device(), m_spritePipeline, nullptr);
    if (m_spriteLayout)    vkDestroyPipelineLayout(m_ctx->device(), m_spriteLayout, nullptr);
    if (m_spriteSampler)   vkDestroySampler(m_ctx->device(), m_spriteSampler, nullptr);
    if (m_tonemapPipeline)        vkDestroyPipeline(m_ctx->device(), m_tonemapPipeline, nullptr);
    if (m_tonemapLayout)          vkDestroyPipelineLayout(m_ctx->device(), m_tonemapLayout, nullptr);
    if (m_bloomPrefilterPipeline) vkDestroyPipeline(m_ctx->device(), m_bloomPrefilterPipeline, nullptr);
    if (m_bloomPrefilterLayout)   vkDestroyPipelineLayout(m_ctx->device(), m_bloomPrefilterLayout, nullptr);
    if (m_bloomBlurPipeline)      vkDestroyPipeline(m_ctx->device(), m_bloomBlurPipeline, nullptr);
    if (m_bloomBlurLayout)        vkDestroyPipelineLayout(m_ctx->device(), m_bloomBlurLayout, nullptr);
    if (m_dofPipeline)            vkDestroyPipeline(m_ctx->device(), m_dofPipeline, nullptr);
    if (m_dofLayout)              vkDestroyPipelineLayout(m_ctx->device(), m_dofLayout, nullptr);
    if (m_postPool)               vkDestroyDescriptorPool(m_ctx->device(), m_postPool, nullptr);
    if (m_tonemapSetLayout)       vkDestroyDescriptorSetLayout(m_ctx->device(), m_tonemapSetLayout, nullptr);
    if (m_postSetLayout)          vkDestroyDescriptorSetLayout(m_ctx->device(), m_postSetLayout, nullptr);
    if (m_postSampler)            vkDestroySampler(m_ctx->device(), m_postSampler, nullptr);
    destroyBloomTargets();
    destroySceneColor();
    destroyShadow();

    m_cameraUBO.clear();  // BufferVk destructores liberan la memoria VMA
    m_lightSSBO.clear();
    if (m_descriptorPool)  vkDestroyDescriptorPool(m_ctx->device(), m_descriptorPool, nullptr);
    if (m_cameraSetLayout) vkDestroyDescriptorSetLayout(m_ctx->device(), m_cameraSetLayout, nullptr);

    destroyDepth();
    destroyPerImageSemaphores();
    destroySyncObjects();
    if (m_commandPool) vkDestroyCommandPool(m_ctx->device(), m_commandPool, nullptr);

    m_forwardPipeline = VK_NULL_HANDLE;
    m_forwardLayout   = VK_NULL_HANDLE;
    m_shadowPipeline  = VK_NULL_HANDLE;
    m_shadowLayout    = VK_NULL_HANDLE;
    m_descriptorPool  = VK_NULL_HANDLE;
    m_cameraSetLayout = VK_NULL_HANDLE;
    m_commandPool     = VK_NULL_HANDLE;
    m_ctx = nullptr;
}

}  // namespace pk
