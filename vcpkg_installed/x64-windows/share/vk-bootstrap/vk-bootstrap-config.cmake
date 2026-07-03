        
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was vk-bootstrap-config.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../" ABSOLUTE)

####################################################################################

        # Try to find Vulkan-Headers using find_package. Assume the package is available through either Vulkan-Headers or the older Vulkan
        # Package managers should have Vulkan-Headers be a dependency of this repo.
        find_package(VulkanHeaders CONFIG)
        if (NOT VulkanHeaders_FOUND)
            find_package(Vulkan)
            if (NOT Vulkan_FOUND)
                message(FATAL_ERROR "Unable to locate required dependency Vulkan::Headers!")
            endif()
        endif()
        include("${CMAKE_CURRENT_LIST_DIR}/vk-bootstrap-targets.cmake")
    
