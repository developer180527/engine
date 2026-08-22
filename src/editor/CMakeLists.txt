# ── editor — the ImGui editor application ────────────────────────────────────
# The only target that links ImGui. Everything engine-side comes from
# engine_runtime; this layer adds panels, gizmos, docking, and the ImGui
# platform/renderer glue (GLFW + bgfx backends).
#
# The window-ops seam used to live here (window_ops_glfw.cpp /
# window_ops_sdl3.cpp) and now ships in engine_runtime as
# runtime/platform/window_ops.h — multi-window support was never an editor
# feature, it is what any host driving more than one window needs, and keeping
# it here forced a Qt or Rust editor to reinvent it against GLFW directly.
#
# What remains backend-specific to the EDITOR is exactly one thing: the ImGui
# platform backend.
if(ENGINE_WINDOW_BACKEND STREQUAL "sdl3")
    # Upstream ImGui backend, used straight from third_party rather than
    # vendored into the editor: there is nothing local to patch in it.
    set(EDITOR_WINDOW_SOURCES
        ${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdl3.cpp)
else()
    set(EDITOR_WINDOW_SOURCES imgui/imgui_impl_glfw.cpp)
endif()

add_executable(editor
    main.cpp
    imgui/imgui_bgfx.cpp
    ${EDITOR_WINDOW_SOURCES}
)

if(ENGINE_WINDOW_BACKEND STREQUAL "sdl3")
    target_include_directories(editor PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/imgui/backends)
    target_link_libraries(editor PRIVATE SDL3::SDL3)
endif()

target_include_directories(editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(editor PRIVATE
    engine_runtime
    imgui
)

# Keep the binary at build/editor (not build/src/editor/editor).
# ENABLE_EXPORTS: project kits dlopened at Play resolve engine symbols (flecs,
# the C API, logger) from this executable — same as engine_host.
set_target_properties(editor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    ENABLE_EXPORTS ON
)
