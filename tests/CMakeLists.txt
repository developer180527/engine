# ── Engine test suite ────────────────────────────────────────────────────────
# Every test is a standalone headless executable (bgfx Noop where GPU types
# are needed) registered with CTest. Lanes:
#
#   ctest -L unit     fast correctness gauntlets — run on every change
#   ctest -L stress   adversarial load/torture   — nightly or before a commit
#                     that touches memory/ECS/physics/assets
#   ctest -L asset    tests needing real game assets (skipped if absent)
#   ctest             everything
#
# Sanitizer lanes: configure a separate build dir with
#   cmake -B build-asan -DENGINE_SANITIZE=address,undefined && ctest
#   cmake -B build-tsan -DENGINE_SANITIZE=thread            && ctest
# (option lives in the root CMakeLists; it instruments engine + third_party.)

# engine_test(<name> [LIB engine_core|engine_runtime] [EXPORTS]
#             [LABEL unit|stress|asset] [TIMEOUT sec] [ARGS ...])
#   EXPORTS: test loads real kit .so modules that resolve engine symbols
#   from the executable (ENABLE_EXPORTS).
function(engine_test name)
    cmake_parse_arguments(ET "EXPORTS" "LIB;LABEL;TIMEOUT" "ARGS" ${ARGN})
    if(NOT ET_LIB)
        set(ET_LIB engine_runtime)
    endif()
    if(NOT ET_LABEL)
        set(ET_LABEL unit)
    endif()
    if(NOT ET_TIMEOUT)
        set(ET_TIMEOUT 120)
    endif()

    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ET_LIB})
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    if(ET_EXPORTS)
        set_target_properties(${name} PROPERTIES ENABLE_EXPORTS ON)
    endif()

    add_test(NAME ${name} COMMAND ${name} ${ET_ARGS})
    set_tests_properties(${name} PROPERTIES
        LABELS ${ET_LABEL}
        TIMEOUT ${ET_TIMEOUT})
endfunction()

# ── unit lane — fast, hermetic, no external assets ───────────────────────────
# Null hid backend link gauntlet (target lives in modules/hid) — running it
# proves the fallback backend still resolves the whole Context contract.
add_test(NAME hid_null_link_check COMMAND hid_null_link_check)
set_tests_properties(hid_null_link_check PROPERTIES LABELS unit TIMEOUT 30)

# SDL3 gamepad backend gauntlet — exists only when that backend is compiled.
# Uses SDL's VIRTUAL joystick, so it needs no controller and runs in CI.
if(TARGET hid_sdl3_gamepad_test)
    add_test(NAME hid_sdl3_gamepad_test COMMAND hid_sdl3_gamepad_test)
    set_tests_properties(hid_sdl3_gamepad_test PROPERTIES LABELS unit TIMEOUT 60)
endif()

# SDL3 event plumbing — only meaningful (and only compilable) on sdl3 builds.
# Catches "renders fine, receives no input": nothing crashes or logs when the
# native-event hook wiring breaks, so it needs an explicit test.
if(ENGINE_WINDOW_BACKEND STREQUAL "sdl3")
    engine_test(sdl3_input_test TIMEOUT 60)
    target_link_libraries(sdl3_input_test PRIVATE SDL3::SDL3)
endif()

# ── sim-purity gauntlet — compiling IS the test (audit A.1–A.4) ───────────────
# Poisoned bgfx/assimp headers sit FIRST on the include path: a sim-facing
# header that transitively includes either fails the build with a message
# naming the violation. See tests/sim_purity_check.cpp for the header list.
engine_test(sim_purity_check TIMEOUT 30)
target_include_directories(sim_purity_check BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/poison)
engine_test(mem_test       LIB engine_core)
engine_test(arena_test     LIB engine_core)
engine_test(profiler_test  LIB engine_core)
engine_test(event_test)
engine_test(input_test)
engine_test(script_host_test)
engine_test(jobs_test)
# Hermetic: builds skeletons/clips through ozz's offline builders, so it needs
# no Assimp and no asset files (unlike anim_pose_test/clip_binding_test).
engine_test(animator_system_test)
engine_test(input_config_test)
engine_test(asset_ready_test)
engine_test(sim_world_test)
engine_test(keymap_test)
engine_test(nav_test)
engine_test(providers_test)
# Importer-dependent tests — dev posture only (the shipping build compiles
# the source-import stack out; these have nothing to exercise there).
if(ENGINE_WITH_SOURCE_IMPORTERS)
    engine_test(async_loader_test)
    engine_test(cooker_test)
    # Cook infrastructure (scheduler / DDC / manifest / scanner) — needs only
    # the GPU-free core, no game assets.
    engine_test(cook_infra_test LIB engine_core)

    # ── Fuzz targets — two ctest lanes from one binary ───────────────────────
    # engine_fuzz_target(<name>) registers:
    #   <name>_regress  replays tests/fuzz/corpus/<domain>/ — fast, DETERMINISTIC,
    #                   gating, label `fuzz-regress`, runs in CI on every change.
    #                   This is what makes past bugs unable to return.
    #   <name>_explore  fresh seeds, label `fuzz-explore`, NOT part of the unit
    #                   lane — long-running, for nightly/self-hosted campaigns.
    #                   Its only job is to feed the corpus.
    function(engine_fuzz_target name domain)
        add_executable(${name} ${name}.cpp)
        target_link_libraries(${name} PRIVATE engine_core)
        target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        set_target_properties(${name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

        add_test(NAME ${name}_regress COMMAND ${name}
                 --corpus ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/${domain})
        set_tests_properties(${name}_regress PROPERTIES
            LABELS "fuzz-regress" TIMEOUT 120)

        # Deliberately modest by default so a developer can run the explore
        # lane locally without it becoming a coffee break; campaigns override
        # with --iterations directly.
        add_test(NAME ${name}_explore COMMAND ${name} --iterations 2000)
        set_tests_properties(${name}_explore PROPERTIES
            LABELS "fuzz-explore" TIMEOUT 600)
    endfunction()

    engine_fuzz_target(fuzz_ddc_manifest_test ddc_manifest)
    engine_fuzz_target(fuzz_mesh_loader_test  mesh_loader)
    engine_test(import_test)
    engine_test(residency_test)   # cooks its own fixture via MeshCooker
endif()

# ── kit lifecycle — loads the project's real combat kit mid-play ─────────────
set(KIT_TEST_PROJECT ${CMAKE_SOURCE_DIR}/fps_shooter)
if(EXISTS ${KIT_TEST_PROJECT}/project.json)
    engine_test(kit_lifecycle_test EXPORTS ARGS ${KIT_TEST_PROJECT})
else()
    # Still build it (catches API drift) even when the project is absent.
    add_executable(kit_lifecycle_test kit_lifecycle_test.cpp)
    target_link_libraries(kit_lifecycle_test PRIVATE engine_runtime)
    set_target_properties(kit_lifecycle_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} ENABLE_EXPORTS ON)
endif()

# ── asset lane — needs real FBX content; registered only when present ────────
set(ZOMBIE_DIR "${CMAKE_SOURCE_DIR}/fps_shooter/assets/Animations/Zombie Anim Pack")
if(NOT ENGINE_WITH_SOURCE_IMPORTERS)
    # FBX-parsing tests can't run in the shipping posture.
elseif(EXISTS "${ZOMBIE_DIR}/Warzombie F Pedroso.fbx")
    engine_test(clip_binding_test LABEL asset
        ARGS "${ZOMBIE_DIR}/Warzombie F Pedroso.fbx" "${ZOMBIE_DIR}/zombie walk.fbx")
    engine_test(anim_pose_test LABEL asset
        ARGS "${ZOMBIE_DIR}/Warzombie F Pedroso.fbx" "${ZOMBIE_DIR}/zombie walk.fbx")
else()
    add_executable(clip_binding_test clip_binding_test.cpp)
    target_link_libraries(clip_binding_test PRIVATE engine_runtime)
    add_executable(anim_pose_test anim_pose_test.cpp)
    target_link_libraries(anim_pose_test PRIVATE engine_runtime)
    set_target_properties(clip_binding_test anim_pose_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

# ── soak lane — continuous-run endurance (trends, not just crashes) ──────────
# CI smoke: 1 minute. The real thing: SOAK_MINUTES=2880 ./build/soak_engine
# (2 days), ideally one lane on Release for trends + one on the TSan build
# for the millions-of-operations sync bugs.
engine_test(soak_engine LABEL soak TIMEOUT 300 ARGS 1)

# ── stress lane — adversarial load/torture (find vulnerabilities) ────────────
engine_test(stress_deep_tree LABEL stress TIMEOUT 600)
engine_test(stress_swarm     LABEL stress TIMEOUT 600)
engine_test(stress_churn     LABEL stress TIMEOUT 600)
engine_test(stress_physics   LABEL stress TIMEOUT 600)
if(ENGINE_WITH_SOURCE_IMPORTERS)
    engine_test(stress_assets LABEL stress TIMEOUT 600)   # importer fuzz
endif()

# ── docs lane — the documentation/maturity contract ──────────────────────────
# engine_doctor validates every doc's front-matter against the TREE: covers/
# tests paths exist, tier claims have mechanical evidence, and `as-built` docs
# whose code changed after their `verified:` date are flagged stale. Docs drift
# silently otherwise — this makes drift a test failure instead of a discovery
# months later. See docs/engineering-standards.md.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME docs_contract
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/scripts/engine_doctor.py check)
    set_tests_properties(docs_contract PROPERTIES LABELS docs TIMEOUT 120)

    # ENGINE_STATUS.md is generated; treat it like a formatter's output so it
    # can never silently disagree with the tree.
    add_test(NAME docs_status_current
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/scripts/engine_doctor.py status --check)
    set_tests_properties(docs_status_current PROPERTIES LABELS docs TIMEOUT 120)
endif()
