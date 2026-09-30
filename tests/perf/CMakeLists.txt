# ── The seam-cost benchmark (perf lane, never a gate) ───────────────────────
# docs/rhi/swappability.md decides whether "swappable renderer" is compatible
# with "no performance loss", and §2's five numbers are what turn that from an
# argument into a measurement. They lived only in the .md until review noted
# that the document's own §8 rule — no abstraction ships without being measured
# — cited three precedents that all HAVE committed harnesses, and this one did
# not. ~130 lines to make the strongest doc in docs/rhi/ reproducible.
#
# Deliberately NOT a gate: dispatch cost is a property of the machine and the
# compiler, not of this repo, so a threshold would fail on somebody's laptop for
# no defect. `perf` lane, run on purpose:  ctest -L perf
#
# BOTH TARGETS ARE FORCED TO OPTIMISE, whatever the build type. This is not a
# preference: the default dev build here is Debug (-g, no -O), and at -O0
# nothing inlines, so every variant costs the same and the benchmark reports
# noise. The first run of this harness in a Debug tree said a cross-.dylib call
# was FASTER than a direct one, which is the shape of a measurement that is
# measuring the build flags instead of the thing.
#
# Its own directory because of MSVC: a Debug build carries /RTC1, which cl
# refuses to combine with /O2 (error D8016), and CMake 3.20 cannot drop a flag
# from one target, only from the flags of a DIRECTORY. So the runtime checks are
# removed here, for these two targets and nothing else. Both Windows legs failed
# on this from 2026-09-07 until WO-038.
if(MSVC)
    string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    set(_seam_opt /O2)
else()
    set(_seam_opt -O2)
endif()

add_library(seam_cost_plugin SHARED seam_cost_plugin.cpp)
target_compile_options(seam_cost_plugin PRIVATE ${_seam_opt})
set_target_properties(seam_cost_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(seam_cost_bench seam_cost_bench.cpp)
target_compile_options(seam_cost_bench PRIVATE ${_seam_opt})
set_target_properties(seam_cost_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_compile_definitions(seam_cost_bench PRIVATE
    ENGINE_SEAM_PLUGIN_PATH="$<TARGET_FILE:seam_cost_plugin>")
add_dependencies(seam_cost_bench seam_cost_plugin)
if(UNIX)
    target_link_libraries(seam_cost_bench PRIVATE ${CMAKE_DL_LIBS})
endif()
add_test(NAME seam_cost COMMAND seam_cost_bench 200)
set_tests_properties(seam_cost PROPERTIES LABELS perf TIMEOUT 300)
