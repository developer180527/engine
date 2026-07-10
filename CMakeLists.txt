cmake_minimum_required(VERSION 3.20)
project(engine C CXX)   # C: tlsf (memory manager backend)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Sanitizer lanes (tests/docs/engine-compiler-test-sheet.md §3) ───────────
# Whole-program instrumentation: applied before every subdirectory so vendored
# libraries are covered too (mixing instrumented/uninstrumented code hides
# bugs at the boundary). Use a SEPARATE build dir per lane:
#   cmake -B build-asan -DENGINE_SANITIZE=address,undefined
#   cmake -B build-tsan -DENGINE_SANITIZE=thread     (can't combine with ASan)
set(ENGINE_SANITIZE "" CACHE STRING
    "Sanitizers to instrument with (address,undefined | thread | leak)")
if(ENGINE_SANITIZE)
    add_compile_options(-fsanitize=${ENGINE_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${ENGINE_SANITIZE})
    message(STATUS "engine: sanitizer lane active: ${ENGINE_SANITIZE}")
    # Heap sanitizers need allocations to keep SYSTEM provenance — the tagged
    # TLSF pools would hide use-after-free/overflow inside recycled pool
    # memory. mem_counters.cpp documents this switch; flip it automatically.
    if(ENGINE_SANITIZE MATCHES "address|leak|memory")
        add_compile_definitions(ENGINE_MEM_ROUTE=0)
        message(STATUS "engine: ENGINE_MEM_ROUTE=0 (heap sanitizer owns allocations)")
    endif()
endif()

# ── Third-party dependencies (vendored) ─────────────────────────────────────

set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "")
set(BGFX_BUILD_TOOLS ON CACHE BOOL "")
set(BGFX_BUILD_TOOLS_BIN2C ON CACHE BOOL "")
set(BGFX_BUILD_TOOLS_SHADER ON CACHE BOOL "")
set(BGFX_BUILD_TOOLS_GEOMETRY OFF CACHE BOOL "")
set(BGFX_BUILD_TOOLS_TEXTURE OFF CACHE BOOL "")
set(BGFX_INSTALL OFF CACHE BOOL "")
add_subdirectory(modules/assetlib)
add_subdirectory(modules/hid)       # raw-input module (devices + timestamped events)

add_subdirectory(third_party/bgfx.cmake)

# Sanitizer lanes instrument the whole tree — but bgfx's HOST BUILD TOOLS
# (shaderc + its compiler stack) run at build time and abort under ASan on a
# vendored stack overflow inside glslang, killing the build before any engine
# code runs. We're testing the engine, not bgfx's offline shader compiler:
# strip instrumentation from tool-only targets (the engine links bgfx/bx/bimg,
# never these).
if(ENGINE_SANITIZE)
    foreach(t bin2c fcpp glsl-optimizer glslang shaderc spirv-cross spirv-opt)
        if(TARGET ${t})
            # Compile-time strip only: the tools still link instrumented bx,
            # so the sanitizer runtime must stay in their link line.
            target_compile_options(${t} PRIVATE -fno-sanitize=all)
        endif()
    endforeach()
endif()

# flecs - ECS framework
set(FLECS_STATIC ON CACHE BOOL "")
set(FLECS_SHARED OFF CACHE BOOL "")
set(FLECS_TESTS OFF CACHE BOOL "")
add_subdirectory(third_party/flecs)
# flecs defaults to hidden visibility, which keeps its symbols out of the
# engine_host export table — hot-reloaded game modules resolve flecs from
# the host at load time (-undefined dynamic_lookup), so they must be visible.
set_target_properties(flecs_static PROPERTIES
    C_VISIBILITY_PRESET   default
    CXX_VISIBILITY_PRESET default)

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
# RTTI on: the engine derives from Jolt polymorphic classes with out-of-line
# key functions (JobSystemWithBarrier -> JoltJobsAdapter); with Jolt's default
# -fno-rtti the base typeinfo is never emitted and the link fails.
set(CPP_RTTI_ENABLED         ON  CACHE BOOL "" FORCE)
add_subdirectory(third_party/JoltPhysics/Build)

# ── recastnavigation — navmesh (Recast build + Detour runtime query) ──────
# Engine-owned navigation infrastructure (like Jolt for physics): the NavService
# bakes a navmesh from static collision geometry and answers path queries; kits
# consume it only through the nav C-API group. Demo/tests/examples off.
set(RECASTNAVIGATION_DEMO     OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS    OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/recastnavigation)

add_subdirectory(third_party/lua)

# ── ozz-animation — skeletal animation backbone ──────────────────────────
# Runtime (sampling/blending/local-to-model, SoA SIMD, allocation-free) +
# offline builders (RawSkeleton/RawAnimation -> optimized runtime structures,
# used by the Assimp import bridge and later the animation cooker). Tools,
# samples and importers are disabled — Assimp remains our only file importer.
set(ozz_build_tools    OFF CACHE BOOL "" FORCE)
set(ozz_build_fbx      OFF CACHE BOOL "" FORCE)
set(ozz_build_gltf     OFF CACHE BOOL "" FORCE)
set(ozz_build_samples  OFF CACHE BOOL "" FORCE)
set(ozz_build_howtos   OFF CACHE BOOL "" FORCE)
set(ozz_build_tests    OFF CACHE BOOL "" FORCE)
set(ozz_build_postfix  OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/ozz-animation)

# ── enkiTS — task scheduler backing engine::jobs ──────────────────────────
# Built directly (imgui-style) — its own CMakeLists declares a minimum too
# old for CMake 4. The engine never includes enkiTS headers outside
# src/runtime/jobs/ — the facade contract (jobs.h) is written so the backend
# can be swapped for FiberTaskingLib without touching call sites.
add_library(enkiTS STATIC third_party/enkiTS/src/TaskScheduler.cpp)
target_include_directories(enkiTS PUBLIC third_party/enkiTS/src)
target_compile_features(enkiTS PUBLIC cxx_std_11)
find_package(Threads REQUIRED)
target_link_libraries(enkiTS PUBLIC Threads::Threads)

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

# ── Tests — CTest-registered gauntlets + stress suite (tests/CMakeLists.txt) ─
# Lanes: ctest -L unit (fast) / -L stress (torture) / -L asset (needs content).
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# ── Samples — prove the SDK builds games without the editor ─────────────────
add_executable(sample_minimal samples/minimal_game/main.cpp)
target_link_libraries(sample_minimal PRIVATE engine::runtime)
set_target_properties(sample_minimal PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_subdirectory("sdk_sample_game(cpp)")
add_subdirectory(samples/hot_reload_game)

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
