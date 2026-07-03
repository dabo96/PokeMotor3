#include "VulkanInstance.h"

#include <cstdio>

namespace pk {

namespace {

// Callback de validación explícito (a stderr con prefijo claro). Devuelve
// VK_FALSE para no abortar la llamada que lo disparó: seguimos y cazamos más.
VKAPI_ATTR VkBool32 VKAPI_CALL ValidationCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT        type,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*userData*/) {

    const char* sevStr = "?";
    switch (severity) {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT: sevStr = "VERBOSE"; break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:    sevStr = "INFO";    break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT: sevStr = "WARNING"; break;
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:   sevStr = "ERROR";   break;
        default: break;
    }
    const char* typeStr =
        (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)  ? "VALIDATION"  :
        (type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) ? "PERFORMANCE" :
        (type & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT)     ? "GENERAL"     : "?";

    std::fprintf(stderr, "[VK-VALIDATION][%s][%s] %s\n", sevStr, typeStr,
                 data && data->pMessage ? data->pMessage : "(no message)");
    std::fflush(stderr);
    return VK_FALSE;
}

}  // namespace

bool VulkanInstance::Initialize(const char* appName, bool enableValidation) {
    vkb::InstanceBuilder builder;
    builder.set_app_name(appName)
           .set_engine_name("PokeMotor")
           .require_api_version(1, 3, 0);

    if (enableValidation) {
        builder.request_validation_layers(true)
               .set_debug_callback(&ValidationCallback)
               .set_debug_messenger_severity(
                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
               .set_debug_messenger_type(
                   VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT    |
                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
               // Sync validation pilla barriers/transiciones de layout faltantes.
               .add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
    }

    auto built = builder.build();
    if (!built) {
        std::fprintf(stderr, "[Vulkan] Instance build failed: %s\n",
                     built.error().message().c_str());
        if (enableValidation) {
            std::fprintf(stderr, "[Vulkan] Validation pedida — comprueba que el Vulkan SDK "
                         "esté instalado y VK_LAYER_KHRONOS_validation disponible "
                         "(`vulkaninfo --summary`).\n");
        }
        return false;
    }
    m_instance = built.value();
    m_initialized = true;
    std::fprintf(stderr, "[Vulkan] Instance creada (validation=%s)\n",
                 enableValidation ? "ON" : "OFF");
    std::fflush(stderr);
    return true;
}

void VulkanInstance::Shutdown() {
    if (!m_initialized) return;
    vkb::destroy_instance(m_instance);
    m_instance = {};
    m_initialized = false;
}

}  // namespace pk
