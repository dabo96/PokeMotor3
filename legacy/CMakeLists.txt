cmake_minimum_required(VERSION 3.25)
project(PokeMotor VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point CMake to vcpkg-installed packages
set(VCPKG_INSTALLED "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows")
list(APPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED}/share")
# For FindZLIB and other built-in find modules that need include/lib paths directly
list(APPEND CMAKE_INCLUDE_PATH "${VCPKG_INSTALLED}/include")
list(APPEND CMAKE_LIBRARY_PATH "${VCPKG_INSTALLED}/lib")

# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------

# GLM
add_subdirectory(Libraries/glm)

# SDL3 — found via vcpkg
find_package(SDL3 CONFIG REQUIRED)

# Lua — manual import (vcpkg doesn't ship a CMake config for lua)
add_library(lua SHARED IMPORTED)
set_target_properties(lua PROPERTIES
    IMPORTED_IMPLIB "${VCPKG_INSTALLED}/lib/lua.lib"
    IMPORTED_LOCATION "${VCPKG_INSTALLED}/bin/lua.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${VCPKG_INSTALLED}/include"
)

# sol2 (header-only, depends on lua)
find_package(sol2 CONFIG REQUIRED)

# json (nlohmann_json)
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/json")

# stb_image
add_library(stb_library INTERFACE)
target_include_directories(stb_library INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/stb_image")

# stb_vorbis (OGG Vorbis decoder)
add_library(stb_vorbis STATIC Libraries/stb_vorbis/stb_vorbis_impl.c)
target_include_directories(stb_vorbis PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/stb_vorbis")

# FluentUI (WinUI3) — Vulkan-only build, no OpenGL backend.
set(FLUENTUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FLUENTUI_OPENGL_BACKEND OFF CACHE BOOL "" FORCE)
add_subdirectory(Libraries/WinUI3)

# Vulkan renderer dependencies
find_package(Vulkan REQUIRED COMPONENTS glslangValidator)
find_package(vk-bootstrap CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(glslang CONFIG REQUIRED)

if(NOT Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    message(FATAL_ERROR
        "glslangValidator not found. Install the LunarG Vulkan SDK from "
        "https://vulkan.lunarg.com/sdk/home#windows and re-run cmake "
        "(the SDK sets the VULKAN_SDK environment variable which CMake's "
        "FindVulkan module reads to locate the shader tools).")
endif()

# ------------------------------------------------------------------------------
# Shader compilation (GLSL -> SPIR-V)
# ------------------------------------------------------------------------------

set(VK_SHADERS_SPV "")
function(pokemotor_compile_glsl SHADER_FILE)
    set(GLSL_IN  "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/GLSL/${SHADER_FILE}")
    set(SPV_OUT  "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/SPV/${SHADER_FILE}.spv")
    add_custom_command(
        OUTPUT "${SPV_OUT}"
        COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V "${GLSL_IN}" -o "${SPV_OUT}"
        DEPENDS "${GLSL_IN}"
        COMMENT "glslangValidator ${SHADER_FILE} -> SPV"
        VERBATIM
    )
    set(VK_SHADERS_SPV "${VK_SHADERS_SPV};${SPV_OUT}" PARENT_SCOPE)
endfunction()

pokemotor_compile_glsl(geometry.vert)
pokemotor_compile_glsl(geometry.frag)
pokemotor_compile_glsl(fullscreen.vert)
pokemotor_compile_glsl(lighting.frag)
pokemotor_compile_glsl(hiz_generate.comp)
pokemotor_compile_glsl(hiz_downsample.comp)
pokemotor_compile_glsl(shadow.vert)
pokemotor_compile_glsl(env_procedural.comp)
pokemotor_compile_glsl(equirect_to_cubemap.comp)
pokemotor_compile_glsl(ibl_irradiance.comp)
pokemotor_compile_glsl(ibl_prefilter.comp)
pokemotor_compile_glsl(ibl_brdflut.comp)
pokemotor_compile_glsl(taa.frag)
pokemotor_compile_glsl(ssr_ray_sort.comp)
pokemotor_compile_glsl(ssr_ray_prefix.comp)
pokemotor_compile_glsl(ssr_packet_trace.comp)
pokemotor_compile_glsl(ssr_upsample.comp)
pokemotor_compile_glsl(ssr_svgf_temporal.comp)
pokemotor_compile_glsl(ssr_svgf_spatial.comp)
pokemotor_compile_glsl(histogram_build.comp)
pokemotor_compile_glsl(histogram_average.comp)
pokemotor_compile_glsl(tonemap.frag)
pokemotor_compile_glsl(cas_sharpen.frag)
pokemotor_compile_glsl(bloom_prefilter.comp)
pokemotor_compile_glsl(bloom_downsample.comp)
pokemotor_compile_glsl(bloom_upsample.comp)
pokemotor_compile_glsl(vol_fog.comp)
pokemotor_compile_glsl(particle_update.comp)
pokemotor_compile_glsl(particle.vert)
pokemotor_compile_glsl(particle.frag)
pokemotor_compile_glsl(sprite_gbuffer.vert)
pokemotor_compile_glsl(sprite_gbuffer.frag)
pokemotor_compile_glsl(sprite_shadow.vert)
pokemotor_compile_glsl(sprite_shadow.frag)
pokemotor_compile_glsl(debug_line.vert)
pokemotor_compile_glsl(debug_line.frag)
pokemotor_compile_glsl(ui.vert)
pokemotor_compile_glsl(ui_basic.frag)
pokemotor_compile_glsl(ui_text.frag)
pokemotor_compile_glsl(ui_msdf.frag)
pokemotor_compile_glsl(ui_image.frag)

# ------------------------------------------------------------------------------
# PokeMotor — Vulkan executable
# ------------------------------------------------------------------------------

file(GLOB VK_RENDERER_SOURCES CONFIGURE_DEPENDS
    "src/Rendering/Vulkan/Core/*.cpp"
    "src/Rendering/Vulkan/Core/*.h"
    "src/Rendering/Vulkan/Resources/*.cpp"
    "src/Rendering/Vulkan/Resources/*.h"
    "src/Rendering/Vulkan/UI/*.cpp"
    "src/Rendering/Vulkan/UI/*.h"
)

add_executable(PokeMotor
    src/main.cpp
    ${VK_RENDERER_SOURCES}
    Core/Camera.cpp
    Core/Camera.h
    Core/ECS/Registry.cpp
    Core/ECS/Registry.h
    Core/ECS/CommonComponents.cpp
    Core/ECS/CommonComponents.h
    Core/ECS/TransformSystem.cpp
    Core/ECS/TransformSystem.h
    Core/ECS/Entity.h
    Core/ECS/Component.h
    Core/ECS/Systems.h
    Core/ECS/World.h
    Core/Physics/Shapes.cpp
    Core/Physics/Shapes.h
    Core/Physics/TerrainCollider.cpp
    Core/Physics/TerrainCollider.h
    Core/Physics/CollisionSystem.cpp
    Core/Physics/CollisionSystem.h
    Core/Scripting/ScriptComponent.h
    Core/Scripting/ScriptEngine.cpp
    Core/Scripting/ScriptEngine.h
    Core/Scripting/ScriptSystem.cpp
    Core/Scripting/ScriptSystem.h
    Core/Audio/AudioClip.cpp
    Core/Audio/AudioClip.h
    Core/Audio/AudioManager.cpp
    Core/Audio/AudioManager.h
)

# vk_shaders depends on PokeMotor's output dir (generator expression). Has
# to be declared AFTER add_executable so $<TARGET_FILE_DIR:PokeMotor> resolves.
# ALL keeps the target part of the default build set; the COPY command runs
# every build (cheap copy_if_different) so a shader-only edit lands in the
# output directory even when the linker has nothing to do.
add_custom_target(vk_shaders ALL
    DEPENDS ${VK_SHADERS_SPV}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/SPV"
        "$<TARGET_FILE_DIR:PokeMotor>/Shaders/SPV"
)
add_dependencies(PokeMotor vk_shaders)

target_link_libraries(PokeMotor PRIVATE
    SDL3::SDL3
    Vulkan::Vulkan
    vk-bootstrap::vk-bootstrap
    GPUOpen::VulkanMemoryAllocator
    glm::glm
    stb_library
    stb_vorbis
    nlohmann_json
    sol2::sol2
    lua
    FluentUI::FluentUI
)

target_include_directories(PokeMotor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Rendering/Vulkan/Core"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Rendering/Vulkan/Resources"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Rendering/Vulkan/UI"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/ECS"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Audio"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Scripting"
    "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/WinUI3/include"
)

target_compile_definitions(PokeMotor PRIVATE
    $<$<CONFIG:Debug>:POKEMOTOR_VK_VALIDATION=1>
    $<$<CONFIG:Debug>:SOL_ALL_SAFETIES_ON=1>
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
)

add_custom_command(TARGET PokeMotor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Textures/Demo"
        "$<TARGET_FILE_DIR:PokeMotor>/Assets/Textures/Demo"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Models/Demo"
        "$<TARGET_FILE_DIR:PokeMotor>/Assets/Models/Demo"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Models/Sprites"
        "$<TARGET_FILE_DIR:PokeMotor>/Assets/Models/Sprites"
    # Lua scripts — copied so the runtime (cwd = exe dir) finds them. Scripts
    # created/edited in-engine live here too (cwd-relative Assets/Scripts).
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Scripts"
        "$<TARGET_FILE_DIR:PokeMotor>/Assets/Scripts"
    # FluentUI assets — MSDF atlas (atlas.png/json) used by Renderer's
    # InitializeDefaultFont + Lucide icon font (lucide.ttf) loaded explicitly
    # from main.cpp. Both expected at <exe>/assets/fonts/.
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/WinUI3/assets"
        "$<TARGET_FILE_DIR:PokeMotor>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/SDL3-3.2.28/lib/x64/SDL3.dll"
        "${VCPKG_INSTALLED}/bin/lua.dll"
        $<TARGET_FILE_DIR:PokeMotor>
)
