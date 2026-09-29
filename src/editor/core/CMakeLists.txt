# ── editor_core — the editor's GUI-free half ─────────────────────────────────
# What every editor front end shares (the ImGui editor, the libgui experiment):
# commands and shortcuts today, panel models next. It links no GUI toolkit and
# is built whether or not the ImGui editor is (ENGINE_BUILD_EDITOR), so a
# second front end and the tests do not need ImGui to exist.
add_library(editor_core STATIC
    shortcuts.cpp
)
target_include_directories(editor_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann)
target_compile_features(editor_core PUBLIC cxx_std_20)
