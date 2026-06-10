cmake_minimum_required(VERSION 3.20)
project(engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Third-party dependencies (vendored) ─────────────────────────────────────

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

# ── JoltPhysics ──────────────────────────────────────────────────────────
set(JOLT_BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS      OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS       OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM     OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/JoltPhysics/Build)

add_subdirectory(third_party/lua)

# ImGui — editor-only UI library. The runtime never links this.
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

# ── Engine targets ──────────────────────────────────────────────────────────
#   engine_runtime  — static library: ECS, content, render, animation, audio,
#                     physics plugins, asset pipeline. The SDK a game links.
#   editor          — executable: ImGui editor application on top of the SDK.
add_subdirectory(src)
