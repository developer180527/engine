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
#                     physics plugins, asset pipeline. The SDK a game links
#                     (alias: engine::runtime). API: include/engine/*.h
#   editor          — executable: ImGui editor application on top of the SDK.
#   engine_cook     — CLI asset cooker.
add_subdirectory(src)

# ── Samples — prove the SDK builds games without the editor ─────────────────
add_executable(sample_minimal samples/minimal_game/main.cpp)
target_link_libraries(sample_minimal PRIVATE engine::runtime)
set_target_properties(sample_minimal PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_subdirectory(sdk_sample_game)

# ── Install — `cmake --install build --prefix <sdk-dir>` ────────────────────
# Produces a relocatable SDK: lib/ (engine + bundled third-party archives),
# include/ (public umbrellas + engine headers + third-party headers), and a
# hand-written package config so consumers just find_package(EngineRuntime).
install(TARGETS engine_runtime engine_core assetlib assimp bgfx bimg bx glfw
                flecs_static Jolt lua
        ARCHIVE DESTINATION lib)
install(TARGETS engine_cook RUNTIME DESTINATION bin)

# Engine headers: public umbrellas + the src tree they re-export.
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY src/ DESTINATION include
        FILES_MATCHING PATTERN "*.h"
        PATTERN "editor" EXCLUDE)

# Third-party headers reachable from public engine headers.
install(DIRECTORY third_party/flecs/include/ DESTINATION include)
install(DIRECTORY third_party/bgfx.cmake/bx/include/ DESTINATION include)
install(DIRECTORY third_party/bgfx.cmake/bgfx/include/ DESTINATION include)
install(DIRECTORY third_party/glfw/include/ DESTINATION include)
install(DIRECTORY third_party/JoltPhysics/Jolt DESTINATION include
        FILES_MATCHING PATTERN "*.h" PATTERN "*.inl")
install(DIRECTORY third_party/nlohmann/ DESTINATION include
        FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY modules/assetlib/include/ DESTINATION include)
install(FILES third_party/miniaudio/miniaudio.h DESTINATION include)
# Lua headers go at the include root: lua.hpp quote-includes "lua.h", so
# they must live in the same directory (mirrors lua/src being on the path).
install(DIRECTORY third_party/lua/src/ DESTINATION include
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")

# Capture the bundled libraries' INTERFACE_COMPILE_DEFINITIONS at generate
# time. ABI-sensitive deps (Jolt especially) require consumers to compile
# with exactly the definitions the libraries were built with — the installed
# config replays them. Generator expressions resolve for this build's config,
# which matches the single set of archives we install.
set(_sdk_def_targets Jolt bx bgfx bimg flecs_static glfw assimp lua assetlib)
set(_sdk_defs_genex "")
foreach(_t ${_sdk_def_targets})
    string(APPEND _sdk_defs_genex "$<TARGET_PROPERTY:${_t},INTERFACE_COMPILE_DEFINITIONS>;")
endforeach()
file(GENERATE
    OUTPUT ${CMAKE_BINARY_DIR}/EngineRuntimeDefinitions.cmake
    CONTENT "set(ENGINE_BUNDLED_DEFINITIONS \"${_sdk_defs_genex}\")\n")
install(FILES ${CMAKE_BINARY_DIR}/EngineRuntimeDefinitions.cmake
        DESTINATION lib/cmake/EngineRuntime)

install(FILES cmake/EngineRuntimeConfig.cmake
        DESTINATION lib/cmake/EngineRuntime)
