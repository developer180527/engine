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

## Phase A — Gameplay loops (DEFERRED by user, July 2026 — infra first)
1. **Kinematic hitbox follow** — push Transform → Jolt for kinematic bodies
   each step (reverse of the existing writeback). Unblocks moving enemies.
2. **Zombie chase AI + health bars** — seek-toward-player velocity + debug-
   draw HP bars. Makes fps_shooter a game; exercises the whole stack.
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
13. **Gamepad curation layer** — GCController/XInput → stable pad model over
    raw hid Axis events.
14. **Editor rebinding UI** over input.json.
15. **Legacy retirement** — InputSystem/InputMap polling shrinks to
    editor-internal; engineKey*/engineAxis deprecated in favor of actions.

## Phase D — Event model + service architecture (from the API reviews)
16. **Explicit event model** — transient ECS components formalized (declare,
    auto-clear, ownership rules) replacing convention (DamageInbox pattern).
17. **entity_t normalization** in service interfaces; continue ScriptHost →
    coordinator (extract Asset/Scene service fronts as they grow).
18. **Live capability negotiation** — headless hosts publish absent API
    groups (version 0); modules adapt via engineApiHas.
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

## Phase F — Windows/Linux port (EXPLICITLY DELAYED — much later)
25. Memory backend: **VirtualAlloc2** aligned reservations (mmap is POSIX).
26. **Win32 Raw Input/GameInput** hid backend (module drop-in, engine
    untouched). Linux evdev alongside if desired.
27. Shaders: fs_triangle HLSL variants; optional-target CMake fixes; dlfcn
    remnants; profiler clock seam. (Module loading already table-based —
    the dynamic_lookup dependency is gone.)

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
