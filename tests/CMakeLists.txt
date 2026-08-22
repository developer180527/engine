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
# The log ring. Speed is a benchmark's job; this pins the three properties the
# speed rests on, each of which fails silently: a filtered line must do NO work
# (asserted through the sequence counter and through argument side effects, not
# through the output), concurrent writers must lose no reservations, and the
# ring's loss must be countable exactly.
engine_test(logger_test    LIB engine_core)
# A scene must outlive the kit that authored it. reflected_serde stashes unknown
# component blobs in ReflectedPending and re-emits them on save — and the SAVE was
# never tested, which is the only step that can destroy anything: a load that
# stashes and a save that omits look identical from inside the session doing the
# damage. Hermetic — "the type is not registered" is the whole condition, so this
# needs no kit and no dlopen.
engine_test(reflected_pending_test)
engine_test(event_test)
engine_test(input_test)
engine_test(script_host_test)
engine_test(jobs_test)
# Hermetic: builds skeletons/clips through ozz's offline builders, so it needs
# no Assimp and no asset files (unlike anim_pose_test/clip_binding_test).
engine_test(animator_system_test)
# GpuResourceCache is payload-agnostic, so its dedup/refcount/LRU/budget logic
# tests with a fake handle and no GPU — src/render has no render harness yet.
engine_test(gpu_cache_test)
# Render diagnostics: churn/budget/census logic is deliberately GPU-free so the
# least-verified subsystem finally has a test that can fail.
engine_test(render_diag_test)
# Render machinery: culling, sort keys, visibility and light packing are pure
# functions over PODs by design, so the two things that fail SILENTLY in a
# renderer — geometry popping at the frustum edge, batching quietly collapsing
# — finally have assertions. Links without bgfx or Mesh.
engine_test(render_world_test)
# The three registries every drawn thing lives in. Their handles are bare slot
# indices over a free list with no generation counter, which has already produced
# one real bug (the material name cache in fd08408) — so the aliasing contract is
# asserted rather than assumed. bgfx Noop: Texture/Mesh own real bgfx handles.
engine_test(render_registry_test)
# Bone palettes moved out of the SkinnedMesh component into a slot pool, which
# turns a component field into an ALLOCATION — so the two ways an allocator fails
# are asserted: a slot never returned (8 KB a time, invisible for a thousand
# spawns) and a slot returned twice (two skeletons writing one palette, which
# presents as an animation bug somewhere else entirely). It also pins the flecs
# behaviour the whole fix depends on — that on_remove fires during WORLD TEARDOWN
# — including the negative case, a hookless world that leaks every slot.
engine_test(skin_palette_test)
# SUBMISSION — the last uncovered part of src/render, and the part that changes
# every week. Possible only because SubmitStats counts on our side of the API:
# bgfx's Noop backend never sets numDraw, so draw counts cannot be read back from
# bgfx headlessly. Builds a RenderView by hand (no ECS, no Renderer, no window)
# and asserts the submission DECISIONS — cull wiring, batch runs, instanced runs,
# the uniform-buffer draw ceiling, the one-palette-per-skinned-item invariant,
# and the shadow pass's separate light-space cull. It cannot check pixels.
engine_test(render_pipeline_test)
# The two-query extraction partition (parentless / parented). Its only failure mode
# is silent — an entity in neither half vanishes from the frame, one in both is
# drawn twice — so the partition is asserted rather than trusted to flecs' with/
# without semantics. Also the only coverage of the SNAPSHOT extraction path, since
# HeadlessPlatform skips bgfx and the editor's Play mode has no other test.
engine_test(extract_partition_test)
# The PrevTransform interpolation snapshot's contract. Rebuilt for speed (12.7 ms
# -> 0.6 ms per step at 50 k objects, issues.md H.0) into an in-place overwrite pass
# plus a structural-add pass, and its failure modes are all invisible to timing and
# to the submit counters: a newcomer that never gets one stutters forever, a
# post-step value makes the lerp interpolate from the wrong end, and a camera that
# acquires one makes look direction lag.
engine_test(prev_snapshot_test)
# Shader cooking: manifest validation, bgfx uniform-table reflection, interface
# verification and the .cshader container — all pure functions over bytes, so
# none of it needs shaderc or a GPU. The subprocess path is covered by actually
# cooking shaders/standard.shader, not here.
engine_test(shader_cooker_test LIB engine_core)
# Materials as data: every parameter name checked against the shader's declared
# interface, and uniform blocks proven COMPLETE (an unset parameter must read
# its default, not the previous draw's register).
engine_test(material_cooker_test LIB engine_core)
# The half of LOD that was missing: R20 measured selection at zero benefit
# because nothing could produce a cheaper mesh. This asserts levels ARE cheaper.
engine_test(decimate_test LIB engine_core)
# Which cooked shader variant this machine wants. Guards two silent failures: a
# package cooked for the wrong backend (no program -> nothing renders) and a
# feature mask with no cooked variant (the nearest variant is confidently wrong).
engine_test(shader_select_test)
# The packaging path. It once shipped a game with every mesh present and every
# surface untextured, from an ordinary clean-rebuild-against-a-warm-DDC, and
# nothing caught it because the logic lived inline in engine_build's main flow.
engine_test(package_closure_test LIB engine_core)

# ── Rust FFI seam ────────────────────────────────────────────────────────────
# Only when the Rust module is actually built. Links engine_net directly rather
# than through engine_runtime: this test is about the BOUNDARY, so it should
# fail if the staticlib is broken, not be masked by whatever else pulls it in.
if(TARGET engine_net)
    add_executable(rust_ffi_test rust_ffi_test.cpp)
    target_link_libraries(rust_ffi_test PRIVATE engine_net)
    set_target_properties(rust_ffi_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME rust_ffi_test COMMAND rust_ffi_test)
    set_tests_properties(rust_ffi_test PROPERTIES LABELS unit TIMEOUT 60)
endif()
engine_test(input_config_test)
engine_test(asset_ready_test)
engine_test(material_name_test)   # by-name addressing + the slot-reuse alias
engine_test(sim_world_test)
engine_test(keymap_test)
engine_test(nav_test)
# The PRIMITIVE tier of the engine API table (jobs / memory / drawSubmit) — the
# layer that lets a developer build their own system instead of adopting ours.
# The compatibility promise itself: a kit built for v1 must bind on a v5 host.
# Guards the frozen group OFFSETS (sizes are static_asserted in the header) and
# the ">=" gating that replaced the "==" which rejected every older module.
engine_test(api_abi_compat_test)
engine_test(api_primitives_test)
engine_test(providers_test)
# Importer-dependent tests — dev posture only (the shipping build compiles
# the source-import stack out; these have nothing to exercise there).
if(ENGINE_WITH_SOURCE_IMPORTERS)
    engine_test(async_loader_test)
    engine_test(cooker_test)
    # Cook infrastructure (scheduler / DDC / manifest / scanner) — needs only
    # the GPU-free core, no game assets.
    engine_test(cook_infra_test LIB engine_core)
    # ENGINE_COOK_DETERMINISM_CHECK: cook twice, compare, fail on a difference.
    # A non-deterministic cooker makes the content-addressed cache serve bytes
    # recooking would not reproduce — and it travels to everyone pulling from the
    # shared DDC. We shipped one (the decimator's (int32_t)floor(NaN), INT_MIN on
    # x86-64 and 0 on arm64) and found it by reading. This proves the check CATCHES
    # each way a cooker can drift, because one that only ever passes is
    # indistinguishable from one that does nothing. Needs the internal dispatch
    # header — the choke point both pipelines share.
    engine_test(cook_determinism_test LIB engine_core)
    target_include_directories(cook_determinism_test PRIVATE
        ${CMAKE_SOURCE_DIR}/modules/assetlib/src)
    # The same binary with the flag OFF: a check that fires when disabled would
    # double every cook, and the flag would be turned off and stay off.
    add_test(NAME cook_determinism_test_off
             COMMAND cook_determinism_test --off)
    set_tests_properties(cook_determinism_test_off
                         PROPERTIES LABELS unit TIMEOUT 120)
    # Block geometry for non-4x4 (ASTC) formats, the cook TARGET in the DDC key,
    # and the EAC encoder bimg does not provide. Links bimg for its ETC2A
    # DECODER: the alpha encoder is validated against an implementation nobody
    # here wrote, which is the only reason to believe the bit layout.
    engine_test(texture_format_test LIB engine_core)
    target_link_libraries(texture_format_test PRIVATE bimg)
    target_include_directories(texture_format_test PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake/bimg/include)

    # Result-file framing, DDC budget/LRU eviction, registry schema versioning
    engine_test(cook_hardening_test LIB engine_core)
    # Enforces that every cooker's DECLARED inputs move its cook key
    engine_test(cook_deps_test LIB engine_core)

    # ── Fuzz targets — two ctest lanes from one binary ───────────────────────
    # engine_fuzz_target(<name>) registers:
    #   <name>_regress  replays tests/fuzz/corpus/<domain>/ — fast, DETERMINISTIC,
    #                   gating, label `fuzz-regress`, runs in CI on every change.
    #                   This is what makes past bugs unable to return.
    #   <name>_explore  fresh seeds, label `fuzz-explore`, NOT part of the unit
    #                   lane — long-running, for nightly/self-hosted campaigns.
    #                   Its only job is to feed the corpus.
    # LIB defaults to engine_core — the deserializers under test are pure byte
    # parsers and must stay linkable without the renderer. A target that reaches
    # into ECS components (the JSON entity path) passes LIB engine_runtime.
    function(engine_fuzz_target name domain)
        cmake_parse_arguments(EFT "" "LIB" "" ${ARGN})
        if(NOT EFT_LIB)
            set(EFT_LIB engine_core)
        endif()
        add_executable(${name} ${name}.cpp)
        target_link_libraries(${name} PRIVATE ${EFT_LIB})
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
    engine_fuzz_target(fuzz_scene_loader_test scene_loader)
    engine_fuzz_target(fuzz_entity_serde_test entity_serde LIB engine_runtime)
    engine_fuzz_target(fuzz_scene_service_test scene_service LIB engine_runtime)
    engine_test(import_test)
    engine_test(residency_test)
# ── The editor's first automated coverage ────────────────────────────────────
# UndoStack is header-only with no ImGui in its include graph, and it is the
# editor's most consequential logic: a wrong undo destroys authored work and
# the only symptom is a scene that is subtly wrong later. src/editor/info.md's
# argument for staying untested covers the mouse-driven PANELS, not this.
# The hierarchy post-pass. createEntity is fuzzed, but parent links are
# restored AFTERWARDS from the same untrusted JSON, and in loadAsync that pass
# sits outside the try/catch that only covers json::parse.
engine_test(scene_parents_test)
engine_test(editor_undo_test)
# editor.json comes back FROM DISK, which makes EditorPrefs::load untrusted
# input on the project-open path — a crash there means the project cannot be
# opened at all. Needs imgui only because EditorCamera's header includes it.
engine_test(editor_prefs_test)
# imgui only because EditorCamera's header includes it; no ImGui context is
# ever created and none of its window-ops calls are referenced, so this links
# headless. engine_test's LIB is a single-value keyword, hence the explicit
# second library here rather than a list.
target_link_libraries(editor_prefs_test PRIVATE imgui)
# One cooked mesh must cost ONE pair of GPU buffers. Pins the fix for a bug that
# rendered 8% of a 50 000-entity scene: loadMesh created buffers per CALL, and bgfx's
# pool is 4096, so the rest failed with "bgfx buffer creation failed" and nothing
# crashed. Loads 5 000 times on purpose — past the pool — so the test reproduces the
# original failure rather than just checking a handle is reused.
engine_test(mesh_dedup_test)   # cooks its own fixture via MeshCooker
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
# The skinned path had no load coverage at all: every other stress test uses
# static meshes, so the one component measured in KILOBYTES per entity was
# never exercised at scale. Measures what extraction pays to WALK it.
engine_test(stress_skinned   LABEL stress TIMEOUT 600)
engine_test(stress_physics   LABEL stress TIMEOUT 600)
if(ENGINE_WITH_SOURCE_IMPORTERS)
    engine_test(stress_assets LABEL stress TIMEOUT 600)   # importer fuzz
endif()

# ── docs lane — the documentation/maturity contract ──────────────────────────
# engine_doctor validates every doc's front-matter against the TREE: covers/
# tests paths exist, tier claims have mechanical evidence, and `as-built` docs
# whose code changed after their `verified:` date are flagged stale. Docs drift
# silently otherwise — this makes drift a test failure instead of a discovery
# months later. See docs/process/engineering-standards.md.
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

# ── The C half of the audio ABI freeze ───────────────────────────────────────
# The header's ENGINE_AUDIO_FROZEN static asserts fire only in a TU that
# INCLUDES it, and until this target existed no TU in the tree did — so they had
# never been compiled once. The Rust suite pins its own transcription and is
# blind to the C side moving underneath it; this is the other half. Plain C11 on
# purpose: it also proves the header is consumable without a C++ compiler, which
# is the claim we make to anyone writing a provider.
add_executable(audio_abi_check audio_abi_check.c)
set_target_properties(audio_abi_check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(audio_abi_check PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME audio_abi_check COMMAND audio_abi_check)
set_tests_properties(audio_abi_check PROPERTIES LABELS unit TIMEOUT 30)

# ── The audio-provider ABI's executable specification (Rust) ─────────────────
# tests/audio_conformance is an "executable specification": it hand-transcribes
# engine_audio_provider.h as Rust #[repr(C)] and holds a provider to the
# contract. It proves the ABI is implementable from the C declarations alone —
# which is the actual claim being made when we tell a third party they can write
# a provider in any language.
#
# WIRED IN BECAUSE IT WAS NOT. The crate existed and ran nowhere: not in ctest,
# not in CI, so nothing would have noticed it breaking. An unrun specification is
# a document.
#
# Optional in the same way as modules/net — Rust is a real dependency for every
# contributor, so a tree without cargo must still configure and test. The
# difference from a skipped test is visible: cmake says so at configure time.
if(ENGINE_WITH_RUST)
    find_program(CARGO_EXECUTABLE cargo)
    if(CARGO_EXECUTABLE)
        add_test(NAME audio_abi_conformance
                 COMMAND ${CMAKE_COMMAND} -E env
                         ENGINE_AUDIO_PROVIDER=$<TARGET_FILE:engine_audio_miniaudio_module>
                         ${CARGO_EXECUTABLE} test --quiet
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests/audio_conformance)
        # Pointed at the REAL provider, not just the Rust reference. Without
        # this the lane proved only that the suite could pass something written
        # to pass it; with it, the shipping miniaudio backend is the thing under
        # test. It is also what caught the first version of that backend letting
        # ma_engine own the device, which left samplesPlayed frozen at 0.
        set_property(TEST audio_abi_conformance APPEND PROPERTY
                     DEPENDS audio_abi_check)
        # `unit`, not a lane of its own: it is a fast, hermetic, no-GPU check of a
        # header, which is exactly what the unit lane is.
        set_tests_properties(audio_abi_conformance PROPERTIES LABELS unit TIMEOUT 300)
    else()
        message(STATUS "audio_abi_conformance: cargo not found — audio ABI "
                       "conformance DISABLED (install rustup to run it)")
    endif()
endif()
