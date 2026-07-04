# ── engine_runtime — the engine SDK as a static library ─────────────────────
# Everything a game needs WITHOUT the editor: ECS world + content registries,
# renderer (bgfx), animation, async asset loading, importers, cook pipeline,
# audio/physics/scripting plugin backends. No ImGui, no editor code.

include(${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake/cmake/bgfxToolUtils.cmake)

# ── Shaders — compiled to embeddable headers, consumed by the renderer ──────
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
set(BGFX_SHADER_INCLUDES
    "${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake/bgfx/src"
    "${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake/bgfx/examples/common"
)

set(ENGINE_VERTEX_SHADERS
    ${CMAKE_SOURCE_DIR}/shaders/vs_triangle.sc
    ${CMAKE_SOURCE_DIR}/shaders/vs_shadow.sc
    ${CMAKE_SOURCE_DIR}/shaders/vs_skinned.sc
    ${CMAKE_SOURCE_DIR}/shaders/vs_shadow_skinned.sc
    ${CMAKE_SOURCE_DIR}/shaders/vs_line.sc
)
set(ENGINE_FRAGMENT_SHADERS
    ${CMAKE_SOURCE_DIR}/shaders/fs_triangle.sc
    ${CMAKE_SOURCE_DIR}/shaders/fs_shadow.sc
    ${CMAKE_SOURCE_DIR}/shaders/fs_line.sc
)

foreach(SHADER ${ENGINE_VERTEX_SHADERS})
    bgfx_compile_shaders(
        TYPE VERTEX
        SHADERS ${SHADER}
        VARYING_DEF ${CMAKE_SOURCE_DIR}/shaders/varying.def.sc
        OUTPUT_DIR ${SHADER_OUTPUT_DIR}
        INCLUDE_DIRS ${BGFX_SHADER_INCLUDES}
        AS_HEADERS
    )
endforeach()
foreach(SHADER ${ENGINE_FRAGMENT_SHADERS})
    bgfx_compile_shaders(
        TYPE FRAGMENT
        SHADERS ${SHADER}
        VARYING_DEF ${CMAKE_SOURCE_DIR}/shaders/varying.def.sc
        OUTPUT_DIR ${SHADER_OUTPUT_DIR}
        INCLUDE_DIRS ${BGFX_SHADER_INCLUDES}
        AS_HEADERS
    )
endforeach()

set(ENGINE_SHADER_SOURCES
    ${ENGINE_VERTEX_SHADERS}
    ${ENGINE_FRAGMENT_SHADERS}
    ${CMAKE_SOURCE_DIR}/shaders/varying.def.sc
)

# ── engine_core — GPU-free layer ─────────────────────────────────────────────
# Pure-CPU code: asset cooking, single-header library implementations. No
# bgfx, no GLFW, no window — links into CLI tools (engine_cook) and servers
# without dragging in graphics or platform libraries.
add_library(engine_core STATIC
    # Asset cook pipeline (Assimp/stb in, cooked binaries out)
    assets/cookers/mesh_cooker.cpp
    assets/cookers/scene_cooker.cpp
    assets/cookers/texture_cooker.cpp
    assets/cookers/cook_service.cpp

    # Project management — scaffolding new projects (canonical v2 layout)
    project/project_scaffold.cpp

    # Single-header library implementation TUs (CPU decode only)
    assets/importers/stb_impl.cpp
    assets/importers/cgltf_impl.cpp

    # Memory manager (2MB blocks + TLSF tagged heaps) and the counting/routing
    # global new/delete that feeds it
    core/memory/mem.cpp
    core/mem_counters.cpp
    ${CMAKE_SOURCE_DIR}/third_party/tlsf/tlsf.c
)

target_include_directories(engine_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/components
    ${CMAKE_CURRENT_SOURCE_DIR}/render
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann
    ${CMAKE_SOURCE_DIR}/third_party/cgltf
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${CMAKE_SOURCE_DIR}/third_party/tlsf
    ${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake/bx/include   # bx math (skeleton types in the cooker)
)

target_link_libraries(engine_core PUBLIC
    assetlib
    assimp
    ozz_animation ozz_animation_offline   # skinned-mesh cooker (skeleton/clip archives)
)
target_compile_definitions(engine_core PUBLIC
    BX_CONFIG_DEBUG=$<IF:$<CONFIG:Debug>,1,0>   # bx headers (cooker skeleton types)
)

add_library(engine::core ALIAS engine_core)

# ── engine_runtime — the full game SDK ───────────────────────────────────────
# Public API surface: include/engine/*.h umbrella headers. Everything under
# src/ is implementation detail unless re-exported there.
add_library(engine_runtime STATIC
    # Runtime core — platform, frame loop, renderer, services
    runtime/runtime.cpp
    runtime/platform/glfw_platform.cpp
    render/renderer.cpp
    runtime/services/async_loader.cpp
    runtime/services/asset_service.cpp
    runtime/services/scene_service.cpp
    runtime/services/nav_service.cpp
    runtime/scripting/engine_api.cpp
    runtime/scripting/engine_api_table.cpp
    runtime/jobs/jobs_enkits.cpp
    runtime/input/input_manager.cpp

    # Source-format importers (produce GPU meshes — main thread)
    assets/importers/gltf_importer.cpp
    assets/importers/assimp_importer.cpp

    # Cooked-binary loaders
    assets/loaders/mesh_loader.cpp

    # Plugin backends
    plugins/audio_impl.cpp

    # Shaders (HEADER_FILE_ONLY — listed to trigger compilation)
    ${ENGINE_SHADER_SOURCES}
)

set_source_files_properties(
    ${ENGINE_SHADER_SOURCES}
    PROPERTIES HEADER_FILE_ONLY TRUE
)

target_include_directories(engine_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/JoltPhysics
    ${CMAKE_SOURCE_DIR}/third_party/miniaudio
    "${SHADER_OUTPUT_DIR}"
)

# Disable WGSL embedded-shader branch since we don't generate WGSL bytecode
# (would require BGFX_CONFIG_RENDERER_WEBGPU and a Dawn/WebGPU build).
target_compile_definitions(engine_runtime PUBLIC
    BGFX_PLATFORM_SUPPORTS_WGSL=0
)

target_link_libraries(engine_runtime PUBLIC
    engine_core
    bgfx bx bimg
    glfw
    flecs_static
    Jolt
    lua
    ozz_animation ozz_animation_offline   # skeletal animation backbone (+ import-time builders)
    enkiTS                                # task scheduler behind engine::jobs (facade-only include)
    hid                                   # raw-input module (modules/hid submodule)
    Recast Detour                         # navmesh build + runtime path query (NavService)
)

if(APPLE)
    target_link_libraries(engine_runtime PUBLIC
        "-framework Cocoa"
        "-framework QuartzCore"
        "-framework Metal"
        "-framework IOKit"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreFoundation"
    )
elseif(WIN32)
    target_link_libraries(engine_runtime PUBLIC
        gdi32
        shell32
        user32
        winmm          # timeGetTime — used by GLFW + miniaudio
    )
elseif(UNIX)
    # X11 + Wayland display servers; PulseAudio or ALSA for miniaudio.
    # pkg-config resolves the right paths per distro.
    find_package(PkgConfig REQUIRED)
    find_package(Threads REQUIRED)

    # X11 — still needed even on Wayland-primary distros (XWayland fallback)
    find_package(X11 REQUIRED)
    target_link_libraries(engine_runtime PUBLIC ${X11_LIBRARIES})
    target_include_directories(engine_runtime PUBLIC ${X11_INCLUDE_DIR})

    target_link_libraries(engine_runtime PUBLIC
        Threads::Threads
        dl              # dlopen — used by Vulkan loader + bgfx
        ${CMAKE_DL_LIBS}
    )
endif()

# Namespaced alias — what consumers link:
#   target_link_libraries(my_game PRIVATE engine::runtime)
add_library(engine::runtime ALIAS engine_runtime)

# ── engine::headers — include dirs + defines, ZERO libraries ─────────────────
# For hot-reloadable game modules (shared libs loaded by engine_host): they
# compile against engine headers but resolve every symbol from the host
# process at load time. Linking engine_runtime statically into a module would
# duplicate engine state — never do it.
add_library(engine_headers INTERFACE)
target_include_directories(engine_headers INTERFACE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/components
    ${CMAKE_CURRENT_SOURCE_DIR}/render
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann
    ${CMAKE_SOURCE_DIR}/third_party/flecs/include
    ${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake/bx/include
    ${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake/bgfx/include
    ${CMAKE_SOURCE_DIR}/modules/assetlib/include
)
target_compile_definitions(engine_headers INTERFACE
    BGFX_PLATFORM_SUPPORTS_WGSL=0
    BX_CONFIG_DEBUG=$<IF:$<CONFIG:Debug>,1,0>
)
target_compile_features(engine_headers INTERFACE cxx_std_20)
add_library(engine::headers ALIAS engine_headers)

add_subdirectory(editor)

# ── engine_cook — CLI asset cooker ───────────────────────────────────────────
# Cooks a project's assets without the editor: engine_cook [project-dir].
# Game build scripts call this as a pre-build step. Links engine_core only —
# no bgfx, no GLFW, no Jolt/Lua in the binary.
add_executable(engine_cook tools/engine_cook.cpp)
target_link_libraries(engine_cook PRIVATE engine_core)
set_target_properties(engine_cook PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── engine_project — CLI project manager ────────────────────────────────────
# Create/list projects without the editor: engine_project create <dir>.
# The editor's project hub calls the same engine_core scaffolding code.
add_executable(engine_project tools/engine_project.cpp)
target_link_libraries(engine_project PRIVATE engine_core)
set_target_properties(engine_project PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── engine_host — hot-reload dev runner ──────────────────────────────────────
# Runs a project like a game but loads its logic from a shared library and
# reloads it live on change: engine_host <project-dir> <module.so>.
# ENABLE_EXPORTS: the module resolves engine symbols from this executable.
add_executable(engine_host tools/engine_host.cpp)
target_link_libraries(engine_host PRIVATE engine_runtime)
set_target_properties(engine_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    ENABLE_EXPORTS ON
)

# ── kit_lifecycle_test — headless mid-play kit unload/reload regression ─────
# Guards the module graveyard: unloading a kit must not unmap code the worlds
# still reference (flecs component hooks). ENABLE_EXPORTS: loads real kits.
add_executable(kit_lifecycle_test tools/kit_lifecycle_test.cpp)
target_link_libraries(kit_lifecycle_test PRIVATE engine_runtime)
set_target_properties(kit_lifecycle_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    ENABLE_EXPORTS ON
)

# ── clip_binding_test — standalone clip name-binding regression (CPU-only) ──
# Links engine_runtime for include dirs (bx/skeleton headers); executes no GPU.
add_executable(clip_binding_test tools/clip_binding_test.cpp)
target_link_libraries(clip_binding_test PRIVATE engine_runtime)
set_target_properties(clip_binding_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── mem_test — memory manager regression gauntlet ───────────────────────────
add_executable(mem_test tools/mem_test.cpp)
target_link_libraries(mem_test PRIVATE engine_core)

# ── input_test — InputManager determinism/election/action gauntlet ──────────
add_executable(input_test tools/input_test.cpp)
target_link_libraries(input_test PRIVATE engine_runtime)

# ── nav_test — NavService bake + Detour path-query gauntlet (headless) ──────
add_executable(nav_test tools/nav_test.cpp)
target_link_libraries(nav_test PRIVATE engine_runtime)
set_target_properties(nav_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── import_test — multi-submesh import gauntlet (bgfx Noop, headless) ────────
add_executable(import_test tools/import_test.cpp)
target_link_libraries(import_test PRIVATE engine_runtime)
set_target_properties(import_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── event_test — EventSweeper lifecycle/mark-sweep gauntlet ─────────────────
add_executable(event_test tools/event_test.cpp)
target_link_libraries(event_test PRIVATE engine_runtime)
set_target_properties(event_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── anim_pose_test — numeric skin-matrix diagnosis for standalone clips ─────
add_executable(anim_pose_test tools/anim_pose_test.cpp)
target_link_libraries(anim_pose_test PRIVATE engine_runtime)
set_target_properties(anim_pose_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── profiler_test — headless profiler correctness check (engine_core) ────────
add_executable(profiler_test tools/profiler_test.cpp)
target_link_libraries(profiler_test PRIVATE engine_core)
set_target_properties(profiler_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── arena_test — headless frame-arena correctness check (engine_core) ────────
add_executable(arena_test tools/arena_test.cpp)
target_link_libraries(arena_test PRIVATE engine_core)
set_target_properties(arena_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ── profiler_frames — headless proof of the frame profiler path ──────────────
# ENABLE_EXPORTS: with a project argument it starts simulation, which loads
# the project's kits (they resolve engine symbols from this executable).
add_executable(profiler_frames tools/profiler_frames.cpp)
target_link_libraries(profiler_frames PRIVATE engine_runtime)
set_target_properties(profiler_frames PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    ENABLE_EXPORTS ON
)

# ── scene_resave — scene migration + serializer round-trip harness ──────────
# Loads a project's scene (waiting for async imports), saves it back in the
# current format. Upgrades legacy scenes to AssetRef references.
add_executable(scene_resave tools/scene_resave.cpp)
target_link_libraries(scene_resave PRIVATE engine_runtime)
set_target_properties(scene_resave PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
