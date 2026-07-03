cmake_minimum_required(VERSION 3.25)
project(PokeMotor VERSION 0.1 LANGUAGES CXX)

# ------------------------------------------------------------------------------
# Motor Pokémon — build incremental por fases (ver MotorGrafico_IndiceMaestro.md).
# Fase 0 — Cimientos: SDL + GLM + Core/Input/Platform.
# Fase 1 — Backend Vulkan: vk-bootstrap + VMA + Renderer (sync + triángulo).
# El código del motor anterior está preservado en legacy/.
# ------------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VCPKG_INSTALLED "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows")
list(APPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED}/share")
list(APPEND CMAKE_INCLUDE_PATH "${VCPKG_INSTALLED}/include")
list(APPEND CMAKE_LIBRARY_PATH "${VCPKG_INSTALLED}/lib")

# --- Dependencias ---
add_subdirectory(Libraries/glm)                 # GLM (math)
find_package(SDL3 CONFIG REQUIRED)              # ventana + input
find_package(Vulkan REQUIRED COMPONENTS glslangValidator)
find_package(vk-bootstrap CONFIG REQUIRED)     # init Vulkan
find_package(VulkanMemoryAllocator CONFIG REQUIRED)  # VMA

# Lua (vcpkg no trae CMake config: import manual) + sol2 (header-only, depende de lua).
add_library(lua SHARED IMPORTED)
set_target_properties(lua PROPERTIES
    IMPORTED_IMPLIB   "${VCPKG_INSTALLED}/lib/lua.lib"
    IMPORTED_LOCATION "${VCPKG_INSTALLED}/bin/lua.dll")
find_package(sol2 CONFIG REQUIRED)

# FluentUI (editor UI, modo Vulkan compartido). Sin ejemplos ni tests.
set(FLUENTUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FLUENTUI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
add_subdirectory(Libraries/WinUI3)

if(NOT Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    message(FATAL_ERROR
        "glslangValidator no encontrado. Instala el Vulkan SDK de LunarG "
        "(https://vulkan.lunarg.com) y re-ejecuta cmake.")
endif()

# ------------------------------------------------------------------------------
# Compilación de shaders GLSL -> SPIR-V
# ------------------------------------------------------------------------------
set(VK_SHADERS_SPV "")
function(pokemotor_compile_glsl SHADER_FILE)
    set(GLSL_IN "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/GLSL/${SHADER_FILE}")
    set(SPV_OUT "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/SPV/${SHADER_FILE}.spv")
    add_custom_command(
        OUTPUT "${SPV_OUT}"
        COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V "${GLSL_IN}" -o "${SPV_OUT}"
        DEPENDS "${GLSL_IN}"
        COMMENT "glslang ${SHADER_FILE} -> SPV"
        VERBATIM)
    set(VK_SHADERS_SPV "${VK_SHADERS_SPV};${SPV_OUT}" PARENT_SCOPE)
endfunction()

pokemotor_compile_glsl(mesh.vert)
pokemotor_compile_glsl(mesh.frag)
pokemotor_compile_glsl(shadow.vert)
pokemotor_compile_glsl(sprite.vert)
pokemotor_compile_glsl(sprite.frag)
pokemotor_compile_glsl(fullscreen.vert)
pokemotor_compile_glsl(tonemap.frag)
pokemotor_compile_glsl(bloom_prefilter.frag)
pokemotor_compile_glsl(bloom_blur.frag)
pokemotor_compile_glsl(dof.frag)
pokemotor_compile_glsl(sprite2d.vert)
pokemotor_compile_glsl(sprite2d.frag)
pokemotor_compile_glsl(text2d.vert)
pokemotor_compile_glsl(text2d.frag)
pokemotor_compile_glsl(upscale.frag)

# ------------------------------------------------------------------------------
# Ejecutable
# ------------------------------------------------------------------------------
add_executable(PokeMotor
    main.cpp
    Engine/Engine.cpp
    Editor/EditorUI.cpp
    Editor/AssetBrowser.cpp
    Editor/ToolWindow.cpp
    Editor/TileEditorUI.cpp
    Game/Database.cpp
    Game/GameStack.cpp
    Game/SceneManager.cpp
    Game/TileMap.cpp
    Game/TileSet.cpp
    Game/OverworldMode.cpp
    Game/MenuMode.cpp
    Game/DialogueMode.cpp
    Game/EventRunner.cpp
    Game/BattleMode.cpp
    Assets/AssetManager.cpp
    # Assets/ModelLoader.cpp — camino 3D pausado (no usa el ECS nuevo); se re-cablea al retomar 3D
    Scene/Scene.cpp
    Scene/SpriteRenderSystem.cpp
    UI/UIRenderer.cpp
    UI/UIInput.cpp
    UI/DialogueBox.cpp
    Core/Log.cpp
    Core/Project.cpp
    Core/Scripting/LuaVM.cpp
    Core/Scripting/ScriptSystem.cpp
    Platform/Window.cpp
    Input/Input.cpp
    Input/ActionMap.cpp
    Renderer/Renderer.cpp
    Renderer/2D/SpriteBatch.cpp
    Renderer/2D/TextBatch.cpp
    Renderer/2D/Animation.cpp
    Renderer/Text/MsdfFont.cpp
    Renderer/Graph/RenderGraph.cpp
    Renderer/Camera/OrthographicCamera.cpp
    Renderer/Camera/PerspectiveCamera.cpp
    Renderer/Vulkan/VulkanInstance.cpp
    Renderer/Vulkan/VulkanDevice.cpp
    Renderer/Vulkan/VulkanSwapchain.cpp
    Renderer/Vulkan/VulkanAllocator.cpp
    Renderer/Vulkan/VulkanContext.cpp
    Renderer/Vulkan/BufferVk.cpp
    Renderer/Vulkan/ImageVk.cpp
    Renderer/Vulkan/Texture.cpp
    Renderer/Vulkan/MeshVk.cpp
    Renderer/Vulkan/GltfLoader.cpp
)

target_include_directories(PokeMotor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"                    # #include "Core/Log.h", etc.
    "${CMAKE_CURRENT_SOURCE_DIR}/Renderer/Vulkan"    # includes same-dir del backend
    "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/json"     # nlohmann json.hpp (glTF)
    "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/WinUI3/include"  # FluentGUI.h, etc.
    "${VCPKG_INSTALLED}/include"                     # lua.h / sol/sol.hpp
)

target_link_libraries(PokeMotor PRIVATE
    SDL3::SDL3
    Vulkan::Vulkan
    vk-bootstrap::vk-bootstrap
    GPUOpen::VulkanMemoryAllocator
    glm::glm
    FluentUI::FluentUI
    sol2::sol2
    lua
)

target_compile_definitions(PokeMotor PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Debug>:POKEMOTOR_VK_VALIDATION=1>
    # En Debug, los scripts se cargan desde el árbol de fuentes (hot-reload sobre el
    # .lua que editas, no la copia del build). En Release se usa la copia junto al exe.
    $<$<CONFIG:Debug>:POKEMOTOR_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}">
    ENGINE_EDITOR=1
)

# Compila shaders y copia el SPV junto al exe (declarado DESPUÉS de add_executable
# para que $<TARGET_FILE_DIR:PokeMotor> resuelva).
add_custom_target(vk_shaders ALL
    DEPENDS ${VK_SHADERS_SPV}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/SPV"
        "$<TARGET_FILE_DIR:PokeMotor>/Shaders/SPV")
add_dependencies(PokeMotor vk_shaders)

# SDL3.dll + assets junto al exe.
add_custom_command(TARGET PokeMotor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/SDL3-3.2.28/lib/x64/SDL3.dll"
        $<TARGET_FILE_DIR:PokeMotor>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_INSTALLED}/bin/lua.dll"
        $<TARGET_FILE_DIR:PokeMotor>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Models/Demo"
        "$<TARGET_FILE_DIR:PokeMotor>/Assets/Models/Demo"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Models/Sprites"
        "$<TARGET_FILE_DIR:PokeMotor>/Assets/Models/Sprites"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Data"
        "$<TARGET_FILE_DIR:PokeMotor>/Assets/Data"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Textures"
        "$<TARGET_FILE_DIR:PokeMotor>/Assets/Textures"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Scripts"
        "$<TARGET_FILE_DIR:PokeMotor>/Assets/Scripts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Fonts"
        "$<TARGET_FILE_DIR:PokeMotor>/Assets/Fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/WinUI3/assets"
        "$<TARGET_FILE_DIR:PokeMotor>/assets")
