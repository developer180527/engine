---
status: unreviewed
---
# Runtime Core — Issue Report

Covers `runtime.cpp`/`runtime.h`, `event_sweeper.h`, `kit_host.h`, `module_loader.h`, `mem_channel.h`, `plugin_registry.h`, `plugin.h`, `runtime_context.h`, and `world_query_cache.h`. Findings are grouped by severity, each with what's wrong, why it matters, and how to test for it.

---

## Critical

### C.1 — `buildDefaultScene()`: likely typo, `.set<n>()` instead of `.set<Name>()`

**What's wrong:**
```cpp
m_ecs.entity(name)
    .set<Transform>(t)
    .set<MeshRenderer>({cubeHandle})
    .set<n>({name})              // <-- should almost certainly be Name
    .set<Spinner>({0.3f, 0.1f});
```
`components/name.h` is included at the top of `runtime.cpp`, and `Name` is used as a real component type elsewhere in the codebase (`RuntimeContext` and friends). There is no local variable or type named `n` in scope in this function.

**Why it matters:** Either this fails to compile outright, or — if `n` unexpectedly resolves to something else — the default scene's entities silently get the wrong component attached. This is the flagship demo scene every new project boots into; if it's broken, it's broken for everyone on day one.

**How to test:** Compile `buildDefaultScene()` in isolation, or add an assertion after scene construction that every generated grid entity has a valid `Name` component with the expected string.

---

## High

### H.1 — Snapshot-mode Play: the simulated world's spinners never rotate, and the editing world runs pointlessly in parallel

**What's wrong:** `tickSystems()` — called unconditionally from the game-facing `tick(dt)` — hardcodes three pieces of per-frame logic against `m_ecs` specifically, never `simWorld()`:
```cpp
m_spinnerQuery.each(...);              // built via m_ecs.query_builder(...) in initSystems()
m_animatorSystem.tick(dt);             // no world arg — implicitly targets m_ecs
m_ecs.progress();
```
Separately, `tickSimulation()` does:
```cpp
if (m_gameWorld) {
    m_animatorSystem.tick(*m_gameWorld, dt);
    m_gameWorld->progress(dt);
}
```
only when a Snapshot-mode game world exists.

**Why it matters:** `info.md` confirms the editor's Play button uses Snapshot mode, meaning the world actually rendered and simulated during Play is `m_gameWorld` — but the spinner-rotation query and one of the two animator ticks only ever touch `m_ecs`, the original editing world nobody is looking at. Result: demo Spinner entities freeze the moment Play starts, while the editing world keeps needlessly progressing and animating in the background every frame. The `tickSimulation` comment shows awareness of half this problem ("In-place mode: simWorld() == m_ecs, which tickSystems already animates... don't double-tick") but the Snapshot branch of the same logic was never routed to the correct world.

**How to test:** Build the default scene, call `startSimulation(SimMode::Snapshot)`, tick several times, and assert that Spinner-owning entities in the *rendered* world (`simWorld()`) actually rotated. This is a two-line regression test that would have caught it immediately.

---

### H.2 — `KitHost::poll()`: a failed hot-reload leaves a null plugin inside `m_loaded`

**What's wrong:**
```cpp
e->plugin->onSimulationStop();
e->plugin->onDetach();
reg.remove(e->plugin.get());
e->plugin.reset();
e->lib.unload();

if (!e->lib.load(e->path)) {
    LOG_ERROR("Kit", "reload failed: '%s'", e->name.c_str());
    continue;              // Entry stays in m_loaded with plugin == nullptr
}
```
If the rebuilt module fails the ABI/contract gauntlet in `ModuleLibrary::load()`, the old plugin has already been fully torn down, but the `Entry` is never removed from `m_loaded`, and `plugin` is never reassigned.

**Why it matters:**
- `stop()` unconditionally calls `e->plugin->onDetach()` for every entry in `m_loaded` — a null-pointer dereference the next time simulation stops.
- `isLoaded(name)` only checks whether an `Entry` with that name exists, not whether its `plugin` is valid, so it keeps reporting the kit as loaded. That makes `loadOne()` silently no-op (`if (isLoaded(name)) return true;`) even though nothing is actually running, and the Plug-in Manager UI shows a dead kit as healthy.

**How to test:** Force a hot-reload to fail the ABI gauntlet (e.g. touch the module's `structSize`), confirm `poll()` handles it without leaving a null-plugin entry, and confirm `stop()`/`isLoaded()` behave correctly afterward.

---

### H.3 — `module_loader.h`: temp module filenames can collide across concurrent processes

**What's wrong:**
```cpp
m_tempPath = fs::temp_directory_path()
           / ("engine_module_" + std::to_string(nextTempId()) + ext);
```
`nextTempId()` is a per-process atomic counter starting at 0.

**Why it matters:** Two engine instances running at once — two editor windows, or a CI matrix running tests in parallel — can both compute `engine_module_0.so` (or `.dylib`/`.dll`) in the same shared system temp directory at close to the same moment. Worst case isn't just a load failure; it's one process's `fs::copy_file` racing another's `libOpen` on the identical path.

**How to test:** Launch two engine instances simultaneously, each loading kits, and confirm no cross-contamination or load failures under load. Fix is cheap: include the PID or a GUID in the temp filename.

---

## Medium

### M.1 — Static-init order fragility around the flecs allocator hook

**What's wrong:** `const bool g_flecsHooked = hookFlecsAllocator();` relies on running before any `flecs::world` is constructed. This works today because `EngineRuntime` (and its member world) is built inside `main()`, after all translation units' static initialization has completed — but that's a guarantee about *`main()` running after all statics*, not about *this particular global's initializer running before other globals'*.

**Why it matters:** If any other translation unit ever declares its own global/static-duration `flecs::world` (a test fixture, a standalone tool), whether its allocations get captured by the tagged-heap hook depends on unspecified cross-TU static-init order. Not an active bug today, but a landmine for the next person who adds a static world elsewhere.

**How to test:** Not easily unit-testable directly; best mitigated by converting to a function-local static (lazy-init on first use) so the ordering question doesn't exist, or by a code-review rule flagging any new global-duration `flecs::world`.

---

### M.2 — No lifecycle guard ensuring `attachPlugins()` ran before `startSimulation()`

**What's wrong:** The class is otherwise careful about loud lifecycle enforcement (`m_initialized`, the `m_pluginsAttached` double-call guard), but nothing stops `startSimulation()` from being called before `attachPlugins()`.

**Why it matters:** `broadcastSimStart` would fire `onSimulationStart` on plugins whose `onAttach()` never ran, silently violating the documented lifecycle order (`onAttach` → `onSimulationStart`) instead of failing loudly, which is inconsistent with the class's own stated philosophy of loud errors over silent corruption.

**How to test:** Call `startSimulation()` without calling `attachPlugins()` first and confirm the engine either refuses with a clear error or logs a loud warning, rather than proceeding silently.

---

### M.3 — `EventSweeper::rebuild()` trigger misses same-size composition changes

**What's wrong:**
```cpp
if (w.c_ptr() != m_world || reg->types.size() != m_queries.size())
    rebuild(w, *reg);
```
This detects a change in the *count* of declared event types, not a change in *which* types are declared.

**Why it matters:** If an event type is ever unregistered and a different one registered in its place while the total count stays constant, the cached queries would go stale — silently pointing at component types no longer meaningfully tracked — without triggering a rebuild.

**How to test:** Register events A, B; unregister B; register C (count stays at 2); confirm the sweeper picks up C correctly rather than silently continuing to sweep the stale B-bound query.

---

## Low

### L.1 — `openProject()`'s pre-init check piggybacks on an unrelated pointer's nullness

**What's wrong:** Distinguishing "init() calling this internally" from "the user called it before init() ever ran" is done by checking `m_assetService`'s nullness rather than an explicit "systems ready" flag.

**Why it matters:** Works today, but it's an implicit protocol riding on a member whose nullness could change for unrelated reasons in a future refactor, silently breaking the check.

**How to test:** Not urgent; worth a dedicated `m_systemsReady` bool if `initSystems()`'s internals ever change.

---

### L.2 — Failed kit reload attempts accumulate graveyard images and temp files faster than the accepted "successful unload" cost

**What's wrong:** Every refused module load (failed ABI/contract gauntlet) still pushes its `LibHandle` into the process-lifetime graveyard via `unload()`, and never deletes its temp-copied file.

**Why it matters:** This is a known, deliberately accepted cost for *successful* load-then-unload cycles (documented in `module_loader.h`), but a developer iterating on a kit that's repeatedly failing the gauntlet (build, reload-fails, fix, build again) accumulates one of each per attempt — a faster-growing version of an already-accepted tradeoff, worth knowing about during a long debugging session.

**How to test:** Not a correctness issue; worth tracking graveyard size / temp directory growth during a simulated rapid-iteration failure loop, just to have a number.

---

### L.3 — Forward-looking: memory counters are plain (non-atomic) globals

**What's wrong:** `ecs_os_api_malloc_count` and friends, read by `MemoryChannel`, are incremented as plain globals in the custom flecs allocator hooks.

**Why it matters:** Entirely fine under today's single-threaded frame orchestration (per `info.md`'s own "Accepted Tradeoffs" section). But that same document names the planned shared job system (`flecs set_threads` + `engine::jobs`) as the explicit trigger that would end single-threaded assumptions — at which point these counters need to become atomic, or `MemoryChannel`'s numbers will quietly become wrong (or racy) rather than failing loudly.

**How to test:** No test needed today; flag as a required follow-up item the day the job system work begins.

---

## Summary table

| # | Severity | Issue |
|---|---|---|
| C.1 | Critical | `.set<n>()` likely typo for `.set<Name>()` in `buildDefaultScene()` |
| H.1 | High | Snapshot-mode Play: spinner/animator/progress hardcoded to `m_ecs`, never `simWorld()` |
| H.2 | High | `KitHost::poll()` failed reload leaves null-plugin entry in `m_loaded` |
| H.3 | High | `module_loader.h` temp filenames can collide across concurrent processes |
| M.1 | Medium | Static-init order fragility around `hookFlecsAllocator()` |
| M.2 | Medium | No guard ensuring `attachPlugins()` precedes `startSimulation()` |
| M.3 | Medium | `EventSweeper` rebuild trigger misses same-size type-set changes |
| L.1 | Low | `openProject()` pre-init check relies on unrelated pointer's nullness |
| L.2 | Low | Failed kit reloads accumulate graveyard/temp-file cost faster than accepted case |
| L.3 | Low | Memory counters are non-atomic; landmine for the planned job system |

## Suggested fix order

1. **C.1** — trivial to confirm and fix, blocks the default scene entirely if it's really a typo.
2. **H.1** — real, visible, testable gameplay bug in the exact feature named after itself (spinning cube).
3. **H.2** — crash risk (`stop()` null-deref) plus a misleading UI state; also cheap to fix (erase the entry on failure).
4. **H.3** — one-line fix (PID/GUID in the temp filename), removes a real multi-instance race.
5. **M.1, M.2, M.3** — good candidates for the next round of hardening or the test suite as it's built out, even before code changes land.
6. **L.1, L.2, L.3** — track, revisit at the documented trigger points rather than fixing preemptively.
