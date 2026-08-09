cmake_minimum_required(VERSION 3.20)
project(engine C CXX)   # C: tlsf (memory manager backend)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── MSVC conformance + encoding ─────────────────────────────────────────────
# /utf-8 is not optional here: this tree's comments are full of box-drawing and
# arrow characters (── → ⊕). Without it MSVC reads source as the local ANSI
# codepage, which at best floods C4819 and at worst mis-tokenises a comment when
# a multi-byte sequence ends in a byte that looks like a backslash.
# /permissive- + /Zc:* get MSVC to the standard semantics the code was written
# against (two-phase lookup, conforming preprocessor, correct __cplusplus).
# /bigobj: the heavier TUs (Assimp, Jolt, the cookers) exceed the default
# section limit in Debug.
if(MSVC)
    # /utf-8 goes into CMAKE_<LANG>_FLAGS, NOT add_compile_options, on purpose:
    # assimp guards its own charset flag with
    #     if(NOT CMAKE_CXX_FLAGS MATCHES "/utf-8") add_compile_options(/source-charset:utf-8)
    # so a flag added any other way is invisible to that check and assimp then
    # emits a flag MSVC rejects as incompatible with ours
    # ("D8016: '/utf-8' and '/source-charset:utf-8' ... incompatible"), which
    # killed the build inside assimp's vendored zlib. Setting it here is what
    # assimp is looking for. Both languages: the zlib TUs that failed are C.
    string(APPEND CMAKE_C_FLAGS   " /utf-8")
    string(APPEND CMAKE_CXX_FLAGS " /utf-8")

    # Language-agnostic.
    add_compile_options(/bigobj /Zc:preprocessor)
    # C++ only — MSVC rejects or ignores these on C TUs, and the tree has
    # several (tlsf, blake3, sqlite, zlib).
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/permissive->
                        $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)

    # Embed debug info in each .obj (/Z7) instead of a per-target .PDB (/Zi).
    # Two independent reasons, both load-bearing here:
    #  1. Parallel cl.exe processes writing one shared .PDB is a HARD ERROR
    #     ("C1041: cannot open program database ... please use /FS"), which is
    #     what stopped the Windows build at target 6 of 1638, inside a vendored
    #     library that overrides CMake's flags.
    #  2. sccache cannot cache /Zi compilations at all, so the Windows CI cache
    #     would never hit and every run would rebuild all 1638 targets.
    # cmake_minimum_required is 3.20, so CMP0141 is OLD and the debug-info flag
    # lives in CMAKE_<LANG>_FLAGS_<CONFIG> — rewrite it there rather than using
    # CMAKE_MSVC_DEBUG_INFORMATION_FORMAT, which needs 3.25.
    foreach(_cfg DEBUG RELWITHDEBINFO)
        foreach(_lang C CXX)
            string(REPLACE "/Zi" "/Z7"
                   CMAKE_${_lang}_FLAGS_${_cfg} "${CMAKE_${_lang}_FLAGS_${_cfg}}")
        endforeach()
    endforeach()
    # Safety net: a vendored target that re-adds /Zi itself still serializes its
    # PDB writes instead of failing the build.
    add_compile_options(/FS)
    # The engine's own code is warning-clean elsewhere; keep MSVC's noisiest
    # non-actionable warnings from burying real ones on the first port pass.
    add_compile_options(/wd4244 /wd4267 /wd4305)   # narrowing conversions
    # NOMINMAX / WIN32_LEAN_AND_MEAN globally: <windows.h> otherwise defines
    # min/max macros that break std::min/std::max and drags in winsock.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES
                            _CRT_SECURE_NO_WARNINGS)
endif()

# ── Shipping posture: source importers in or out ────────────────────────────
# ON  (default): dev tree — editor, cookers, Assimp/FBX import, all tools.
# OFF (shipping): a fully-cooked runtime. Assimp is not compiled OR linked
#   anywhere; dev-only targets (editor, engine_cook, engine_host, import
#   tools/tests) are skipped. engine_player is the intended consumer:
#     cmake -B build-ship -DENGINE_WITH_SOURCE_IMPORTERS=OFF
#     cmake --build build-ship --target engine_player
#   This is the "runtime never parses FBX" rule (cookers/problems.md) made
#   into a build fact instead of a convention.
option(ENGINE_WITH_SOURCE_IMPORTERS
    "Compile source-format import (Assimp) into the engine" ON)

# ── Optional target groups ───────────────────────────────────────────────────
# The core SDK must be buildable WITHOUT the editor, the CLI tools, or the
# samples. This exists for porting: on a platform where (say) a sample's
# linker flags or a tool's POSIX call is broken, an unconditional build stops
# at the first such failure and hides every real engine error behind it.
# Turning a group off converts "the build is broken" into a specific,
# incrementally fixable list — which is the entire point of the CI matrix.
#
#   cmake -B build-core -DENGINE_BUILD_EDITOR=OFF \
#                       -DENGINE_BUILD_TOOLS=OFF  \
#                       -DENGINE_BUILD_SAMPLES=OFF -DBUILD_TESTING=OFF
#
# All default ON: a normal dev build is unchanged. Tests use CTest's own
# standard BUILD_TESTING rather than inventing a parallel switch.
option(ENGINE_BUILD_EDITOR  "Build the ImGui editor application"   ON)
option(ENGINE_BUILD_TOOLS   "Build the CLI tools (cook/build/host/…)" ON)
option(ENGINE_BUILD_SAMPLES "Build the sample games and kits"      ON)

# ── Window / raw-input backend ───────────────────────────────────────────────
# GLFW is the default and fully supported. SDL3 is being brought up as the
# window + raw-input PROVIDER behind the engine's own input abstraction — the
# draw is its controller support (mapping database, hotplug, rumble, gyro),
# which is the one part of raw input not worth writing three times.
#
#   cmake -B build-sdl3 -DENGINE_WINDOW_BACKEND=sdl3
#
# Both backends stay compiling so the migration never breaks `main`. See
# src/runtime/platform/sdl3_platform.h for the current limits.
set(ENGINE_WINDOW_BACKEND "glfw" CACHE STRING
    "Window/input backend: glfw (default, complete) or sdl3 (in progress)")
set_property(CACHE ENGINE_WINDOW_BACKEND PROPERTY STRINGS glfw sdl3)
if(NOT ENGINE_WINDOW_BACKEND MATCHES "^(glfw|sdl3)$")
    message(FATAL_ERROR
        "ENGINE_WINDOW_BACKEND must be 'glfw' or 'sdl3', got "
        "'${ENGINE_WINDOW_BACKEND}'")
endif()
if(ENGINE_WINDOW_BACKEND STREQUAL "sdl3")
    # Prefer a vendored submodule if someone adds one; otherwise use an
    # installed SDL3 (brew/vcpkg/distro). Not vendored by default — a large
    # dependency shouldn't land in the tree before the backend is chosen.
    if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/SDL/CMakeLists.txt)
        add_subdirectory(third_party/SDL)
    else()
        find_package(SDL3 CONFIG QUIET)
        if(NOT SDL3_FOUND)
            message(FATAL_ERROR
                "ENGINE_WINDOW_BACKEND=sdl3 but SDL3 was not found.\n"
                "  macOS:  brew install sdl3\n"
                "  Linux:  apt install libsdl3-dev  (or build from source)\n"
                "  Windows: vcpkg install sdl3\n"
                "Or vendor it at third_party/SDL.")
        endif()
    endif()
endif()
message(STATUS "engine: window backend = ${ENGINE_WINDOW_BACKEND}")

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

# Rust modules. OFF unless explicitly enabled OR cargo happens to be present —
# Rust is a genuine new dependency for contributors and CI, so the tree must
# still configure without it. modules/net skips itself when cargo is missing,
# so this is safe to leave ON.
option(ENGINE_WITH_RUST "Build the Rust modules (engine_net) — needs cargo" ON)
if(ENGINE_WITH_RUST)
    add_subdirectory(modules/net)   # networking core behind a C ABI
endif()

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
# Same reason, second mechanism: flecs marks hot API functions
# FLECS_ALWAYS_INLINE with bodies in flecs.c — at -O2 Clang emits no
# out-of-line copy, so a RELEASE host lacks symbols a dlopen'd kit calls
# (verified: _ecs_children_w_rel absent from the Release player; kit load
# failed). Kits resolve flecs from the host, so every API function must be
# a real exported symbol. Cost: an inline hint on a few calls — irrelevant
# next to a working module ABI.
target_compile_definitions(flecs_static PUBLIC FLECS_NO_ALWAYS_INLINE)

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
if(ENGINE_WITH_SOURCE_IMPORTERS)
    add_subdirectory(third_party/assimp)
    # Assimp chews through multi-hundred-MB FBX/DAE during cooks; at Debug
    # -O0 the parse dominates cook time several-fold. It's a vendored parser
    # nobody steps through with the engine debugger — keep it optimized even
    # in Debug builds (same policy as the BC encoders in src/CMakeLists.txt).
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
        target_compile_options(assimp PRIVATE $<$<CONFIG:Debug>:-O2>)
    endif()
endif()

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
# ABSOLUTE, and it has to be: a relative path in an INTERFACE include
# directory is resolved against the CONSUMING target's source dir, not this
# one. That worked by luck for `editor` and broke the moment anything outside
# src/editor linked imgui (tests/editor_prefs_test.cpp: "imgui.h not found").
target_include_directories(
    imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ImGuizmo
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
# sample_minimal builds on every dev build deliberately: ~30 lines against the
# public API only, so it fails the moment the SDK surface stops being honest.
if(ENGINE_BUILD_SAMPLES)
    add_executable(sample_minimal samples/minimal_game/main.cpp)
    target_link_libraries(sample_minimal PRIVATE engine::runtime)
    set_target_properties(sample_minimal PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    add_subdirectory("sdk_sample_game(cpp)")
    add_subdirectory(samples/hot_reload_game)
endif()

# ── Install — `cmake --install build --prefix <sdk-dir>` ────────────────────
# Produces a relocatable SDK: lib/ (engine + bundled third-party archives),
# include/ (public umbrellas + engine headers + third-party headers), and a
# hand-written package config so consumers just find_package(EngineRuntime).
install(TARGETS engine_runtime engine_core assetlib bgfx bimg bx glfw
                flecs_static Jolt lua
        ARCHIVE DESTINATION lib)
if(ENGINE_WITH_SOURCE_IMPORTERS)
    # Dev SDK only — shipping posture has no assimp target and no cooker CLI.
    install(TARGETS assimp ARCHIVE DESTINATION lib)
    # The worker is part of the cook capability, not the tool group, so it
    # installs whenever cooking is compiled in (see src/CMakeLists.txt).
    install(TARGETS engine_cook_worker RUNTIME DESTINATION bin)
    if(ENGINE_BUILD_TOOLS)
        install(TARGETS engine_cook RUNTIME DESTINATION bin)
    endif()
endif()

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
set(_sdk_def_targets Jolt bx bgfx bimg flecs_static glfw lua assetlib)
if(ENGINE_WITH_SOURCE_IMPORTERS)
    list(APPEND _sdk_def_targets assimp)   # dev SDK only — no assimp when shipping
endif()
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
