#include "VulkanInstance.h"

#include <cstdio>

namespace pokemotor::vk {

namespace {

// Custom debug callback — explicit so we can see clearly that the validation
// layer is alive. vk-bootstrap's default_debug_callback writes to std::cout;
// this one writes to stderr with a clear [VK-VALIDATION] prefix so it
// interleaves with our other diagnostic prints. Returns VK_FALSE so the
// triggering call is NOT aborted (we want to keep running and catch more).
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
        (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)     ? "VALIDATION"  :
        (type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)    ? "PERFORMANCE" :
        (type & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT)        ? "GENERAL"     : "?";

    std::fprintf(stderr, "[VK-VALIDATION][%s][%s] %s\n",
                 sevStr, typeStr,
                 data && data->pMessage ? data->pMessage : "(no message)");
    std::fflush(stderr);
    return VK_FALSE;
}

}  // namespace

bool VulkanInstance::Initialize(const char* appName, bool enableValidation) {
    vkb::InstanceBuilder builder;
    builder.set_app_name(appName)
           .set_engine_name("PokeMotor2")
           .require_api_version(1, 3, 0);

    if (enableValidation) {
        builder.request_validation_layers(true)
               .set_debug_callback(&ValidationCallback)
               // Capture every severity — VERBOSE/INFO included so we can see
               // exactly what the layer is up to during the MSDF flash. If
               // this turns out to be spammy in steady state, drop VERBOSE.
               .set_debug_messenger_severity(
                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT    |
                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
               .set_debug_messenger_type(
                   VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT     |
                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT  |
                   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
               // Sync validation pillaría barriers faltantes / image-layout
               // transitions ausentes — exactamente la clase de bug que
               // produciría el flash MSDF si el atlas se lee en una layout
               // equivocada por un solo frame.
               .add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT)
               .add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT);
    }

    auto built = builder.build();
    if (!built) {
        std::fprintf(stderr, "[Vulkan] Instance build failed: %s\n",
                     built.error().message().c_str());
        if (enableValidation) {
            std::fprintf(stderr, "[Vulkan] Validation was requested — check that "
                         "the Vulkan SDK is installed and VK_LAYER_KHRONOS_validation "
                         "is available (run `vulkaninfo --summary`).\n");
        }
        return false;
    }
    m_instance = built.value();
    m_initialized = true;

    std::fprintf(stderr, "[Vulkan] Instance created (validation=%s)\n",
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

}  // namespace pokemotor::vk
