// Renderer/Vulkan/VulkanInstance.h — instancia Vulkan vía vk-bootstrap.
// Diseño: MotorGrafico_IndiceMaestro.md (Fase 1). Validation layers ON en debug.
#pragma once

#include <VkBootstrap.h>
#include <vulkan/vulkan.h>

namespace pk {

class VulkanInstance {
public:
    bool Initialize(const char* appName, bool enableValidation);
    void Shutdown();

    VkInstance Handle() const { return m_instance.instance; }
    const vkb::Instance& Bootstrap() const { return m_instance; }

private:
    vkb::Instance m_instance{};
    bool m_initialized = false;
};

}  // namespace pk
