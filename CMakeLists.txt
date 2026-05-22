cmake_minimum_required(VERSION 3.20)
project(engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "")
set(BGFX_BUILD_TOOLS ON CACHE BOOL "")
set(BGFX_BUILD_TOOLS_BIN2C ON CACHE BOOL "")
set(BGFX_BUILD_TOOLS_SHADER ON CACHE BOOL "")
set(BGFX_BUILD_TOOLS_GEOMETRY OFF CACHE BOOL "")
set(BGFX_BUILD_TOOLS_TEXTURE OFF CACHE BOOL "")
set(BGFX_INSTALL OFF CACHE BOOL "")
add_subdirectory(modules/assetlib)

add_subdirectory(third_party/bgfx.cmake)

# flecs - ECS framework
set(FLECS_STATIC ON CACHE BOOL "")
set(FLECS_SHARED OFF CACHE BOOL "")
set(FLECS_TESTS OFF CACHE BOOL "")
add_subdirectory(third_party/flecs)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "")
set(GLFW_BUILD_TESTS OFF CACHE BOOL "")
set(GLFW_BUILD_DOCS OFF CACHE BOOL "")
add_subdirectory(third_party/glfw)

add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/ImGuizmo/ImGuizmo.cpp
)
target_include_directories(
    imgui PUBLIC
    third_party/imgui
    third_party/ImGuizmo
)

include(third_party/bgfx.cmake/cmake/bgfxToolUtils.cmake)

set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
set(BGFX_SHADER_INCLUDES
    "${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake/bgfx/src"
    "${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake/bgfx/examples/common"
)

bgfx_compile_shaders(
    TYPE VERTEX
    SHADERS ${CMAKE_SOURCE_DIR}/shaders/vs_triangle.sc
    VARYING_DEF ${CMAKE_SOURCE_DIR}/shaders/varying.def.sc
    OUTPUT_DIR ${SHADER_OUTPUT_DIR}
    INCLUDE_DIRS ${BGFX_SHADER_INCLUDES}
    AS_HEADERS
)
bgfx_compile_shaders(
    TYPE FRAGMENT
    SHADERS ${CMAKE_SOURCE_DIR}/shaders/fs_triangle.sc
    VARYING_DEF ${CMAKE_SOURCE_DIR}/shaders/varying.def.sc
    OUTPUT_DIR ${SHADER_OUTPUT_DIR}
    INCLUDE_DIRS ${BGFX_SHADER_INCLUDES}
    AS_HEADERS
)


# ---- Assimp (multi-format mesh importer) ----
# Disable ALL importers/exporters by default, then opt-in only what we need.
# This keeps build time reasonable (7 formats vs 60+).
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT          ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL            OFF CACHE BOOL "" FORCE)
if(WIN32)
    set(ASSIMP_BUILD_ZLIB ON  CACHE BOOL "" FORCE)  # no system zlib on Windows
else()
    set(ASSIMP_BUILD_ZLIB OFF CACHE BOOL "" FORCE)  # macOS + Linux ship zlib
endif()
# Formats cgltf doesn't cover:
set(ASSIMP_BUILD_FBX_IMPORTER     ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER     ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_COLLADA_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_3DS_IMPORTER     ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_PLY_IMPORTER     ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_STL_IMPORTER     ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_BLEND_IMPORTER   ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/assimp)
add_executable(engine
    src/cookers/mesh_cooker.cpp
    src/main.cpp
    src/render/imgui_bgfx.cpp
    src/render/imgui_impl_glfw.cpp
    src/io/cgltf_impl.cpp
    src/io/gltf_importer.cpp
    src/io/stb_impl.cpp
    src/engine/runtime.cpp
    src/engine/async_loader.cpp
    src/io/assimp_importer.cpp
    shaders/vs_triangle.sc
    shaders/fs_triangle.sc
    shaders/varying.def.sc
)

set_source_files_properties(
    shaders/vs_triangle.sc
    shaders/fs_triangle.sc
    shaders/varying.def.sc
    PROPERTIES HEADER_FILE_ONLY TRUE
)

target_include_directories(
    engine PRIVATE
    third_party/nlohmann
    src
    src/core
    src/components
    src/render
    src/editor
    src/io
    third_party/cgltf
    third_party/stb
    "${SHADER_OUTPUT_DIR}"
)

# Disable WGSL embedded-shader branch since we don't generate WGSL bytecode
# (would require BGFX_CONFIG_RENDERER_WEBGPU and a Dawn/WebGPU build).
target_compile_definitions(engine PRIVATE
    BGFX_PLATFORM_SUPPORTS_WGSL=0
)

target_link_libraries(engine PRIVATE
    assetlib
    assimp
    bgfx bx bimg
    glfw
    imgui
    flecs_static
)

if(APPLE)
    target_link_libraries(engine PRIVATE
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework IOKit"
    )
endif()
