// Editor/ToolWindow.cpp — ventana OS secundaria con su propio swapchain + 2º
// contexto FluentUI (modo Vulkan compartido sobre el VkDevice del motor).
#include "Editor/ToolWindow.h"

#include "Core/Log.h"
#include "Editor/EditorTheme.h"
#include "Renderer/Vulkan/VulkanContext.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "FluentGUI.h"
#include "core/RenderBackend.h"

namespace pk {

namespace {
void transitionImage(VkCommandBuffer cmd, VkImage img,
                     VkImageLayout oldL, VkImageLayout newL,
                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                     VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask        = srcStage;  b.srcAccessMask = srcAccess;
    b.dstStageMask        = dstStage;  b.dstAccessMask = dstAccess;
    b.oldLayout           = oldL;      b.newLayout     = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = img;
    b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}
}  // namespace

void ToolWindow::makeCurrent() {
    FluentUI::SetCurrentContext(static_cast<FluentUI::UIContext*>(m_uictx));
}

bool ToolWindow::open(VulkanContext& ctx, const char* title, int width, int height) {
    if (m_open) return true;
    m_ctx = &ctx;

    m_window = SDL_CreateWindow(title, width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!m_window) { LOG_ERROR("ToolWindow: SDL_CreateWindow falló: %s", SDL_GetError()); return false; }
    m_windowId = SDL_GetWindowID(m_window);

    if (!SDL_Vulkan_CreateSurface(m_window, ctx.instance(), nullptr, &m_surface)) {
        LOG_ERROR("ToolWindow: SDL_Vulkan_CreateSurface falló: %s", SDL_GetError());
        SDL_DestroyWindow(m_window); m_window = nullptr;
        return false;
    }

    int pw = width, ph = height;
    SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
    if (!m_swapchain.Initialize(ctx.deviceObj(), m_surface,
                                static_cast<uint32_t>(pw), static_cast<uint32_t>(ph))) {
        LOG_ERROR("ToolWindow: swapchain falló");
        vkDestroySurfaceKHR(ctx.instance(), m_surface, nullptr); m_surface = VK_NULL_HANDLE;
        SDL_DestroyWindow(m_window); m_window = nullptr;
        return false;
    }
    if (!createSync()) { close(); return false; }

    // Segundo contexto FluentUI (modo Vulkan compartido) para esta ventana.
    FluentUI::VulkanSharedContext shared{};
    shared.instance         = static_cast<void*>(ctx.instance());
    shared.physicalDevice   = static_cast<void*>(ctx.physicalDevice());
    shared.device           = static_cast<void*>(ctx.device());
    shared.graphicsQueue    = static_cast<void*>(ctx.graphicsQueue());
    shared.queueFamilyIndex = ctx.graphicsFamily();
    shared.dynamicRendering = true;
    shared.colorFormat      = static_cast<uint32_t>(m_swapchain.ImageFormat());
    shared.sampleCount      = 1;

    // Contexto FluentUI PROPIO de esta ventana (no el singleton del editor):
    // CreateStandaloneContext crea su propio backend en modo Vulkan compartido sobre
    // el device del motor. Así esta ventana tiene su widget-tree/input/foco aislados.
    FluentUI::RenderBackend* be = nullptr;
    FluentUI::UIContext* c = FluentUI::CreateStandaloneContext(
        m_window, FluentUI::RenderBackendType::Vulkan, &shared, &be);
    if (!c) { LOG_ERROR("ToolWindow: CreateStandaloneContext falló"); close(); return false; }
    c->style = winuiEditorStyle();
    c->renderer.LoadIconFont("assets/fonts/lucide.ttf", 16);
    m_uictx   = c;
    m_backend = be;

    // SDL3 entrega los eventos de texto POR VENTANA. CreateStandaloneContext (ventana
    // externa) no arranca la entrada de texto (sí lo hace FluentApp, que esta ventana no
    // usa), así que la habilitamos a mano para que TextInput reciba teclas aquí.
    SDL_StartTextInput(m_window);

    m_open = true;
    m_shouldClose = false;
    m_swapchainDirty = false;
    LOG_INFO("Ventana de herramientas abierta: %s", title);
    return true;
}

bool ToolWindow::createSync() {
    VkDevice dev = m_ctx->device();

    VkCommandPoolCreateInfo pi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = m_ctx->graphicsFamily();
    if (vkCreateCommandPool(dev, &pi, nullptr, &m_cmdPool) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool        = m_cmdPool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &ai, &m_cmd) != VK_SUCCESS) return false;

    VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateSemaphore(dev, &si, nullptr, &m_imageAvailable) != VK_SUCCESS) return false;
    if (vkCreateFence(dev, &fi, nullptr, &m_inFlight) != VK_SUCCESS) return false;

    m_renderFinished.resize(m_swapchain.ImageCount());
    for (auto& s : m_renderFinished)
        if (vkCreateSemaphore(dev, &si, nullptr, &s) != VK_SUCCESS) return false;
    return true;
}

void ToolWindow::destroySync() {
    VkDevice dev = m_ctx->device();
    for (auto& s : m_renderFinished) if (s) vkDestroySemaphore(dev, s, nullptr);
    m_renderFinished.clear();
    if (m_imageAvailable) vkDestroySemaphore(dev, m_imageAvailable, nullptr);
    if (m_inFlight)       vkDestroyFence(dev, m_inFlight, nullptr);
    if (m_cmdPool)        vkDestroyCommandPool(dev, m_cmdPool, nullptr);
    m_imageAvailable = VK_NULL_HANDLE;
    m_inFlight       = VK_NULL_HANDLE;
    m_cmdPool        = VK_NULL_HANDLE;
    m_cmd            = VK_NULL_HANDLE;
}

bool ToolWindow::recreateSwapchain() {
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
    if (pw == 0 || ph == 0) return false;   // minimizada

    vkDeviceWaitIdle(m_ctx->device());
    destroySync();
    m_swapchain.Shutdown(m_ctx->deviceObj());
    if (!m_swapchain.Initialize(m_ctx->deviceObj(), m_surface,
                                static_cast<uint32_t>(pw), static_cast<uint32_t>(ph)))
        return false;
    if (!createSync()) return false;
    m_swapchainDirty = false;
    return true;
}

void ToolWindow::beginInputFrame() {
    if (!m_open) return;
    auto* c = static_cast<FluentUI::UIContext*>(m_uictx);
    FluentUI::SetCurrentContext(c);
    if (c) c->input.Update(m_window);
}

void ToolWindow::processEvent(const SDL_Event& e) {
    if (!m_open) return;
    if (sdlEventWindowId(e) != m_windowId) return;   // solo eventos de ESTA ventana

    if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) { m_shouldClose = true; return; }
    if (e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
        m_swapchainDirty = true;

    auto* c = static_cast<FluentUI::UIContext*>(m_uictx);
    FluentUI::SetCurrentContext(c);
    if (c) { SDL_Event copy = e; c->input.ProcessEvent(copy); }
}

void ToolWindow::renderFrame(float dt, const std::function<void(int, int)>& buildUI) {
    if (!m_open) return;

    if (m_swapchainDirty) { if (!recreateSwapchain()) return; }

    auto* c = static_cast<FluentUI::UIContext*>(m_uictx);
    FluentUI::SetCurrentContext(c);

    int w = 0, h = 0;
    SDL_GetWindowSize(m_window, &w, &h);
    c->renderer.SetViewport(w, h);
    FluentUI::NewFrame(dt);
    if (buildUI) buildUI(w, h);

    VkDevice dev = m_ctx->device();
    vkWaitForFences(dev, 1, &m_inFlight, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(dev, m_swapchain.Handle(), UINT64_MAX,
                                         m_imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { m_swapchainDirty = true; return; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        LOG_ERROR("ToolWindow: vkAcquireNextImageKHR falló (%d)", acq);
        return;
    }
    vkResetFences(dev, 1, &m_inFlight);

    VkCommandBuffer cmd = m_cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImage     img    = m_swapchain.Image(imageIndex);
    VkImageView view   = m_swapchain.ImageView(imageIndex);
    VkExtent2D  extent = m_swapchain.Extent();

    transitionImage(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView   = view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = { { 0.10f, 0.11f, 0.13f, 1.0f } };

    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea           = { { 0, 0 }, extent };
    ri.layerCount           = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &color;
    vkCmdBeginRendering(cmd, &ri);

    if (auto* be = static_cast<FluentUI::RenderBackend*>(m_backend))
        be->SetFrameCommandBuffer(static_cast<void*>(cmd));
    FluentUI::RenderDeferredDropdowns();
    FluentUI::Render();

    vkCmdEndRendering(cmd);

    transitionImage(cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0);
    vkEndCommandBuffer(cmd);

    VkSemaphoreSubmitInfo waitSem{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    waitSem.semaphore = m_imageAvailable;
    waitSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signalSem{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signalSem.semaphore = m_renderFinished[imageIndex];
    signalSem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cmdInfo.commandBuffer = cmd;
    VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submit.waitSemaphoreInfoCount   = 1; submit.pWaitSemaphoreInfos   = &waitSem;
    submit.commandBufferInfoCount   = 1; submit.pCommandBufferInfos   = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1; submit.pSignalSemaphoreInfos = &signalSem;
    vkQueueSubmit2(m_ctx->graphicsQueue(), 1, &submit, m_inFlight);

    VkSwapchainKHR sc = m_swapchain.Handle();
    VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = &m_renderFinished[imageIndex];
    present.swapchainCount     = 1;
    present.pSwapchains        = &sc;
    present.pImageIndices      = &imageIndex;
    VkResult pr = vkQueuePresentKHR(m_ctx->presentQueue(), &present);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) m_swapchainDirty = true;
}

void ToolWindow::close() {
    if (!m_ctx) return;
    vkDeviceWaitIdle(m_ctx->device());

    // Esta ventana posee su PROPIO contexto FluentUI (CreateStandaloneContext en
    // open()), independiente del singleton del editor principal. Lo destruimos junto
    // a su backend; DestroyStandaloneContext no toca el contexto global del editor.
    if (m_uictx) {
        FluentUI::DestroyStandaloneContext(static_cast<FluentUI::UIContext*>(m_uictx),
                                           static_cast<FluentUI::RenderBackend*>(m_backend));
        m_uictx   = nullptr;
        m_backend = nullptr;
    }
    destroySync();
    m_swapchain.Shutdown(m_ctx->deviceObj());
    if (m_surface) { vkDestroySurfaceKHR(m_ctx->instance(), m_surface, nullptr); m_surface = VK_NULL_HANDLE; }
    if (m_window)  { SDL_StopTextInput(m_window); SDL_DestroyWindow(m_window); m_window = nullptr; }

    m_open = false;
    m_shouldClose = false;
    m_windowId = 0;
    m_ctx = nullptr;   // idempotente: una segunda llamada a close() sale temprano
}

}  // namespace pk
