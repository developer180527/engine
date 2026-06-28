# ── editor — the ImGui editor application ────────────────────────────────────
# The only target that links ImGui. Everything engine-side comes from
# engine_runtime; this layer adds panels, gizmos, docking, and the ImGui
# platform/renderer glue (GLFW + bgfx backends).

add_executable(editor
    main.cpp
    imgui/imgui_bgfx.cpp
    imgui/imgui_impl_glfw.cpp
)

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
