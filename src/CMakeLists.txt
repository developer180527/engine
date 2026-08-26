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
    ${CMAKE_SOURCE_DIR}/shaders/vs_instanced.sc
    ${CMAKE_SOURCE_DIR}/shaders/vs_skinned.sc
    ${CMAKE_SOURCE_DIR}/shaders/vs_shadow_instanced.sc
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
# Source-format import stack (Assimp cookers/importers) — compiled only in
# dev trees. ENGINE_WITH_SOURCE_IMPORTERS=OFF is the shipping posture: a
# fully-cooked runtime that never parses FBX and never links Assimp.
if(ENGINE_WITH_SOURCE_IMPORTERS)
    set(ENGINE_COOKER_SOURCES
        assets/cookers/mesh/mesh_cooker.cpp
        assets/cookers/mesh/decimate.cpp
        assets/cookers/scene/scene_cooker.cpp
        assets/cookers/texture/texture_cooker.cpp
        assets/cookers/texture/texture_encode.cpp
        assets/cookers/cook_service.cpp
        # Shader cooking — one concern per TU (src/assets/cookers/shader/info.md).
        # No bgfx: shaderc runs as a child process, so engine_core stays GPU-free.
        assets/cookers/shader/shader_manifest.cpp
        assets/cookers/shader/shader_reflect.cpp
        assets/cookers/shader/shader_verify.cpp
        assets/cookers/shader/shaderc_invoke.cpp
        assets/cookers/shader/shader_cooker.cpp
        # Material cooking — resolves authored values against a shader's
        # declared interface (src/assets/cookers/material/info.md).
        assets/cookers/material/material_manifest.cpp
        assets/cookers/material/material_resolve.cpp
        assets/cookers/material/material_cooker.cpp
        # Packaging closure — what a dist must contain. In engine_core (not the
        # engine_build TU) so it can be tested; see its header for the
        # shipped-untextured bug that motivated extracting it. NOT named
        # "build/": .gitignore's unanchored `build/` matches at any depth and
        # silently excluded the whole directory from the repo.
        tools/packaging/package_closure.cpp
        # Texture targets: which block family this cook is for, and the EAC
        # encoder bimg does not have (ETC2 alpha + RG11 normals).
        assets/cookers/texture/texture_target.cpp
        assets/cookers/texture/texture_eac.cpp
        # Fast CPU block compression (rgbcx BC1/3/5 + bc7enc BC7) — offline only
        ${CMAKE_SOURCE_DIR}/third_party/bc7enc/rgbcx.cpp
        ${CMAKE_SOURCE_DIR}/third_party/bc7enc/bc7enc.cpp)
else()
    set(ENGINE_COOKER_SOURCES "")
    message(STATUS "engine: SHIPPING posture — Assimp/cookers compiled out; "
                   "dev-only targets skipped")
endif()

add_library(engine_core STATIC
    # Asset cook pipeline (Assimp/stb in, cooked binaries out) — dev only
    ${ENGINE_COOKER_SOURCES}

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
    ozz_animation ozz_animation_offline   # skinned-mesh cooker (skeleton/clip archives)
)
if(ENGINE_WITH_SOURCE_IMPORTERS)
    target_link_libraries(engine_core PUBLIC assimp)
    # BC1/3/5/7 texture encoding (texture_encode.cpp) via vendored
    # rgbcx/bc7enc. Offline-only: shipped runtimes read blocks, never encode.
    # (Replaced bimg_encode: squish cluster-fit was ~6.5s per 4K BC1 and
    # nvtt AVPCL BC7 was exhaustive-minutes; rgbcx does the same 4K in ~100s
    # of ms at equal-or-better quality.)
    target_include_directories(engine_core PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/bc7enc)
    # ASTC + ETC2 come from bimg, which already vendors ARM's astc-encoder and
    # an ETC2 encoder for bgfx — so the mobile texture targets needed no new
    # dependency, only this target. BC stays on rgbcx/bc7enc: bimg's BC path is
    # squish cluster-fit, which is the ~6.5s-per-4K encoder this engine replaced.
    # Offline only, and inside the ENGINE_WITH_SOURCE_IMPORTERS branch, so a
    # shipped player links no encoder of any kind.
    # bimg (not just bimg_encode): image_encode.cpp calls back into bimg's core
    # for getBlockInfo/imageConvert/imageAlloc. engine_runtime already linked bimg
    # so the editor was fine, and engine_cook_worker — the out-of-process cook
    # path, which links engine_core WITHOUT the runtime — was the target that
    # failed. Worth remembering that the worker is the build that catches
    # engine_core's real dependency surface.
    target_link_libraries(engine_core PRIVATE bimg_encode bimg)
    target_include_directories(engine_core PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake/bimg/include)
    # Where the ShaderCooker finds the compiler. Baked in rather than searched
    # for at runtime so a cook from any working directory resolves it; the
    # ENGINE_SHADERC env var overrides, which is how a Windows runner points at
    # its own build to produce the D3D profiles this host cannot emit.
    if(TARGET bgfx::shaderc)
        target_compile_definitions(engine_core PRIVATE
            ENGINE_SHADERC_PATH="$<TARGET_FILE:bgfx::shaderc>")
        add_dependencies(engine_core bgfx::shaderc)
        # Every .sc includes bgfx_shader.sh, which lives in bgfx's own tree.
        # The build-time shader step gets this via BGFX_SHADER_INCLUDE_PATH;
        # the cooker needs the same list or every compile fails at the first
        # #include. ';'-separated (ENGINE_SHADER_INCLUDES overrides).
        # Emitted as a C initializer list ("a","b",) rather than a separated
        # string. A semicolon cannot survive the trip: as a literal it splits
        # target_compile_definitions' LIST argument into two broken -D flags,
        # and as $<SEMICOLON> it reaches the shell as a command separator.
        # Commas and quotes have neither problem.
        set(ENGINE_SHADER_INCLUDE_INIT "")
        foreach(_dir ${BGFX_SHADER_INCLUDES})
            string(APPEND ENGINE_SHADER_INCLUDE_INIT "\"${_dir}\",")
        endforeach()
        target_compile_definitions(engine_core PRIVATE
            ENGINE_SHADER_INCLUDE_LIST=${ENGINE_SHADER_INCLUDE_INIT})
    else()
        message(STATUS "engine: bgfx::shaderc target absent — shader cooking "
                       "will need ENGINE_SHADERC at runtime")
    endif()
    # The engine's own default assets (shaders today, default materials later).
    # CookService scans this as a SECOND asset root so engine defaults cook into
    # each project's .cache — no new packaging concept, and the DDC dedups the
    # work across projects since the sources are byte-identical. $ENGINE_ASSETS
    # overrides; absent is normal in a shipped dist, where the cooked output
    # already travelled inside .cache.
    target_compile_definitions(engine_core PRIVATE
        ENGINE_DEFAULT_ASSETS_DIR="${CMAKE_SOURCE_DIR}/shaders")
    # Block encoders + mip filter are hot inner loops over gigapixels; at
    # Debug -O0 they dominate cook time 5-10x. They're deterministic vendored
    # codecs nobody steps through with the engine — always optimize them.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
        set_source_files_properties(
            ${CMAKE_SOURCE_DIR}/third_party/bc7enc/rgbcx.cpp
            ${CMAKE_SOURCE_DIR}/third_party/bc7enc/bc7enc.cpp
            assets/cookers/texture/texture_encode.cpp
            assets/cookers/texture/texture_eac.cpp
            PROPERTIES COMPILE_OPTIONS "-O2")
    elseif(MSVC)
        # NOT applied in Debug: MSVC's Debug config carries /RTC1, and
        # `/RTC1 /O2` is a hard command-line error (D8016). These are per-SOURCE
        # pins inside engine_core, so /RTC1 cannot be stripped for them alone
        # without disarming Debug checks for the entire library — and silently
        # weakening checks across the engine is a worse trade than a slower
        # Windows Debug cook.
        # Consequence: a Debug cook on Windows is ~8x slower than on
        # Clang/GNU. The fix, when that bites, is to move these TUs into their
        # own static library and strip /RTC1 in that scope — exactly what
        # modules/assetlib/third_party/sqlite3/CMakeLists.txt does.
        set_source_files_properties(
            ${CMAKE_SOURCE_DIR}/third_party/bc7enc/rgbcx.cpp
            ${CMAKE_SOURCE_DIR}/third_party/bc7enc/bc7enc.cpp
            assets/cookers/texture/texture_encode.cpp
            assets/cookers/texture/texture_eac.cpp
            PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:/O2>")
    endif()
endif()
# Same policy for the single-header decode/parse TUs: stb decodes 4K PNGs,
# cgltf parses GLB buffers — both byte-at-a-time loops that crawl at -O0,
# both vendored code with no engine symbols to debug.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    set_source_files_properties(
        assets/importers/stb_impl.cpp
        assets/importers/cgltf_impl.cpp
        PROPERTIES COMPILE_OPTIONS "-O2")
elseif(MSVC)
    # See the note above: /O2 conflicts with Debug's /RTC1 (D8016), so this is
    # non-Debug only.
    set_source_files_properties(
        assets/importers/stb_impl.cpp
        assets/importers/cgltf_impl.cpp
        PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:/O2>")
endif()
target_compile_definitions(engine_core PUBLIC
    BX_CONFIG_DEBUG=$<IF:$<CONFIG:Debug>,1,0>   # bx headers (cooker skeleton types)
    # Headers gate their Assimp paths on this (clip_library.h cook-on-miss).
    ENGINE_WITH_SOURCE_IMPORTERS=$<BOOL:${ENGINE_WITH_SOURCE_IMPORTERS}>
)

add_library(engine::core ALIAS engine_core)

# ── engine_runtime — the full game SDK ───────────────────────────────────────
# Public API surface: include/engine/*.h umbrella headers. Everything under
# src/ is implementation detail unless re-exported there.
# ── Window backend selection (ENGINE_WINDOW_BACKEND, root CMakeLists) ────────
# Only the chosen backend's TU is compiled: the other one's library isn't
# necessarily even available. platform_factory.cpp resolves the choice.
#
# window_ops_*.cpp rides along with its matching IPlatform: they are the same
# choice made twice — one models the single window a game has, the other the
# many windows a TOOL has. Keeping them in one branch is what stops a build
# pairing a GLFW platform with an SDL window seam.
#
# These moved here from src/editor/. Multi-window support had been an
# ImGui-editor privilege, so any other host — Qt, a Rust tool, a custom
# launcher — had to reinvent it against GLFW or SDL directly, which is exactly
# the coupling the SDK exists to prevent.
if(ENGINE_WINDOW_BACKEND STREQUAL "sdl3")
    set(ENGINE_PLATFORM_SOURCES
        runtime/platform/sdl3_platform.cpp
        runtime/platform/window_ops_sdl3.cpp)
else()
    set(ENGINE_PLATFORM_SOURCES
        runtime/platform/glfw_platform.cpp
        runtime/platform/window_ops_glfw.cpp)
endif()

# Title-bar suppression is a WINDOW-SYSTEM concern, not a windowing-LIBRARY
# one: it reaches past GLFW/SDL to the native window, so it is selected by OS
# rather than by backend. Unimplemented platforms link the stub and keep a
# normal title bar (title_bar.h explains why that beats a borderless window).
if(APPLE)
    list(APPEND ENGINE_PLATFORM_SOURCES runtime/platform/title_bar_macos.mm)
elseif(WIN32)
    # UNVERIFIED — written without a Windows toolchain to compile it against.
    # See the header of that file and docs/process/roadmap.md.
    list(APPEND ENGINE_PLATFORM_SOURCES runtime/platform/title_bar_windows.cpp)
else()
    # Linux/BSD: no implementation. Whether a title bar exists at all is the
    # COMPOSITOR's decision there (server- vs client-side decorations), and it
    # differs between GNOME, KDE and the wlroots compositors — there is no
    # portable "hide the title bar but keep the window" call to make. Falls
    # back to a normal title bar rather than guessing.
    list(APPEND ENGINE_PLATFORM_SOURCES runtime/platform/title_bar_stub.cpp)
endif()

add_library(engine_runtime STATIC
    # Runtime core — platform, frame loop, renderer, services
    runtime/runtime.cpp
    runtime/runtime_boot.cpp
    runtime/runtime_frame.cpp
    runtime/runtime_sim.cpp
    runtime/frame_stats_channel.cpp
    ${ENGINE_PLATFORM_SOURCES}
    runtime/platform/platform_factory.cpp
    render/renderer.cpp               # pipeline ownership + RenderContext
    # Renderer, one concern per TU (see src/render/info.md).
    render/renderer/device.cpp        # bgfx init/shutdown + Rendering-heap allocator
    render/renderer/targets.cpp       # framebuffers + the render* entry points
    render/renderer/extract.cpp       # ECS world -> RenderView (the hot path)
    # ForwardPipeline, one concern per TU — it was 812 lines in a header.
    render/pipeline/programs.cpp      # programs, uniforms, shadow RT (onAttach)
    render/pipeline/opaque_pass.cpp   # render(): visible set -> submits
    render/pipeline/shadow_pass.cpp   # renderShadow(): light-space pass
    # Render diagnostics — one concern per TU (see src/render/diag/info.md).
    render/diag/frame_gpu_stats.cpp
    render/diag/gpu_budget.cpp
    # Render machinery — GPU-free, one concern per TU (src/render/world/info.md).
    render/world/frustum.cpp
    render/world/draw_sort.cpp
    render/world/visibility.cpp
    render/world/light_packing.cpp
    # Cooked-shader loading. shader_select is GPU-free (and tested); only
    # shader_library touches bgfx.
    render/shader/shader_select.cpp
    render/shader/shader_library.cpp
    runtime/services/asset_service.cpp
    runtime/services/scene_service.cpp
    runtime/services/nav_service.cpp
    runtime/scripting/engine_api.cpp
    runtime/scripting/engine_api_table.cpp
    runtime/jobs/jobs_enkits.cpp
    runtime/input/input_manager.cpp

    # Cooked-binary loaders
    assets/loaders/mesh_loader.cpp

    # Plugin backends
    plugins/audio_impl.cpp

    # miniaudio behind EngineAudioProviderV1. Compiled INTO the engine (no
    # dlopen in a shipping game) while the identical source also builds as a
    # standalone module for the conformance suite — see the target below.
    audio/miniaudio_provider.cpp

    # Shaders (HEADER_FILE_ONLY — listed to trigger compilation)
    ${ENGINE_SHADER_SOURCES}
)

# Source-format import (dev trees only): the editor's drag-drop AsyncLoader
# + the glTF/Assimp importers. Shipping builds load cooked binaries only.
if(ENGINE_WITH_SOURCE_IMPORTERS)
    target_sources(engine_runtime PRIVATE
        runtime/services/async_loader/loader.cpp
        runtime/services/async_loader/parse.cpp
        runtime/services/async_loader/upload.cpp
        assets/importers/gltf_importer.cpp
        assets/importers/assimp_importer.cpp
    )
endif()

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
        dwmapi         # DwmExtendFrameIntoClientArea — custom title bar
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
# ── The window library, and ONLY the one selected ───────────────────────────
# GLFW used to be linked in BOTH backends, because input_system.h was
# GLFW-callback based and every TU that touched input pulled <GLFW/glfw3.h>
# through it. An SDL3 build therefore still linked GLFW, and — worse for the
# SDK's actual promise — so did a host that supplies its own window: a Qt or
# Rust editor dragged in a windowing library it had no use for.
#
# input_system.h now goes through wsi:: (runtime/platform/window_ops.h) and
# names no backend at all, so each library is reachable from exactly two TUs —
# its IPlatform and its window_ops — and both are already excluded by the
# branch above. Which makes this the whole fix: one library, chosen once.
if(ENGINE_WINDOW_BACKEND STREQUAL "sdl3")
    target_link_libraries(engine_runtime PUBLIC SDL3::SDL3)
    target_compile_definitions(engine_runtime PUBLIC ENGINE_WINDOW_BACKEND_SDL3=1)
else()
    target_link_libraries(engine_runtime PUBLIC glfw)
endif()

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

# ── engine_player — the shipped-game runtime ─────────────────────────────────
# What a game actually ships: cooked content only, no dev machinery. Built in
# BOTH postures; under ENGINE_WITH_SOURCE_IMPORTERS=OFF it links zero Assimp.
# ENABLE_EXPORTS: project kits resolve engine symbols from this executable.
add_executable(engine_player tools/engine_player.cpp)
target_link_libraries(engine_player PRIVATE engine_runtime)
set_target_properties(engine_player PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    ENABLE_EXPORTS ON
)

# ── engine_module_probe — "why won't my kit load?" ──────────────────────────
# Runs ModuleLibrary's compatibility gauntlet against a shared library and
# reports accept/refuse, without a window, a GPU or a project. Also the process
# boundary the module-ABI conformance suite drives (tests/module_abi).
#
# Outside the dev-only block on purpose. The gauntlet is what a SHIPPED player
# runs when it loads dist/kits/, so the posture that ships is the posture where
# being able to ask "was this refused, and why" matters most — and it is the
# posture ENGINE_WITH_SOURCE_IMPORTERS=OFF builds. Costs one small executable.
add_executable(engine_module_probe tools/engine_module_probe.cpp)
target_link_libraries(engine_module_probe PRIVATE engine_runtime)
set_target_properties(engine_module_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── Dev-only targets: skipped in the shipping posture ────────────────────────
if(ENGINE_WITH_SOURCE_IMPORTERS)

# ── engine_cook_worker — isolated per-asset cook child ──────────────────────
# NOT a "tool": it is the cook pipeline's out-of-process half, spawned at
# runtime by the editor and by engine_cook alike. So it is built whenever
# cooking is compiled in, independent of ENGINE_BUILD_TOOLS — otherwise
# -DENGINE_BUILD_TOOLS=OFF would silently downgrade the editor to in-process
# cooking (no crash isolation). Links engine_core only, same as engine_cook.
add_executable(engine_cook_worker tools/engine_cook_worker.cpp)
target_link_libraries(engine_cook_worker PRIVATE engine_core)
set_target_properties(engine_cook_worker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

if(ENGINE_BUILD_EDITOR)
    add_subdirectory(editor)
    # Spawners locate the worker next to their own binary at runtime — make
    # sure it exists whenever they build.
    add_dependencies(editor engine_cook_worker)
endif()

if(ENGINE_BUILD_TOOLS)

# ── engine_cook — CLI asset cooker ───────────────────────────────────────────
# Cooks a project's assets without the editor: engine_cook [project-dir].
# Game build scripts call this as a pre-build step. Links engine_core only —
# no bgfx, no GLFW, no Jolt/Lua in the binary.
add_executable(engine_cook tools/engine_cook.cpp)
target_link_libraries(engine_cook PRIVATE engine_core)
set_target_properties(engine_cook PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_dependencies(engine_cook engine_cook_worker)

# ── input_ab — SDL3 vs native-hid input measurement ─────────────────────────
# Settles whether SDL3 can be the raw-input provider for mouse/keyboard or only
# for gamepads (backlog Phase F #26). Needs SDL3 for one side and a real native
# hid backend for the other, so it only builds when both are present:
#   cmake -B build-ab -DENGINE_WINDOW_BACKEND=sdl3 -DHID_BACKEND=iohid
if(ENGINE_WINDOW_BACKEND STREQUAL "sdl3" AND NOT HID_BACKEND_RESOLVED STREQUAL "sdl3")
    add_executable(input_ab tools/input_ab.cpp)
    target_link_libraries(input_ab PRIVATE hid SDL3::SDL3)
    set_target_properties(input_ab PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

# ── engine_build — package a project into a shippable dist/ ─────────────────
# Cook -> shipping player (Assimp-free) -> kits rebuilt Release -> assemble.
add_executable(engine_build tools/engine_build.cpp)
target_link_libraries(engine_build PRIVATE engine_core)
target_compile_definitions(engine_build PRIVATE
    ENGINE_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
set_target_properties(engine_build PROPERTIES
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

# ── Tests moved to tests/ (registered with CTest — see tests/CMakeLists.txt).
# Run: ctest -L unit (fast lane) / ctest -L stress (torture lane) / ctest (all)

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

endif() # ENGINE_BUILD_TOOLS

endif() # ENGINE_WITH_SOURCE_IMPORTERS (dev-only targets)

# ── engine_audio_miniaudio_module — the provider as a standalone .so ─────────
# The SAME source the engine links, built as a loadable module with its own copy
# of miniaudio and NO link to engine_runtime. Two things fall out of that:
#
#   1. It is what tests/audio_conformance dlopens. The Rust suite therefore
#      holds the code the game actually runs to the contract, rather than a
#      demo written beside it.
#   2. It MECHANICALLY PROVES the provider is engine-independent. If anyone adds
#      an engine header or an engine symbol to miniaudio_provider.cpp, this
#      target fails to link — which is the whole claim the audio ABI makes, made
#      unbreakable instead of merely documented.
#
# Deliberately not installed and not linked by anything: its job is to be built.
add_library(engine_audio_miniaudio_module MODULE
    audio/miniaudio_provider.cpp
    plugins/audio_impl.cpp          # this module's own MA_IMPLEMENTATION
)
target_include_directories(engine_audio_miniaudio_module PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/miniaudio
)
set_target_properties(engine_audio_miniaudio_module PROPERTIES
    CXX_VISIBILITY_PRESET hidden      # only engineAudioProviderV1 escapes
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    PREFIX "lib" OUTPUT_NAME "engine_audio_miniaudio" SUFFIX ".so"
)
if(APPLE)
    target_link_libraries(engine_audio_miniaudio_module PRIVATE
        "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
elseif(UNIX)
    target_link_libraries(engine_audio_miniaudio_module PRIVATE dl m pthread)
endif()
