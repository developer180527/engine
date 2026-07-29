# ── editor — the ImGui editor application ────────────────────────────────────
# The only target that links ImGui. Everything engine-side comes from
# engine_runtime; this layer adds panels, gizmos, docking, and the ImGui
# platform/renderer glue (GLFW + bgfx backends).
#
# Two files carry the whole window-system dependency: the window-ops seam
# (editor/window_ops.h) and the ImGui platform backend. Selecting a backend
# swaps exactly those — no panel, gizmo or camera code changes.
if(ENGINE_WINDOW_BACKEND STREQUAL "sdl3")
    set(EDITOR_WINDOW_SOURCES
        window_ops_sdl3.cpp
        # Upstream ImGui backend, used straight from third_party rather than
        # vendored into the editor: there is nothing local to patch in it.
        ${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdl3.cpp)
else()
    set(EDITOR_WINDOW_SOURCES
        window_ops_glfw.cpp
        imgui/imgui_impl_glfw.cpp)
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
