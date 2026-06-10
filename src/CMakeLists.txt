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
)
set(ENGINE_FRAGMENT_SHADERS
    ${CMAKE_SOURCE_DIR}/shaders/fs_triangle.sc
    ${CMAKE_SOURCE_DIR}/shaders/fs_shadow.sc
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

# Public API surface: include/engine/*.h umbrella headers. Everything under
# src/ is implementation detail unless re-exported there.
add_library(engine_runtime STATIC
    # Runtime core — platform, frame loop, renderer, services
    runtime/runtime.cpp
    runtime/glfw_platform.cpp
    runtime/renderer.cpp
    runtime/async_loader.cpp
    runtime/asset_service.cpp
    runtime/scene_service.cpp

    # Import + serialization
    io/cgltf_impl.cpp
    io/gltf_importer.cpp
    io/stb_impl.cpp
    io/assimp_importer.cpp
    io/cook_service.cpp

    # Asset cook pipeline
    cookers/mesh_cooker.cpp
    cookers/scene_cooker.cpp
    cookers/texture_cooker.cpp

    # Cooked-binary loaders
    loaders/mesh_loader.cpp

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
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/components
    ${CMAKE_CURRENT_SOURCE_DIR}/render
    ${CMAKE_CURRENT_SOURCE_DIR}/io
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann
    ${CMAKE_SOURCE_DIR}/third_party/cgltf
    ${CMAKE_SOURCE_DIR}/third_party/stb
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
    assetlib
    assimp
    bgfx bx bimg
    glfw
    flecs_static
    Jolt
    lua
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

add_subdirectory(editor)

# ── engine_cook — CLI asset cooker ───────────────────────────────────────────
# Cooks a project's assets without the editor: engine_cook [project-dir].
# Game build scripts call this as a pre-build step.
add_executable(engine_cook tools/engine_cook.cpp)
target_link_libraries(engine_cook PRIVATE engine_runtime)
set_target_properties(engine_cook PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
