# ── hot_reload_game — sample hot-reloadable game module ──────────────────────
# A MODULE library compiled against engine headers only; every engine symbol
# resolves from the engine_host executable at load time. Never link
# engine_runtime into a module — that would duplicate engine state.
add_library(hot_reload_game MODULE game.cpp)
target_link_libraries(hot_reload_game PRIVATE engine::headers)
if(APPLE)
    # Undefined symbols (flecs, bx, logger) resolve against the host at load.
    target_link_options(hot_reload_game PRIVATE -undefined dynamic_lookup)
endif()
set_target_properties(hot_reload_game PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    DEBUG_POSTFIX ""            # stable name for the host's file watcher
)
