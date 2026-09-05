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

# ── The audio provider's allocator, under the sanitizer ──────────────────────
# `maRealloc` read past the end of every block it grew, on every sound decode,
# for the provider's whole existence — and ASan, which runs on every push, never
# saw it. Two reasons, both removed here:
#
#   * audio_abi_conformance is the only test that drives createSound, and it is
#     EXCLUDED from sanitizer builds (the Rust harness aborts under a dlopen'd
#     instrumented library, see the guard further down). The one test that
#     exercised the code was the one test the sanitizer could not see.
#   * The engine's host allocator is TLSF over large mappings, so a 64 KB
#     over-read lands inside a 2 MB block and disturbs nothing. It took a Linux
#     container with a different heap layout to turn it into a SIGSEGV.
#
# So: C++ (no Rust harness, therefore no exclusion) with a malloc-backed host
# allocator, which gives ASan redzones to guard. It dlopens the SAME module the
# conformance suite does, so it holds the shipping code rather than a copy.
if(TARGET engine_audio_miniaudio_module)
    add_executable(audio_provider_asan_test audio_provider_asan_test.cpp)
    target_include_directories(audio_provider_asan_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(audio_provider_asan_test PRIVATE
        ENGINE_AUDIO_MODULE_PATH="$<TARGET_FILE:engine_audio_miniaudio_module>")
    add_dependencies(audio_provider_asan_test engine_audio_miniaudio_module)
    set_target_properties(audio_provider_asan_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    if(UNIX)
        target_link_libraries(audio_provider_asan_test PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME audio_provider_asan COMMAND audio_provider_asan_test)
    # `unit`, so it runs on EVERY leg and in BOTH sanitizer lanes. Deliberately
    # not gated on ENGINE_SANITIZE: it is a useful allocator-invariant test
    # without a sanitizer, and the whole point is that it is present when one is.
    set_tests_properties(audio_provider_asan PROPERTIES LABELS unit TIMEOUT 120)
endif()

# ── unit lane — fast, hermetic, no external assets ───────────────────────────
# Null hid backend link gauntlet (target lives in modules/hid) — running it
# proves the fallback backend still resolves the whole Context contract.
add_test(NAME hid_null_link_check COMMAND hid_null_link_check)
set_tests_properties(hid_null_link_check PROPERTIES LABELS unit TIMEOUT 30)

# The hid SPSC ring: push order, drop-newest overflow, monotonic clock, and the
# 24-byte wire POD. It had an add_executable in modules/hid/CMakeLists.txt and
# NO add_test anywhere, so it was compiled by every build and run by nothing —
# the same shape as BUG-0010 (kit_lifecycle_test built but never registered, so
# CI had never executed it once). It passes, and has presumably passed the whole
# time; a test nobody runs is a test nobody can trust.
add_test(NAME hid_ring_test COMMAND hid_ring_test)
set_tests_properties(hid_ring_test PROPERTIES LABELS unit TIMEOUT 60)

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
# The shared components' LAYOUT, frozen. componentLayoutHash() — the gate that
# decides whether a kit may touch live ECS memory — hashes sizeof and alignof
# only, so reordering two same-sized fields leaves it byte-identical and the gate
# accepts a kit that then misreads world data. Same defect class the API table
# fixed with api_abi_compat_test; nothing had done it for components, and
# `offsetof` appeared in exactly one file in the tree.
engine_test(component_abi_test)
engine_test(event_test)
engine_test(input_test)
engine_test(script_host_test)
engine_test(jobs_test)
# Thread scheduling classes. Every engine::qos call site is fire-and-forget, so
# a deleted or never-fired hook produces a build that is merely slower on a
# machine we do not own — the profile of a call site that quietly stops running.
# Asserts the pool's workers land on Initiated, that the calling thread is NOT
# demoted with them, and that an external (kit/provider) thread keeps the class
# its owner chose. Strong readback exists only on Apple; the test gates on
# engine::qos::classIsObservable() rather than a platform macro, so a platform
# that later gains a real getter strengthens it without an edit.
engine_test(thread_qos_test)
# The upload seam's BEHAVIOUR, which neither the grep nor the include probe can
# see. Two contracts: headless produces no handles and never crashes, and the
# texture format is checked BEFORE staging — because createTexture2D's refusal
# strands the payload for the life of the process (bgfx frees a Memory only when
# a command consumes it), and a mis-targeted build takes that path for every
# texture in the scene. The contract sentence in gpu.h claimed the opposite
# until review caught it.
engine_test(gpu_seam_test)
# The null renderer's BEHAVIOUR. Its completeness is already the compiler's job —
# IRenderer is pure virtual, so a new method is a build failure until
# NullRenderer implements it. What a compiler cannot see is the three queries
# whose null answer must still be MEANINGFUL, and the regression this whole
# change exists for: a kit submitting draws on a server must be accepted and
# discarded, never accumulated (the ~480 KB/s leak, issues.md 2026-08-10).
engine_test(null_renderer_test)
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
# Phase 5 step 4: Material has ONE representation. Pins the synthesized standard
# form against shaders/standard.shader's declared interface, and that the typed
# accessors are a window onto the blocks rather than a second copy — which is the
# property that stops an inspector slider editing something the renderer ignores.
engine_test(material_form_test LIB engine_core)
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

# ── The editor UI seam, crossed by something that is not ImGui ──────────────
# EngineApiUiV1 is frozen and had exactly ONE backend (imgui_bgfx.cpp:264),
# which is how IRenderPipeline decayed into decoration. A planned Rust/egui
# editor depends on the frozen group NOT being ImGui-shaped; this proves it by
# registering a second backend rather than by reading the header.
engine_test(ui_backend_test)
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

    # ── The no-editor path ──────────────────────────────────────────────────
    # Every tool is a CLI and every source format is text, so a complete game can
    # be built with the SDK and a text editor. Nothing verified that, and this
    # repo has shipped "a feature nothing exercises is a comment" twice already
    # (ENGINE_AUDIO_FROZEN's never-compiled asserts; kit_lifecycle_test never
    # once run in CI).
    #
    # Scaffolds a project with engine_project, replaces its scene with one
    # written BY HAND, cooks it with engine_cook, and requires the hand-written
    # values to survive into the binary the runtime loads. The drift it catches
    # is specific: the editor WRITES .scene files and the cooker READS them, and
    # every .scene in this tree came out of the editor — so a cooker that starts
    # depending on an editor-only field breaks hand authoring and nothing notices.
    #
    # The other half — that all of this still works with no editor binary in the
    # tree at all — is the `sdk-only-game` CI job, which configures
    # ENGINE_BUILD_EDITOR=OFF and asserts the editor is absent.
    engine_test(sdk_only_game_test LIB engine_core TIMEOUT 300)
    if(TARGET engine_project AND TARGET engine_cook)
        set_tests_properties(sdk_only_game_test PROPERTIES ENVIRONMENT
            "ENGINE_PROJECT_TOOL=$<TARGET_FILE:engine_project>;ENGINE_COOK_TOOL=$<TARGET_FILE:engine_cook>;ENGINE_REQUIRE_SDK_ONLY_TEST=1")
        add_dependencies(sdk_only_game_test engine_project engine_cook)
        # The cook spawns engine_cook_worker per asset, so the worker must exist
        # before the test runs or every cook falls back and the lane proves less
        # than it claims.
        if(TARGET engine_cook_worker)
            add_dependencies(sdk_only_game_test engine_cook_worker)
        endif()
    endif()

    # ── The Add-on protocol's own rules ─────────────────────────────────────
    # The writer's structural guarantees (verdict ordering, sanitising, the
    # record/recordExact split) plus engine_build's exit-code and manifest
    # contract. NOT engine_build's packaging — package_closure_test covers that,
    # and a real package configures and builds the whole engine tree.
    engine_test(addon_protocol_test LIB engine_core)
    if(TARGET engine_build)
        # ENGINE_REQUIRE_ADDON_TOOL_TESTS makes a missing binary a FAILURE rather
        # than a skip: under ctest the tool is known to exist, so "it wasn't
        # there" is a finding. Without it a lane that had quietly stopped
        # exercising the tool would print `ok` exactly like one that ran.
        set_tests_properties(addon_protocol_test PROPERTIES
            ENVIRONMENT "ENGINE_BUILD_TOOL=$<TARGET_FILE:engine_build>;ENGINE_REQUIRE_ADDON_TOOL_TESTS=1")
        add_dependencies(addon_protocol_test engine_build)
    endif()
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
    # The two OPAQUE ozz archives inside a cooked mesh. fuzz_mesh_loader_test
    # fuzzes the container — header, counts, strides — and stops at the blob,
    # which is opaque to it and never decoded. So the container was fuzzed and
    # the two third-party deserializers reading out of it were not.
    # engine_runtime: cooked_skin.h pulls in ozz + the animation headers.
    engine_fuzz_target(fuzz_cooked_skin_test cooked_skin LIB engine_runtime)
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
# The kit MODULES are the fixture, and they are built from separate, gitignored
# repos under Kits/ — so project.json existing was never the right condition.
# It is committed, which meant the test registered everywhere (CI included) and
# then failed on a fixture that could not possibly be there. Nobody noticed
# because CI ran only the docs lane; the moment any lane gates on `-L unit`, a
# permanently-red test makes the gate worthless.
#
# Same rule the asset lane already uses: registered only when present.
set(KIT_TEST_PROJECT ${CMAKE_SOURCE_DIR}/fps_shooter)
set(KIT_TEST_MODULE
    ${CMAKE_SOURCE_DIR}/Kits/CombatKit/build/libcombat_kit_module.so)
if(EXISTS ${KIT_TEST_PROJECT}/project.json AND EXISTS ${KIT_TEST_MODULE})
    engine_test(kit_lifecycle_test EXPORTS ARGS ${KIT_TEST_PROJECT})
elseif(EXISTS ${KIT_TEST_PROJECT}/project.json)
    message(STATUS "kit_lifecycle_test: built but NOT registered — no kit module at "
                   "${KIT_TEST_MODULE} (build the Kits repos to enable it)")
    add_executable(kit_lifecycle_test kit_lifecycle_test.cpp)
    target_link_libraries(kit_lifecycle_test PRIVATE engine_runtime)
    set_target_properties(kit_lifecycle_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} ENABLE_EXPORTS ON)
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
# The job facade under contention: sustained churn, EXTERNAL threads (BUG-0003's
# livelock shape), nested waits, work distribution, exact parallelFor coverage on
# a saturated pool, and the main-thread channel. Written against jobs.h and never
# against enkiTS, so it doubles as the acceptance gate for the FiberTaskingLib
# swap the facade was shaped for.
engine_test(stress_jobs      LABEL stress TIMEOUT 600)
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

    # ── Missing standard headers, caught here instead of on a Linux runner ────
    # libc++ (macOS) pulls much of the standard library in transitively;
    # libstdc++ (Linux) does not. So `std::memcpy` with no <cstring>, or
    # `std::string` with no <string>, builds fine on the development machine and
    # fails on every Linux leg. Three separate CI round-trips went to exactly
    # that, ONE ERROR AT A TIME, because a build stops at the first failure:
    # four files missing <cstring>, then <string> in gpu_budget.cpp, and no
    # reason to think that was the last. The next 25 were found by this script in
    # one pass.
    #
    # `unit`, not `docs`: it guards whether the tree COMPILES, and it runs on
    # every leg because that is nearly free and the gating leg is the one that
    # cannot see the problem it protects against.
    add_test(NAME std_includes
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/scripts/check_std_includes.py)
    set_tests_properties(std_includes PROPERTIES LABELS unit TIMEOUT 120)

    # ── G1's guard ──────────────────────────────────────────────────────────
    # Nothing outside src/render/ may include a graphics API. The five files
    # that used to parse assets and upload them in the same function are the
    # reason a headless server, a second backend and an embedding host were all
    # blocked at once (docs/rhi/evidence-coupling.md 2.1).
    #
    # Removing that was the easy half. This is the half that keeps it removed,
    # because the coupling does not return as a decision — it returns as one
    # #include added to fix one compile error, in a file nobody thinks of as
    # graphics code. A grep is the only thing that can see that, and a grep
    # nobody runs is a comment.
    add_test(NAME gpu_seam
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_SOURCE_DIR}/scripts/check_gpu_seam.py)
    set_tests_properties(gpu_seam PROPERTIES LABELS unit TIMEOUT 120)
endif()

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
# BOTH TARGETS ARE FORCED TO -O2, whatever the build type. This is not a
# preference: the default dev build here is Debug (-g, no -O), and at -O0
# nothing inlines, so every variant costs the same and the benchmark reports
# noise. The first run of this harness in a Debug tree said a cross-.dylib call
# was FASTER than a direct one, which is the shape of a measurement that is
# measuring the build flags instead of the thing.
add_library(seam_cost_plugin SHARED perf/seam_cost_plugin.cpp)
target_compile_options(seam_cost_plugin PRIVATE -O2)
set_target_properties(seam_cost_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_executable(seam_cost_bench perf/seam_cost_bench.cpp)
target_compile_options(seam_cost_bench PRIVATE -O2)
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

# ── G1c: runtime.h must not REACH a graphics API ────────────────────────────
# The grep above catches a direct #include. It cannot catch the coupling that
# actually mattered, which was transitive: runtime.h -> render/renderer.h ->
# bgfx handle members, and before that mesh.h, and before that vertex.h through
# Vertex::layout(). Each was found by walking `-H` output by hand.
#
# So this target is compiled with every engine include directory EXCEPT bgfx's.
# If anything reachable from runtime.h or the pipeline seam includes
# <bgfx/bgfx.h>, it fails with "file not found" — the compiler finds all of them
# at once, and keeps finding them.
#
# It deliberately does NOT link engine_runtime: that would inherit bgfx through
# the PUBLIC include directories, which is the very thing being tested. The
# include list is therefore spelled out, and a new engine-wide include dir may
# need adding here — a build failure that says "file not found" for an ENGINE
# header means this list, not a real coupling.
#
# bx is on the list on purpose: it is bgfx's MATH library and is explicitly out
# of G1's scope (docs/rhi/evidence-coupling.md 2.1).
add_executable(headless_include_probe headless_include_probe.cpp)
set_target_properties(headless_include_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_compile_definitions(headless_include_probe PRIVATE BX_CONFIG_DEBUG=1)
target_include_directories(headless_include_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/components
    ${CMAKE_SOURCE_DIR}/src/render
    ${CMAKE_SOURCE_DIR}/modules/assetlib/include
    ${CMAKE_SOURCE_DIR}/modules/hid/include
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann
    ${CMAKE_SOURCE_DIR}/third_party/cgltf
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${CMAKE_SOURCE_DIR}/third_party/tlsf
    ${CMAKE_SOURCE_DIR}/third_party/flecs/distr
    ${CMAKE_SOURCE_DIR}/third_party/enkiTS/src
    ${CMAKE_SOURCE_DIR}/third_party/ozz-animation/include
    ${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake/bx/include
    ${CMAKE_SOURCE_DIR}/third_party/bgfx.cmake/bx/include/compat/osx
)
add_test(NAME headless_includes
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                 --target headless_include_probe)
set_tests_properties(headless_includes PROPERTIES LABELS unit TIMEOUT 300)

# ── Cooked-format conformance (Rust) ────────────────────────────────────────
# A .cooked file is what a SHIPPED GAME reads, which makes its byte layout an ABI
# — and it had only ever been checked by the C++ that writes it. A
# saveMesh/loadMesh round-trip proves the two agree with each other; both could
# move together and nothing would notice.
#
# This crate parses the layout from outside, with no engine code, and drives the
# real cooker through engine_cook_worker's PROCESS CLI. Both are genuine
# boundaries, so no C shim exists purely to be tested — which is the condition
# that makes a Rust test the right choice here rather than a preference.
if(ENGINE_WITH_RUST AND NOT ENGINE_SANITIZE AND TARGET engine_cook_worker)
    find_program(CARGO_EXECUTABLE cargo)
    if(CARGO_EXECUTABLE)
        # ENGINE_REQUIRE_COOK_TESTS turns a SKIP into a FAILURE. The tests that
        # cook a real asset and read the bytes back are the only ones that can
        # catch the writer drifting — every other test in the crate builds
        # structs by hand — and each of them used to print a reason and return.
        # `cargo test --quiet` captures stdout, so a lane that had silently
        # stopped exercising anything reported `ok` exactly like one that
        # passed. Here the build directory, the worker and the shaders are all
        # known to exist, so a skip is a finding rather than a shrug.
        add_test(NAME cooked_format_conformance
                 COMMAND ${CMAKE_COMMAND} -E env
                         ENGINE_BUILD_DIR=$<TARGET_FILE_DIR:engine_cook_worker>
                         ENGINE_REQUIRE_COOK_TESTS=1
                         ${CARGO_EXECUTABLE} test --quiet
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests/cooked_format)
        set_tests_properties(cooked_format_conformance PROPERTIES
                             LABELS unit TIMEOUT 300)
    endif()
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
# Excluded from the sanitizer lanes, deliberately. The suite is a CARGO-built
# binary, so it never gets CMake's -fsanitize flags — and it dlopens a provider
# module that DOES have them. ASan then reports "Interceptors are not working…
# loaded too late (e.g. via dlopen)" and aborts, which is a fact about the
# harness rather than about the engine. Instrumenting the Rust side needs
# nightly's -Zsanitizer; the C provider is covered by the C++ lanes instead.
if(ENGINE_WITH_RUST AND NOT ENGINE_SANITIZE)
    find_program(CARGO_EXECUTABLE cargo)
    if(CARGO_EXECUTABLE)
        add_test(NAME audio_abi_conformance
                 COMMAND ${CMAKE_COMMAND} -E env
                         ENGINE_AUDIO_PROVIDER=$<TARGET_FILE:engine_audio_miniaudio_module>
                         # No CI runner has dependable audio hardware. On Linux
                         # ALSA finds no card and hands back a working 48 kHz
                         # dummy device anyway, whose callback cadence is whatever
                         # the scheduler felt like — so the provider must not
                         # assert hardware TIMING there, and must be allowed to
                         # fall back to miniaudio's null backend where there is
                         # no device at all. See maCreate. A shipped game never
                         # sets this: a real device failure must stay visible.
                         ENGINE_AUDIO_NO_HARDWARE=1
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

# ── Module-ABI gate conformance (Rust + fixture modules) ────────────────────
# `ModuleLibrary::load` refuses a module for five reasons — struct size, API
# version, ABI fingerprint, component layout hash, kit-to-kit contract — and
# until now not one of them had ever executed under test.
#
# That gap is not the same shape as an untested function. api_abi_compat_test
# pins the function-table LAYOUTS thoroughly, and a layout regression is loud:
# an offset assert fires at compile time. A GATE regression is silent. Reorder
# the checks, invert a comparison, replace a `return refuse()` with a bare
# `return false` that skips destroy(t), and nothing fails and nothing prints —
# the engine simply starts accepting a module it was built to reject, and the
# symptom arrives later as corrupted ECS memory after a hot-reload.
#
# kit_lifecycle_test cannot cover it. It needs .so files from the gitignored
# Kits repos, so on a clean checkout it is built but NOT registered (see above)
# and CI has never run it. These fixtures depend on nothing outside the tree.
#
# One source, one module per defect, selected by -DABI_GATE_DEFECT. Six separate
# fixtures would drift: the good one gets updated when the table grows and the
# five bad ones go stale, still failing, still green, now for the wrong reason.
set(ABI_GATE_FIXTURES
    good          0
    struct_size   1
    api_version   2
    fingerprint   3
    layout_hash   4
    no_exports    5
    null_table    6
    contract_v1   7
    contract_v2   8
    # The one fixture whose defect is not in its ABI table. Its table is
    # correct and it MUST load; what it attacks is the channel the verdicts
    # used to travel on, by printing lines indistinguishable from the probe's
    # own. That is what moved the probe's machine output off stdout and into a
    # framed Add-on result file.
    noisy_stdout  9
)
set(_abi_gate_targets "")
list(LENGTH ABI_GATE_FIXTURES _abi_gate_len)
math(EXPR _abi_gate_last "${_abi_gate_len} - 1")
foreach(_i RANGE 0 ${_abi_gate_last} 2)
    list(GET ABI_GATE_FIXTURES ${_i} _name)
    math(EXPR _j "${_i} + 1")
    list(GET ABI_GATE_FIXTURES ${_j} _defect)
    set(_t abi_gate_${_name})
    add_library(${_t} MODULE fixtures/abi_gate_module.cpp)
    # engine::headers, never engine_runtime: linking the runtime into a module
    # duplicates engine state, and the SDK header says so. These fixtures go
    # further and reference NO host symbol at all — ecs_world_t* appears only as
    # an opaque pointer and no flecs function is called — so unlike the
    # hot_reload_game sample they have no undefined symbols and need no
    # load-time resolution against the host. That is what lets them build on
    # every platform, Windows included.
    target_link_libraries(${_t} PRIVATE engine::headers)
    target_compile_definitions(${_t} PRIVATE ABI_GATE_DEFECT=${_defect})
    set_target_properties(${_t} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/abi_gate
        DEBUG_POSTFIX ""        # the Rust side builds paths by name
    )
    list(APPEND _abi_gate_targets ${_t})
endforeach()

if(ENGINE_WITH_RUST AND NOT ENGINE_SANITIZE AND TARGET engine_module_probe)
    find_program(CARGO_EXECUTABLE cargo)
    if(CARGO_EXECUTABLE)
        add_test(NAME module_abi_conformance
                 COMMAND ${CMAKE_COMMAND} -E env
                         ENGINE_MODULE_PROBE=$<TARGET_FILE:engine_module_probe>
                         ENGINE_ABI_GATE_DIR=${CMAKE_BINARY_DIR}/abi_gate
                         # Same contract as the cooked-format lane: here the
                         # probe and every fixture are known to exist, so a skip
                         # is a finding rather than a shrug.
                         ENGINE_REQUIRE_MODULE_ABI_TESTS=1
                         ${CARGO_EXECUTABLE} test --quiet
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests/module_abi)
        # The fixtures are ordinary ALL targets, so a plain `cmake --build`
        # produces them and ctest finds them already there. ENGINE_REQUIRE_
        # MODULE_ABI_TESTS above is what stops a missing one from passing
        # quietly: ctest does not chase target dependencies, so "the fixture was
        # never built" has to be a failure rather than a skip.
        set_tests_properties(module_abi_conformance PROPERTIES
                             LABELS unit TIMEOUT 300)
    else()
        message(STATUS "module_abi_conformance: cargo not found — module ABI "
                       "gate conformance DISABLED (install rustup to run it)")
    endif()
endif()
