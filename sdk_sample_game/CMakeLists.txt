# ── sdk_sample_game ──────────────────────────────────────────────────────────
# Standalone SDK exercise: window, input, primitives, lights, fly camera.
# Links engine::runtime only — exactly what an external game would do.
add_executable(sdk_sample_game main.cpp)
target_link_libraries(sdk_sample_game PRIVATE engine::runtime)
# Binary lands at build/sdk_sample (build/sdk_sample_game is this target's
# object directory — same name would collide).
set_target_properties(sdk_sample_game PROPERTIES
    OUTPUT_NAME sdk_sample
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
