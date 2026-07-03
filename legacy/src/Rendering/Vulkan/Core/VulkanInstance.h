#pragma once

#include <VkBootstrap.h>
#include <vulkan/vulkan.h>

namespace pokemotor::vk {

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

}  // namespace pokemotor::vk
