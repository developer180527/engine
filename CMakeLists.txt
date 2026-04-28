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
add_subdirectory(third_party/bgfx.cmake)

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
)
target_include_directories(imgui PUBLIC third_party/imgui)

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

add_executable(engine
    src/main.cpp
    src/render/imgui_bgfx.cpp
    src/render/imgui_impl_glfw.cpp
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

target_include_directories(engine PRIVATE
    src
    src/render
    "${SHADER_OUTPUT_DIR}"
)

# Disable WGSL embedded-shader branch since we don't generate WGSL bytecode
# (would require BGFX_CONFIG_RENDERER_WEBGPU and a Dawn/WebGPU build).
target_compile_definitions(engine PRIVATE
    BGFX_PLATFORM_SUPPORTS_WGSL=0
)

target_link_libraries(engine PRIVATE
    bgfx bx bimg
    glfw
    imgui
)

if(APPLE)
    target_link_libraries(engine PRIVATE
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework IOKit"
    )
endif()
