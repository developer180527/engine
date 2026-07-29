# Infrastructure Backlog & Execution Plan

*Last updated: July 2026. The single source of truth for deferred engine
work. Items graduate out of here into commits; nothing gets re-litigated
without new information.*

Recently shipped (for context): jobs pool (enkiTS/FTL-swappable), tagged
memory manager, hid module + InputManager (raw/hybrid/actions/snapshots),
fixed timestep + render interpolation, EngineApiTableV1 (per-subsystem
versions), contract gauntlet, SerdeTransient, IAnimService extraction,
tier-3 game module.

---

## Ragebait stress-test suite (SHIPPED — src/tests/stress_*, July 2026)
Adversarial load/torture harnesses to find vulnerabilities the happy-path FPS
never exercised. Findings:
- **stress_deep_tree** → FOUND + FIXED a crash: flecs HARD-ABORTS on a ChildOf
  chain deeper than FLECS_DAG_DEPTH_MAX (128). Fixed with `safeReparent`
  (cycle + depth guard) on every parenting site — a deep scene/script can't
  crash the engine.
- **stress_assets** → FOUND + FIXED a crash: NaN/astronomical nav geometry made
  Recast compute an INT_MAX² cell grid → OOM. NavService::build now rejects
  non-finite bounds / >64M-cell grids.
- **stress_swarm** → MEASURED the deferred allocator-contention item (H #33):
  12-thread concurrent alloc ran at **0.32×** (per-tag TagHeap lock). FIXED —
  tag heaps now striped across 16 per-shard locks: 0.32x -> 0.87x, the ~3x
  slowdown gone (mem.cpp). ECS scales fine (50k ents @ 0.6ms/tick).
- **stress_physics** → STABLE: 2000-body pile, no NaN/tunnel; 22.8ms/step (Debug)
  = physics is the frame-budget wall at high body counts.
- **stress_churn** (+ `--soak`) → CLEAN: 40M create/destroy ops, memory dead
  flat, zero leak. Memory manager + flecs + event sweeper robust under churn.
- TODO (measurable, not yet built): live GPU swarm to measure the no-instancing
  draw-call cliff (headless can't see it).

## Navigation + AI (SHIPPED — engine nav infra + AIKit, July 2026)
- **NavService** (runtime/services/nav_service.*): Recast bake + Detour
  findPath/projectPoint; nav_test proves routing around obstacles. Exposed
  to kits over the `nav` C-API group (engineNav*), capability-negotiated.
- **AIKit** (Kits/AIKit, own repo): Faction/AiAgent contract, pluggable IBrain
  + FsmBrain (Idle/Chase/Attack/Search), perception (raycast LOS) + nav-steering
  (direct-steer until a mesh is baked) + melee via combat::dealDamage. Links
  nothing — pure C API. Zombie wired with AiAgent + Faction + CharacterController
  (the capsule doubles as its hitbox — fixes the old "shots hit scenery" issue).
- **Debug-draw split**: its own `draw` C-API group (renderer capability), no
  longer gated behind the editor widget backend.
- REMAINING: navmesh bake from real scene geometry (source decision: Jolt
  static bodies / nav-tagged meshes / offline cook); health bars; player
  mortality + game-over; attack animation.

## Phase A — Gameplay loops (DEFERRED by user, July 2026 — infra first)
1. ~~Kinematic hitbox follow~~ SIDESTEPPED — the zombie uses a
   CharacterController (Jolt CharacterVirtual), which both moves it and IS its
   hitbox capsule; no separate kinematic-body follow needed for enemies.
2. ~~Zombie chase AI~~ SHIPPED via the AIKit (see Navigation + AI above);
   health bars still pending.
3. **Editor scroll bug** — repro the intermittent trackpad/wheel scroll loss
   in the editor (suspect: ImGui NoMouse gate or hybrid scroll routing).
4. **Inspector world-space shape preview** — show effective (scale-applied)
   collider size next to raw values; the Mixamo 0.01 footgun.
5. **`engineActionId` fast path** — compiled action handles; query-by-id
   variants (input group v3).

## Phase B — Asset pipeline maturity (ACTIVE — cooker first, then C)
6. **Animation cooker** — ozz archive serialization; runtime loads cooked
   skeletons/clips (mmap-and-go, no Assimp at runtime). Kills the sync-parse
   hitch class permanently.
7. **Async clip/asset bind** — jobs-pool IO path + "pending" clip state.
8. ~~Skinned-mesh cooking~~ SHIPPED COMPLETE (v3: bones + ozz skeleton/clip
   archives + embedded textures extracted to sibling .ctex files; zombie
   renders textured + animated with zero runtime Assimp).
9. **AsyncLoader onto the job pool IO channel**; parallel per-asset cooks.

## Phase C — Input completion
10. ~~Input recorder~~ SHIPPED — .irec tee of accepted events + tick marks
    (engine_host --record-input); replay through a fresh manager proves
    bit-identical snapshots in input_test.
11. ~~LatencyTracker~~ SHIPPED — InputLatencyChannel: queue avg/max,
    device→tick, device→look per frame; engine_host periodic dump.
12. ~~Sub-tick timestamps~~ SHIPPED — SubTickEdge[16] per snapshot (us
    offsets from tick start) + actionPressedOffsetUs; microsecond-exact in
    input_test. C API exposure when a kit consumer exists (input v3).
13. **Gamepad curation layer** (deferred by user; SEAMS MARKED: 'pad:'
    binding parser stub in input_manager.cpp, keymap tables + capture flow
    marked TODO(#13) in hid_keymap.h / input_bindings_panel.h; hid already
    delivers raw Gamepad Axis/Button events).
14. ~~Editor rebinding UI~~ SHIPPED — Input Bindings panel (View > Panels):
    edits input.json with per-binding text + key-capture, Save & Apply
    hot-reloads the manager mid-play.
15. ~~Legacy retirement~~ SHIPPED — FPSKit 100% action-based (ToggleCursor
    action replaces Escape polling; capture-click via Fire), hosts' dead
    InputMap bindings removed, engineKey*/engineMouse* marked DEPRECATED in
    the header + warn-once at runtime. InputSystem remains editor-internal.

## Phase D — Event model + service architecture (from the API reviews)
16. ~~Explicit event model~~ SHIPPED — EventComponent + EventSweeper mark/
    sweep (components/event_component.h, runtime/event_sweeper.h): events::
    declare<T> (implies SerdeTransient), a message written anywhere in a tick
    is guaranteed observable for the whole NEXT tick regardless of broadcast
    order, drains via events::consume<T> (no orphaned staleness marker),
    self-expires unconsumed. DamageInbox migrated; Died stays transient STATE.
    event_test proves the lifecycle incl. re-fire-after-drain.
17. ~~entity_t normalization~~ SHIPPED — IPhysicsService/IAnimService take an
    explicit (flecs::world&, flecs::entity_t) instead of a world-bundling
    flecs::entity; ScriptHost splits at the boundary (public API + C API + Lua
    unchanged), Jolt/AnimService impls updated. (Continue ScriptHost →
    coordinator / extract Asset/Scene service fronts as they grow — remaining.)
18. ~~Live capability negotiation~~ SHIPPED — engineApiHostTable re-publishes
    ui.version per bind (backend present ? UI_V : 0); v0 = ABSENT is now a
    silent, expected state at bind (no false "version mismatch" warning);
    modules adapt via engineApiHas. ui is the exemplar for optional groups.
19. **Kit param editor UI**; menubar/context-menu contribution API;
    engineKitAnnounce for embedded kits (registry-of-record).
20. **Lua parity** — action + anim bindings mirroring the C API.

## Phase E — Frame pacing & renderer
21. **Render-queue depth control / frame pacing** (Reflex-style) — the
    single biggest motion-to-photon lever left; pairs with LatencyTracker.
22. **GPU profiler channel + chrome-trace export** (profiler P3);
    editor profiler overlay (P2).
23. **Renderer quality pass** — PBR/multi-material, texture/material
    authoring (deferred by user until more game infra exists).
24. **bgfx shutdown RefCount warnings sweep** (resource lifetime hygiene).

## Phase F — Windows/Linux port (IN PROGRESS — see `.github/workflows/ci.yml`)
CI matrix is live: macOS gates, Linux (GCC+Clang) and Windows (MSVC) run
non-fatally so they report a fixable error list instead of a red badge. Flip
`experimental: false` per platform the moment it first passes — from then on it
is a ratchet. `docker/linux-build.Dockerfile` checks Linux from a Mac in
seconds. Survey (2026-07-29): engine C++ is *clean* — one non-portable
construct in all of `src/`+`modules/`, and it's in a vendored ImGui backend.
Only 10 files touch POSIX-only headers and most already carry the Win32 side.

25. ~~Memory backend: VirtualAlloc2 aligned reservations~~ — **DONE**;
    `mem.cpp` has the `MEM_RESERVE`/`MEM_COMMIT` over-reserve + trim path.
26. **Raw-input backends.** `hid_null` is the fallback on Windows/Linux today
    (engine builds and runs, no gamepad/raw input). **SDL3 is now wired as an
    optional window backend** (`-DENGINE_WINDOW_BACKEND=sdl3`,
    `src/runtime/platform/sdl3_platform.*`) — verified: `sample_minimal` opens
    an SDL3 window with bgfx/Metal on it. Next, in order:
    - **Measure before scoping.** SDL3 stamps events with OS-level ns
      timestamps and uses RawInput internally for relative mouse on Windows,
      so it *may* satisfy the sub-tick/late-latch requirements. A/B it against
      the working IOHIDManager backend on macOS. If it measures equal, Win32
      RawInput and Linux evdev are **deleted from this list**; if not, SDL3
      stays window+gamepad only and the native backends proceed as designed.
    - `hid_sdl3.cpp` gamepad backend — the unambiguous win (mapping database,
      hotplug, rumble, battery, DualSense gyro/touchpad). Note SDL's event
      queue is main-thread-bound (Cocoa), so it cannot park a thread in the OS
      wait primitive the way IOHIDManager does; fine at gamepad rates.
    - **Editor on SDL3** is gated off in CMake: it needs the ImGui SDL3
      platform backend wired (with event forwarding out of
      `Sdl3Platform::pollEvents`) plus an SDL3 window input source to replace
      the GLFW-callback `InputSystem`. `src/editor/window_ops_sdl3.cpp` is
      already written (incl. the Key→SDL_Scancode table) and syntax-clean.
    - Selecting sdl3 today swaps the WINDOW only; GLFW stays linked because
      `runtime/input/input_system.h` is still the window input source.
27. ~~Optional-target CMake fixes~~ — **DONE**: `ENGINE_BUILD_{EDITOR,TOOLS,
    SAMPLES}` + `BUILD_TESTING`. Remaining: fs_triangle HLSL variants (texture
    sample inside the shadow loop); MSVC flags (**`/utf-8` is the sneaky one** —
    the tree is full of `──`/`→` in comments; also `/permissive-`,
    `/Zc:preprocessor`, `/bigobj`, and an `/O2` branch for the cook TUs that are
    `-O2`-pinned only for Clang/GNU today); Linux **case-sensitivity** sweep
    (APFS hides wrong-case includes and asset paths); profiler clock seam;
    `engine_host` hardcodes `.dylib` in its usage text.
    (Module loading is already table-based — the dynamic_lookup dependency is
    gone. **Verify early:** `samples/hot_reload_game` still passes
    `-undefined dynamic_lookup` under `if(APPLE)`. Build it *without* that flag;
    if it links, Windows kits are unblocked. If it doesn't, C++ kits need
    Phase H #32 — and the Windows port is that trigger.)

## Phase G — Export milestone (DELAYED FURTHER: after enough Kits + a
## robust single-genre-capable engine)
28. **engine_player** — generic runtime binary (engine_host minus dev/hot-
    reload machinery).
29. **engine_build** — packaging CLI producing `<project>/build/`: player
    binary + kits + cooked assets + input.json + scenes.
30. **Prefabs** — formalize scene-entity-as-template (the spawner pattern).

## Phase H — Triggered, not scheduled (do when the trigger fires)
31. **Multi-instance runtimes** (kill g_host/g_input globals; per-instance
    API tables) — trigger: PIE/second viewport/dedicated server/test harness.
32. **Engine core as shared lib** — trigger: first real cross-boundary
    allocator incident, or C# hosting. Removes the kit container rule.
33. **Memory: per-project budgets + editor Memory panel; per-thread frame
    arenas; guard-page forensics** — trigger: first real memory hunt.
34. **FTL fiber backend swap** — trigger: job graphs deep enough that
    blocking waits dominate (facade is ready).
35. **Contract-as-versioned-assets with migrations** — trigger: first
    third-party kit or breaking contract change in the wild.
36. **C# scripting (CoreCLR/hostfxr)**; **netcode** on the InputSnapshot
    foundation — after export milestone.

---

## Sequencing rationale
- ~~A before everything~~ (user re-prioritized: essential systems first;
  gameplay when the engine is reliably game-capable) — **B then C**: tiny debts that make daily testing pleasant and
  the game real; each item exercises infrastructure built this month.
- **B before G**: export without cooked animation/meshes ships Assimp and
  source FBX files — the cooker IS most of the export work.
- **C and D are parallel-friendly** (input vs. scripting layers barely
  touch); order by appetite.
- **E after C**: LatencyTracker gives frame-pacing work its measuring stick.
- **F bundles naturally**: every Windows blocker is listed and three were
  already dissolved by this month's work (API table, jobs, input module).
- **H items have explicit triggers** — building them early is speculation,
  and the reviews agreed speculation is how god objects and dead layers
  are born.

## Export milestone (SHIPPED — engine_player + engine_build, July 2026)
- **engine_player** (tools/engine_player.cpp): the shipped-game runtime —
  cooked content only, no dev machinery. Under the shipping posture
  (ENGINE_WITH_SOURCE_IMPORTERS=OFF) it links ZERO Assimp: 5.1MB Release
  binary, 0 assimp symbols (dev Debug: 32MB, 19,259).
- **engine_build** (tools/engine_build.cpp): cook → shipping player → kits
  REBUILT Release (kit binaries are per-config artifacts: Debug kits
  reference debug-only symbols like ecs_assert_log_ — mixing configs fails
  at dlopen, verified) → reference-walked dist assembly. fps_shooter:
  3,422MB naive copy → 38.7MB closure. Game RUNS from dist (4/4 kits,
  FPSKit firing, sim live).
- **flecs + hot-reload ABI**: FLECS_NO_ALWAYS_INLINE on flecs_static —
  always_inline API fns with bodies in flecs.c emit no out-of-line copy at
  -O2, so Release hosts lacked symbols kits dlopen (ecs_children_w_rel).
- REMAINING (found BY the pipeline, tracked):
  1. MeshCooker has no .gltf support (cgltf-based cook) — gltf scene meshes
     (pistol, warehouse) invisible in ship builds until cooked.
  2. AssetService cooked streaming rejects skinned meshes (stride 68 vs 48)
     — the zombie streams via the editor's AsyncLoader path only. Add
     SkinnedVertex layout to the cooked-stream loader.
  3. Cooked textures are raw RGBA32 (64MB per 4k texture) with no runtime
     consumer — BCn compression + material binding when the texture
     pipeline matures.
