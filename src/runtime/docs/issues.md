---
status: unreviewed
---
# Engine Runtime — Consolidated Issues Report

This supersedes the standalone `docs/issues.md`, `docs/runtime-core-issues-report.md`, and the earlier input/hid report. Every issue below has been verified against actual source lines (not inferred from comments or documentation) as of this review. Grouped into **Architecture** (structural violations of the engine's own layering rules) and **Bugs** (severity-ranked, each with the precise mechanism, not just the symptom).

---

# RESOLUTION STATUS (July 10, 2026 — audit sweep complete)

Every claim was re-verified against source before fixing; **one was false**.
Each fix landed with a regression test proven to catch the pre-fix bug
(reverted code fails the test / trips ASan) — see `tests/`, run `ctest -L unit`.

| # | Status | Fix / evidence |
|---|--------|----------------|
| C.1 | **FIXED** | ScriptHost owns its observers, destructs per re-bind + alive-token. `script_host_test` (pre-fix: ASan stack-use-after-scope + 2N observer growth) |
| C.2 | **FIXED** | Failed reload erases the entry + surfaces LoadFailed; ModuleLibrary::unload resets m_plugin. `kit_lifecycle_test` failed-reload phase |
| C.3 | **FIXED** | hid_null defines deviceGeneration(); `hid_null_link_check` links the whole Context surface on every platform (pre-fix: exact undefined-symbol repro) |
| C.4 | **FIXED** | One canonical normalized key at every map boundary. `async_loader_test` (pre-fix: 5 failures — waiter drop, cache defeat, raw-key lies) |
| C.5 | **FIXED** | JobHandle co-owns the completion block (shared_ptr); lifetime documented in jobs.h. `jobs_test` (pre-fix: ASan heap-use-after-free) |
| H.1 | **FALSE** | `.set<n>()` exists only inside this report — never in code. Report artifact |
| H.2 | **FIXED** | Spinner query is a WorldQueryCache on simWorld(); edit-world animator skipped during Snapshot play. `sim_world_test` (pre-fix: frozen) |
| H.3 | **FIXED** | dlsym → libSym |
| H.4 | **FIXED** | Exception-free parses + type guards + backstop; bad configs degrade. `input_config_test` (pre-fix: aborts on uncaught type_error) |
| H.5 | **FIXED** | m_simAccumulator reset per session |
| H.6 | **FIXED** | AssetService::loadFailed() + strict isSceneReady + sceneLoadFailed(). `asset_ready_test` |
| M.1 | **FIXED** | Raw context opens mouse-only (gamepad returns with curation #13) |
| M.2 | OPEN | devMu contention — real but needs a lock-free device snapshot; revisit with gamepad curation |
| M.3 | **FIXED** | Sweeper rebuild compares type identity, not count |
| M.4 | **FIXED** | PID in temp module names |
| M.5 | **FIXED** | startSimulation warns when attachPlugins never ran |
| M.6 | **FIXED** | Hook sequenced via member-init before m_ecs; idempotent |
| M.7 | **FIXED** | Tables reunified; `keymap_test` round-trips all 65 mapped keys |
| L.1–L.8 | TRACKED | Low-severity; revisit at their documented trigger points |
| A.1–A.4 | **FIXED (seams) + ENFORCED** | Passthrough GPU accessors moved behind renderer(); camera_util pure (caps parameterized); importers gated headless; async_loader.h forward-declares. `sim_purity_check` compiles 16 sim-facing headers against poisoned bgfx/assimp stubs — future leaks fail the build. Full engine_sim lib split lands with engine_player/server (backlog G) |

---

# Part 1: Architecture Violations

The engine's own `info.md` states the rule plainly: `engine_core` is the GPU-free layer; `engine_core` links only assetlib + assimp, no bgfx/GLFW/Jolt/Lua; `engine_runtime` links `engine_core` plus "the graphics/platform/plugin stack." In practice, the boundary between "core simulation, safe for a dedicated server" and "graphics-and-tooling stack" is not enforced anywhere — it's a convention that individual files have already drifted from.

### A.1 — bgfx is a hard dependency of the core runtime object's public API

`runtime.h` — the header defining `EngineRuntime` itself — does this:
```cpp
#include <bgfx/bgfx.h>
...
bgfx::TextureHandle sceneColorTexture() const { return m_renderer.sceneColorTexture(); }
bgfx::TextureHandle gameColorTex()      const { return m_renderer.gameColorTex(); }
```
`bgfx::TextureHandle` is a type in the public interface of the class that a headless server would link. `runtime.cpp` goes further — it calls `bgfx::createVertexBuffer`, `bgfx::createIndexBuffer`, and `bgfx::frame()` directly, rather than routing all GPU work through `Renderer`, which is the object whose entire job is to own the "GPU device lifecycle" per `info.md`. There is a runtime `if (!m_headless)` guard around some of this, which is why it doesn't crash on a headless build today — but a guard checked at every call site is not the same guarantee as a type system that makes the mistake impossible. Nothing stops a future contributor from adding one more direct `bgfx::` call without the guard.

### A.2 — `camera_util.h` calls live bgfx state, not just bgfx-adjacent math

```cpp
#include <bx/math.h>
#include <bgfx/bgfx.h>
...
const bool rhNdc = bgfx::getCaps()->homogeneousDepth;
```
`bx::math` (vector/matrix math with no GPU dependency) would be defensible in a core header. `bgfx::getCaps()` is a live query against an initialized bgfx context — this is not incidental, it's a real runtime dependency. The one call site that matters (`EngineRuntime::tick(float dt)`) is currently protected by `if (m_headless) return false;` placed *before* the call to `m_cameraFinder.find()` — so this is not reachable on today's headless path. But this is safety by convention at one call site, not by the header's own contract, and `camera_util.h` is included directly by `runtime.h`, meaning bgfx is a transitive dependency of the core runtime header regardless of whether the headless path ever calls into it.

### A.3 — Assimp is registered unconditionally for every `EngineRuntime`, including shipped games

`info.md` describes `AsyncLoader` as "legacy import path used by **the editor** for source-format assets (FBX via Assimp etc.)" — explicitly editor-only. But:
```cpp
// runtime.cpp:71
#include "assets/importers/assimp_importer.h"
...
// runtime.cpp:234, inside initSystems() — runs for every EngineRuntime
m_importers.registerImporter(std::make_unique<AssimpImporter>());
```
This is not gated behind an editor flag, a build configuration, or `cfg.openAssetDatabase`. Every game that links `engine_runtime` — a shipped title, a dedicated server, a headless CLI tool — pays the link cost and the init cost of Assimp, a large, primarily offline, editor-facing parsing library, even though gameplay code has no path that ever calls into it.

### A.4 — `async_loader.h`/`.cpp` is the real concentration point, and it's worse than A.3 alone suggests

The header itself pulls in bgfx:
```cpp
// async_loader.h
#include <bgfx/bgfx.h>
```
And the implementation pulls in the full offline-import stack:
```cpp
// async_loader.cpp
#include "animation/assimp_skeleton_loader.h"
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <assimp/material.h>
#include <stb_image.h>
```
Five separate Assimp headers, ozz's archive/stream I/O, and stb_image, all in a file whose header comment describes it as editor tooling. Because the bgfx include is in the **header**, not just the `.cpp`, anything that includes `async_loader.h` to get `AsyncLoader`'s public interface transitively gets the full bgfx API surface too, whether or not it ever calls a bgfx function.

### Why this matters beyond "it's untidy"

The stated goal — "the runtime could be compiled into a simulation-only engine and rolled up in a dedicated server" — is not actually true today, and the gap isn't visible from reading `info.md`; it only surfaces by grepping actual includes, which is how all four of the above were found. The concrete risk is a dedicated server binary silently linking (and initializing) a GPU API and an offline mesh importer it will never use, inflating binary size, startup time, and attack surface for no functional benefit.

### The fix that actually holds

Auditing this by hand, as this document does, will go stale the moment someone adds a new include. The durable fix is a CMake target — a real "headless/server" build — that links `engine_core` plus whatever a dedicated server legitimately needs (ECS, input snapshot/replay, plugins, jobs) and explicitly does **not** link bgfx, GLFW, or Assimp. Every violation above becomes a compile error the moment that target exists, rather than something a reviewer has to notice. This should be treated as infrastructure work that unblocks everything else in this report, not as one more item in the list.

---

# Part 2: Bugs

## Critical

### C.1 — `ScriptHost::setWorld()` leaks flecs observers, and on ordinary shutdown this becomes a real use-after-free

**Mechanism:**
```cpp
void setWorld(flecs::world* w) {
    m_world = w;
    m_nameQuery.reset();
    m_nameIndex.clear();
    if (!w) return;
    w->observer<const Name>().event(flecs::OnSet)
        .each([this](flecs::entity e, const Name& n) { m_nameIndex[n.value] = e.id(); });
    w->observer<const Name>().event(flecs::OnRemove)
        .each([this](flecs::entity e, const Name& n) { ... });
    ...
}
```
Neither observer's handle is stored. The comment justifies this with "observers die with the world, so re-register per bind" — true only if the world is destroyed before the next `setWorld()` call on it.

**Two distinct failure modes, escalating in severity:**

1. **Observer accumulation (In-Place Play mode).** In `SimMode::InPlace`, `simWorld() == m_ecs`, a member of `EngineRuntime` that persists for the process's entire life — it is never destroyed between Play/Stop cycles. Every `startSimulation()` calls `setWorld(&simWorld())`, registering two more observers on the same still-alive world; every `stopSimulation()` calls `setWorld(nullptr)`, which — because `w` is null — skips creating new observers but does **nothing** to remove the ones already attached. A developer who presses Play/Stop N times during one editor session has 2N live observers permanently attached to `m_ecs`, each redundantly updating the same map entries. Wasteful, not corrupting, on its own.

2. **Use-after-free on engine shutdown (the serious one).** `runtime.h`'s member declaration order is:
   ```cpp
   flecs::world m_ecs;                          // declared earlier
   ...
   std::unique_ptr<ScriptHost> m_scriptHost;     // declared later
   ```
   C++ destroys class members in **reverse** declaration order. `~EngineRuntime()` therefore destroys `m_scriptHost` — and the `ScriptHost` object it owns — **before** `m_ecs`. `m_ecs`'s own destructor tears down every entity it holds; for any entity carrying a `Name` component, that fires the `OnRemove` observer registered above — whose lambda captured `this`, a pointer to the `ScriptHost` that was destroyed one step earlier in the same teardown sequence. `EngineRuntime::shutdown()` calls `setWorld(nullptr)` via `stopSimulation()`, but as established, that never removes the existing observers — it only skips adding new ones. **This is not an obscure edge case: In-Place is the boot-time default for a standalone game ("game: boot = play" per `info.md`), meaning any game that ever calls `startSimulation()` — which is all of them — hits this use-after-free on ordinary process shutdown**, the moment `m_ecs` tears down any named entity during its own destruction.

**Fix direction:** store the `flecs::observer` handles returned by `.observer<...>()` (they're destructible/movable objects, not fire-and-forget registrations) as members of `ScriptHost`, and explicitly destroy/reset them at the top of `setWorld()` before creating new ones — regardless of whether the old world is about to be destroyed. This removes the dependency on member-destruction-order correctness entirely.

**How to test:** Boot a game with In-Place simulation, run to shutdown, run under ASan — this should reproduce reliably as a heap-use-after-free the moment `m_ecs` destructs a `Name`-carrying entity. Separately, a Play→Stop→Play→Stop loop with an observer-count assertion (flecs exposes iterating registered observers, or a manual counter in a test build) would catch the accumulation issue without needing ASan.

---

### C.2 — `KitHost::poll()` leaves a dead `nullptr`-plugin entry after a failed hot-reload, and the `ModuleLibrary` underneath it independently goes stale too

**Mechanism (KitHost side):**
```cpp
// kit_host.h, poll()
e->plugin->onSimulationStop();
e->plugin->onDetach();
reg.remove(e->plugin.get());
e->plugin.reset();
e->lib.unload();

if (!e->lib.load(e->path)) {          // reload fails the ABI/contract gauntlet
    LOG_ERROR("Kit", "reload failed: '%s'", e->name.c_str());
    continue;                          // Entry stays in m_loaded, plugin == nullptr
}
```
The old plugin is fully torn down before the reload is attempted. If the rebuilt module fails `ModuleLibrary::load()`'s ABI/contract checks, the `continue` skips reassigning `e->plugin`, but the `Entry` is never removed from `m_loaded`.

**Consequences:**
- `stop()` unconditionally does `e->plugin->onDetach()` for every entry in `m_loaded` — a null-pointer dereference the next time simulation stops.
- `isLoaded(name)` only checks whether an `Entry` with that name exists in `m_loaded`, not whether its `plugin` is non-null — so it keeps reporting the kit as loaded. That makes `loadOne()` silently no-op (`if (isLoaded(name)) return true;`), and any UI built on `status()`/`isLoaded()` shows a dead kit as healthy.
- `KitStatus` for this kit also never updates to reflect the failure — `setStatus()` is only called on success or on the *initial*-load failure path, never on a reload failure, so the UI keeps showing the kit's previous "Loaded" status indefinitely.

**Mechanism (ModuleLibrary side — a second, independent bug underneath the first):**
```cpp
void unload() {
    releaseContracts();
    if (m_table && m_destroy) m_destroy(m_table);
    m_table   = nullptr;
    m_destroy = nullptr;
    if (m_handle) { graveyard().push_back(m_handle); m_handle = nullptr; }
    m_tempPath.clear();
}
```
`m_plugin` — the `shared_ptr<GameModuleAdapter>` — is never touched here. The `GameModuleAdapter` holds its own raw pointer to the module's `EngineGameModuleV1*` table, captured once at construction; `unload()` nulls out `ModuleLibrary`'s own copy of that pointer but does nothing to the adapter that's still alive via `m_plugin`. On a **successful** reload this is masked, because `load()` immediately overwrites `m_plugin` with a freshly-constructed adapter. On a **failed** reload — the exact scenario in the paragraph above — `e->lib`'s internal `m_plugin` is left holding a stale adapter pointing at freed module memory, for as long as that `ModuleLibrary` object survives (which, per the bug above, is indefinitely — the `Entry` is never removed). If anything ever calls `e->lib.plugin()` directly instead of going through `Entry::plugin` — a future diagnostic, a refactor, a different code path — it receives a live `shared_ptr` to a use-after-free.

**Fix direction:** On reload failure, erase the `Entry` from `m_loaded` entirely (mirroring how `loadKit()`'s own initial-failure path never inserts one in the first place), and update `KitStatus` to `LoadFailed` explicitly. Separately, `ModuleLibrary::unload()` should reset `m_plugin` to `nullptr` unconditionally, regardless of reload outcome — it should never be possible for `plugin()` to return an adapter whose underlying table is known to be destroyed.

**How to test:** Force a hot-reload to fail the ABI gauntlet (e.g. touch the module's declared `structSize` so the version check trips), then confirm: `poll()` doesn't leave a null-plugin entry; `stop()` and `isLoaded()` behave correctly afterward; `e->lib.plugin()` never returns a non-null adapter after a failed reload.

---

### C.3 — `hid_null.cpp` is missing `Context::deviceGeneration()` — a link error on any non-macOS build

`hid.h` declares `deviceGeneration()` as part of `Context`'s required interface. `hid_iohid.cpp` implements it; `hid_null.cpp` — the fallback backend for "anything else," per `info.md` — does not. `HidSource::deviceGeneration()` (in `input_sources.h`) unconditionally calls `m_ctx.deviceGeneration()`, and `HidSource` is constructed unconditionally in `InputManager::init()` regardless of whether the real backend initializes successfully. The moment Windows or Linux — both of which currently fall back to `hid_null.cpp` per `info.md`'s own "Future Work" section — attempt to build, the linker fails to resolve `hid::Context::deviceGeneration() const`.

**How to test:** A CI lane that force-compiles against `hid_null.cpp` even on macOS (build flag override) turns this into an immediate, every-commit check rather than a surprise when a Windows port begins.

---

### C.4 — `AsyncLoader`'s path-key mismatch defeats its own cache, and separately drops queued callbacks entirely

**Mechanism (cache defeat):**
```cpp
static std::string normalizeKey(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}
```
`load()`'s fast-path cache check normalizes: `m_loadedResults.find(normalizeKey(path))`. But the completion handler that populates that same map does not:
```cpp
m_loadedResults[req.asset.path] = result;   // raw, un-normalized path used as the key
```
On any platform or caller where `path` contains backslashes (Windows paths, or any mixed-separator caller), the key used to **store** a completed load is never equal to the key used to **look it up** — the normalized lookup can never match the raw-keyed entry. This doesn't just cause "occasional cache misses" — it structurally defeats the cache for every path that isn't already forward-slash-clean, meaning the same asset gets fully reprocessed (worker-thread parse + decode) on every single `load()` call rather than being served from cache after the first.

**Mechanism (dropped callbacks — a second, distinct bug):**
```cpp
// load(), in-flight branch:
m_waiters[normalizeKey(path)].push_back(std::move(cb));
...
// completion handler, ~950 lines later:
auto it = m_waiters.find(req.asset.path);   // raw path — does not match the normalized insert key above
```
A caller that requests a path already in-flight gets its callback queued under `normalizeKey(path)`. The completion handler looks it up under the raw, un-normalized `req.asset.path`. If those differ, the lookup misses, `waiters` stays empty, and **the queued callback is never invoked at all** — not a stale result, a caller left waiting indefinitely for a callback that will never fire.

`unload()`, `isLoading()`, and `isLoaded()` also use raw, un-normalized paths for their own lookups (matching the originally-reported claim), compounding the same root inconsistency across the whole public interface.

**Fix direction:** Normalize at every single entry/exit point touching these maps — storage, lookup, erase — with no exceptions. The safest structural fix is to normalize once at the very top of every public method (`load`, `unload`, `isLoading`, `isLoaded`) and never store or look up a raw path anywhere internally.

**How to test:** A unit test that calls `load()` twice with a Windows-style path (backslashes) for the same asset, on a second call asserting the cache actually hit (no second worker dispatch) rather than just checking the returned result is correct. Separately, a test that requests the same in-flight path from two callers and asserts *both* callbacks fire.

---

### C.5 — `JobHandle` can become a genuine use-after-free, and the safety rule isn't documented where callers would see it

**Mechanism:**
```cpp
JobHandle run(const char* name, std::function<void()> fn) {
    ...
    auto task = std::make_unique<RunTask>(name, std::move(fn));
    RunTask* raw = task.get();
    { std::lock_guard<std::mutex> lk(g_inflightMu); g_inflight.push_back(std::move(task)); }
    g_ts.AddTaskSetToPipe(raw);
    return {raw};                      // caller now holds a raw pointer into g_inflight
}
```
```cpp
void pumpMain() {
    ...
    // Sweep finished run() tasks. Completed blocks die here.
    for (size_t i = g_inflight.size(); i-- > 0;) {
        if (g_inflight[i]->GetIsComplete()) {
            g_inflight[i] = std::move(g_inflight.back());
            g_inflight.pop_back();      // the RunTask is freed here
        }
    }
}
```
```cpp
void wait(JobHandle h) {
    if (!h.valid() || !g_init.load()) return;
    g_ts.WaitforTask(static_cast<RunTask*>(h.opaque));   // dereferences the raw pointer
}
```
If a caller holds a `JobHandle` across a `pumpMain()` call that happens to run after the job completed, the `RunTask` is freed by the sweep. A subsequent `wait(h)` on that stale handle dereferences freed memory.

**The documentation gap is real, not incidental:** the warning against this — "handles must not be stashed across frames" — exists only as a comment inside `jobs_enkits.cpp`, the *backend implementation file*. The public facade header, `jobs.h`, which is what any caller actually reads to use the API, documents fiber-safety rules (no thread-local state, no held mutexes across a `wait()`) but says nothing about handle lifetime, and its own phrasing — "a default-constructed handle is 'already complete'," counter-style semantics — reads as safe-to-hold. The public contract and the implementation's actual guarantee don't match.

**Fix direction:** Either make the handle's lifetime actually safe (e.g. an atomic generation/epoch counter checked by `wait()` instead of a raw pointer dereference — cheap and eliminates the whole class of bug), or move the "don't stash handles across a frame boundary" rule into `jobs.h` itself, in the same prominent position as the other three usage rules already documented there.

**How to test:** Schedule a `run()` job, call `pumpMain()` enough times to guarantee it's swept, then call `wait()` on the stale handle under ASan — should reproduce as a heap-use-after-free reliably.

---

## High

### H.0 — ✅ FIXED — `Sim.prevSnapshot` was the largest cost in the frame at scene scale
The interpolation snapshot in `runtime_sim.cpp` runs once per FIXED STEP over every
entity with a Transform:

```cpp
w.defer_begin();
w.each([](flecs::entity e, Transform& t) {
    if (e.has<Camera>()) return;
    e.set<PrevTransform>({t.position, t.rotation, t.scale});
});
w.defer_end();
```

On a 50 000-object scene that is **12.7 ms per step, ~25 ms of a 34 ms frame** —
three times the entire render path (8.6 ms) after the extraction work. It was
invisible until 2026-08-04 because the block had no profiler zone; it has one now
(`Sim.prevSnapshot`), which is how the number above exists.

Why it costs that much is the same mistake the renderer's extraction just had fixed,
in a worse form: `set<>` is a STRUCTURAL operation, so every entity every step goes
through the deferred command buffer — allocate a command, copy the payload, replay
it — plus an `e.has<Camera>()` pair lookup per entity. For an entity that already
has a PrevTransform, none of that is needed: the component exists, and overwriting
its fields is a plain memory write.

Suggested fix, in the order that keeps it safe:
1. A query over `(Transform, PrevTransform)` that bulk-copies in place, no defer and
   no per-entity lookup — this covers every entity after its first step.
2. A second query over `(Transform)` `.without<PrevTransform>()` for the newcomers,
   which is the only case that needs a structural `set<>` and is nearly always empty.
3. Exclude cameras with `.without<Camera>()` on both, so the per-entity `has<Camera>`
   disappears into the query.
4. Then consider `jobs::parallelFor` over chunks, as extraction does — but measure
   after 1–3, which may already make it negligible.

Correctness to preserve: cameras must keep being excluded (their rotation is
late-latched at render rate and must not lag), and PrevTransform must be written
BEFORE the step's broadcasts so rendering lerps from the pre-step state.

**FIXED 2026-08-04**, steps 1–3 exactly as outlined. Two cached queries replace the
one `w.each`: an in-place overwrite over `(const Transform, PrevTransform)` for
everything that already has the component, and a structural `set<>` over
`(const Transform)` `.without<PrevTransform>()` for newcomers — normally empty.
Cameras are excluded with `.without<Camera>()` on both, so the per-entity
`has<Camera>()` is gone. Both are `WorldQueryCache` entries reset in
`stopSimulation` with the others; a query outliving its world is a crash, not a leak.

| objects | before (per step) | after | frame before | frame after |
|---|---|---|---|---|
| 20 000 | 12.4 ms | **0.28 ms** | 8.4 (vsync) | 8.4 (vsync) |
| 50 000 | 12.7 ms | **0.63 ms** | 34.3 ms | **9.3 ms** |

At 50 000 objects the frame is 3.7x faster and the renderer is once again nearly all
of it (8.4 ms of 9.3). **Step 4 — `jobs::parallelFor` — was deliberately NOT done**,
because step 4 said to measure first and the measurement says no: 0.63 ms per step is
no longer worth a job dispatch, and the same work parallelised would still cost the
round trip. Revisit only if it climbs back.

TESTED by `tests/prev_snapshot_test.cpp`, because every failure mode here is
invisible to both timing and the render submit counters: a newcomer that never gets
the component stutters forever, a value captured at the wrong moment makes the lerp
interpolate from the wrong end, and a camera that acquires one makes look direction
lag. It asserts the add pass, the overwrite pass, per-entity correctness over 500
entities (skipping AND crossing), the camera exclusion, and three Snapshot
start/stop cycles for the cache-reset path. MUTATION-CHECKED: deleting the add pass
fails 9 assertions, dropping `.without<Camera>()` fails the camera one.

A note on how that test was written, since it is the reusable lesson: its first
version asserted that after moving an entity externally and stepping, PrevTransform
would still read the OLD position. It does not — and running the test against the
PRE-FIX implementation showed it failing identically, which is how the wrong
expectation was caught instead of being "fixed" in the code. `Prev := Transform` at
the top of the step means an external move between steps IS the state entering the
next step. A second draft then tried to observe Prev and Transform diverging via a
Spinner; that cannot happen headlessly either, because Spinner runs in `tick()` at
render rate and nothing else mutates a transform inside a step with no plugins and
no physics bodies. That assertion was removed rather than weakened, and the file
says so.

### H.1 — `buildDefaultScene()`: likely typo, `.set<n>()` instead of `.set<Name>()`

```cpp
m_ecs.entity(name)
    .set<Transform>(t)
    .set<MeshRenderer>({cubeHandle})
    .set<n>({name})              // should almost certainly be Name
    .set<Spinner>({0.3f, 0.1f});
```
`components/name.h` is included at the top of `runtime.cpp`, and `Name` is a real component used throughout this codebase. There is no local variable or type named `n` in scope in this function. Either this fails to compile outright, or — if `n` unexpectedly resolves to something else — every entity in the default scene silently gets the wrong component. This is the flagship demo scene every new project boots into.

*(Note: `ScriptHost::create()` in `script_host.h` has the exact same pattern — `.set<n>({name})` — suggesting this may be a copy-paste of the same typo into two places, not an isolated mistake. Worth grepping the whole tree for `.set<n>(` specifically.)*

**How to test:** Compile in isolation; add an assertion after scene construction that every generated entity has a valid `Name` component with the expected string.

---

### H.2 — Snapshot-mode Play: the simulated world's spinners never rotate, and the editing world runs pointlessly in parallel

`tickSystems()` — called unconditionally from the game-facing `tick(dt)` — hardcodes three pieces of logic against `m_ecs` specifically, never `simWorld()`:
```cpp
m_spinnerQuery.each(...);     // query built via m_ecs.query_builder(...) in initSystems()
m_animatorSystem.tick(dt);    // no world argument — implicitly targets m_ecs
m_ecs.progress();
```
Separately, `tickSimulation()` does:
```cpp
if (m_gameWorld) {
    m_animatorSystem.tick(*m_gameWorld, dt);
    m_gameWorld->progress(dt);
}
```
only when a Snapshot-mode game world exists. Since the editor's Play button uses Snapshot mode (per `info.md`), the world actually rendered and simulated during Play is `m_gameWorld` — but the spinner-rotation query and one of the two animator ticks only ever touch `m_ecs`, the original editing world nobody is looking at. Demo Spinner entities freeze the moment Play starts; the editing world keeps needlessly animating and progressing in the background every frame for no visible benefit.

**How to test:** Build the default scene, `startSimulation(SimMode::Snapshot)`, tick several times, assert that Spinner-owning entities in the *rendered* world (`simWorld()`) actually rotated.

---

### H.3 — `module_loader.h`: raw `dlsym` call breaks the Windows build

```cpp
if (auto bindApi = (EngineModuleBindApiV1Fn)
        dlsym(m_handle, "engineModuleBindApiV1"))
    bindApi(engineApiHostTable());
```
`dlsym` is POSIX-only. The file's own portable wrapper — `libSym`, which maps to `GetProcAddress` on Windows — is defined earlier in the same file and used correctly two lines later for the contracts lookup:
```cpp
if (auto contractsFn = (EngineModuleContractsV1Fn)
        libSym(m_handle, "engineModuleContractsV1")) { ... }
```
This one call site bypasses the wrapper. On Windows, `dlsym` doesn't exist — this is a straightforward compile failure, not a runtime edge case, and easy to miss precisely because the correct pattern is right next to it.

**How to test:** Same as C.3 — a CI build lane targeting Windows (or at minimum, compiling this file with `_WIN32` defined and stub Win32 headers) turns this into an immediate check.

---

### H.4 — Unguarded `std::stof`/`std::stoi`/`json::get<string>()` crash the engine on a malformed `input.json`

```cpp
if (parts.size() >= 3) out->scale = std::stof(parts[2]);   // parseBinding
...
else out->code = (uint16_t)std::stoi(parts[1]);              // parseBinding
...
if (parseBinding(jb.get<std::string>(), a.type, &b))          // loadConfigText
```
The top-level `nlohmann::json::parse` call is in non-throwing mode, but none of the field-level accesses are protected. A binding value that's a number instead of a string, or a scale value that overflows a float, throws an uncaught exception straight out of `loadConfigText` — crashing the engine at boot or during an editor hot-reload of the config, from nothing more exotic than a typo or a bad merge in a hand-edited config file.

**How to test:** Fuzz `loadConfigText` with a corpus of malformed JSON (non-string bindings, oversized numeric strings, missing fields), wrapped in the fuzz harness's own try/catch so every throw site surfaces before deciding whether to add validation or accept a deliberate crash-on-bad-config policy.

---

### H.5 — Simulation accumulator survives across play sessions, causing an early tick and a corrupted interpolation alpha on the first frame of a new session

`startSimulation()`:
```cpp
m_simulating = true;
m_simElapsed = 0.0;
m_simFrame   = 0;
```
resets elapsed time and frame count but never touches `m_simAccumulator`. `tickSimulation()`:
```cpp
m_simAccumulator += dt;
if (m_simAccumulator > 4.0f * kSimDt) m_simAccumulator = 4.0f * kSimDt;
while (m_simAccumulator >= kSimDt) { ... }
...
m_renderer.setSimAlpha(m_simAccumulator / kSimDt);   // leftover fraction
```
adds the new frame's `dt` straight onto whatever fraction was left over from the *previous* play session. Concretely: stop a session mid-step (say, 12ms into a 16ms fixed step), start a new one — the very first `tickSimulation()` call of the new session adds fresh `dt` on top of that stale 12ms, potentially firing a fixed step earlier than the new session should, and the interpolation alpha handed to the renderer starts from a stale, non-zero fraction instead of 0 — a visible interpolation jump on the first rendered frame.

**How to test:** Start a session, tick a known partial fraction into the accumulator, stop, start again, assert `m_simAccumulator == 0` immediately after `startSimulation()` returns.

---

### H.6 — Scene preload reports failed assets as ready

```cpp
bool SceneService::isSceneReady(const char* cookedPath) const {
    ...
    for (const auto& path : it->second.meshPaths) {
        if (m_assets.queryMesh(path.c_str()) == 0
            && m_assets.isLoading(path.c_str()))
            return false;
    }
    return true;
}
```
This only returns "not ready" when a mesh has no handle **and** is still loading. A mesh that failed to load asynchronously has no handle (`queryMesh() == 0`) and is no longer loading (`isLoading()` is false) — neither condition trips, the loop falls through silently, and `isSceneReady()` reports `true` for a scene containing an asset that will never actually appear.

**How to test:** Force a mesh load failure (bad path, corrupt cooked asset) mid-preload and assert `isSceneReady()` correctly reports `false`, or exposes the failure some other explicit way — silently reporting "ready" should not be possible.

---

## Medium

### M.1 — Raw `hid::Context` captures device classes the consumer immediately discards, wasting the shared low-latency ring

`HidSource::init()` calls `m_ctx.init({})` — default `Config`, meaning `mouse=true, keyboard=true, gamepad=true` all stream into one shared 4096-entry ring. But `input_manager.cpp`'s hybrid `drain()` discards every raw `Key`/`Button` event from the raw source outright, and never processes `Axis` (gamepad) events past staging (they fall through `beginTick()`'s switch to `default: break;`). All of this competes for space in the same ring as the raw mouse motion the whole hybrid architecture exists to protect at low latency, and pollutes `InputLatencyChannel`'s reported numbers with events nobody consumes. Fix is a one-line `Config` change: `m_ctx.init({.keyboard = false, .gamepad = false})` (re-enable gamepad once curation lands).

### M.2 — `devMu` lock contention between the hot HID report path and sim-thread hotplug reconcile

`onValue()` — called once per incoming HID report, potentially thousands of times a second — takes `devMu` via `lookup()`. `Context::devices()` takes the same mutex, and `input_manager.cpp`'s `pump()` calls it every frame the device generation changes. A hotplug during a fast motion burst contends the backend thread's report-handling path against the sim thread — exactly the kind of latency spike `InputLatencyChannel` exists to catch.

### M.3 — `EventSweeper`'s cache-invalidation check misses same-size composition changes

```cpp
if (w.c_ptr() != m_world || reg->types.size() != m_queries.size())
    rebuild(w, *reg);
```
Detects a change in the *count* of declared event types, not a change in *which* types are declared. If an event type is unregistered and a different one registered in its place while the count stays constant, cached queries silently go stale.

### M.4 — `module_loader.h` temp module filenames can collide across concurrent processes

```cpp
m_tempPath = fs::temp_directory_path() / ("engine_module_" + std::to_string(nextTempId()) + ext);
```
`nextTempId()` is a per-process counter starting at 0. Two engine instances running concurrently (two editor windows, a parallel CI matrix) can compute the identical temp path in the shared system temp directory at close to the same moment.

### M.5 — No lifecycle guard ensuring `attachPlugins()` ran before `startSimulation()`

The class is otherwise careful about loud lifecycle enforcement (`m_initialized`, the `m_pluginsAttached` double-call guard), but nothing stops `startSimulation()` from broadcasting `onSimulationStart` to plugins whose `onAttach()` never ran, silently violating the documented lifecycle order instead of failing loudly like everything else in this class.

### M.6 — Static-init order fragility around the flecs allocator hook

`const bool g_flecsHooked = hookFlecsAllocator();` relies on running before any `flecs::world` is constructed — true today because `EngineRuntime` is built inside `main()`, after all translation units' static init has completed, but that's a guarantee about `main()` running after all statics, not about this global's initializer running before others'. A future global/static-duration `flecs::world` elsewhere would have unspecified hook coverage.

### M.7 — `hid_keymap.h`: editor rebind flow and name-based bindings have asymmetric key coverage

`nameFromGlfw` (used by the "capture a key" rebind UI) only covers F1–F6 and stops at `LCtrl`, never handling F7–F12 or `RShift`/`RCtrl`/`RAlt`/`RSuper`, even though `usageFromGlfw`/`usageFromName` fully support all of them. Separately, `usageFromName`'s lookup table is missing `RAlt` and `RSuper` entirely — a hand-written `"key:RAlt"` binding in `input.json` silently fails with only a log warning. A single round-trip unit test across every `Key` enum value (`usageFromGlfw` → `nameFromGlfw` → `usageFromName`, all should agree) would have caught both gaps immediately.

---

## Low

### L.1 — Mouse button index has no upper bound in the raw HID backend
`hid_iohid.cpp` reports mouse buttons as `usage - 1` with no cap; `beginTick()` silently drops any `Button` event with `code >= 32`. Mice with more than 32 programmable buttons lose input with no error or log.

### L.2 — No retry path after a mid-session Input Monitoring permission grant
`Context::init()`'s `if (m_impl) return m_impl->running;` guard means a permission granted after an initial denial (same process lifetime) can't be retried without destroying and recreating the whole `HidSource`.

### L.3 — `m_lookTotalX`/`m_lookTotalY` accumulate in `double` and never reset
Intentional cumulative-diff design, but a long-lived process (dedicated server, editor open for weeks) will eventually approach 2^53, the point where `double` can no longer represent every integer exactly — a real motion delta could silently round to no change.

### L.4 — Sub-tick edge buffer can overflow under a high-rate input burst
`InputSnapshot::kMaxEdges = 16` is fine for a human but not for a macro keyboard or bot; overflow drops extra edges gracefully (no crash) but `actionPressedOffsetUs` silently loses sub-tick precision for the dropped ones with no signal.

### L.5 — Raw/window motion handoff has a theoretical double-count window
`kRawHandoffNs = 250ms` is a documented heuristic, not a guarantee; a burst ending right at the boundary could theoretically let both raw and window motion contribute for one frame.

### L.6 — `openProject()`'s pre-init check piggybacks on an unrelated pointer's nullness
Distinguishes "init() calling this internally" from "called too early" via `m_assetService`'s nullness rather than an explicit flag — works today, fragile to a future refactor.

### L.7 — Failed kit reload attempts accumulate graveyard images and temp files faster than the accepted "successful unload" cost
Every refused module load still pushes its handle into the process-lifetime graveyard and never deletes its temp file — a faster-growing version of an already-accepted tradeoff, worth knowing during a rapid iterate-fix-rebuild session.

### L.8 — Memory counters are plain (non-atomic) globals — a landmine for the planned job system
Fine under today's single-threaded frame orchestration; `info.md` itself names the planned shared job system as the trigger that ends that assumption, at which point `ecs_os_api_malloc_count` and friends need to go atomic or `MemoryChannel`'s numbers become quietly wrong.

---

# Summary Table

| # | Area | Severity | Issue |
|---|---|---|---|
| A.1 | Architecture | — | bgfx types in `EngineRuntime`'s public API (`runtime.h`) |
| A.2 | Architecture | — | `camera_util.h` calls live `bgfx::getCaps()`, not just math |
| A.3 | Architecture | — | Assimp registered unconditionally for every runtime instance |
| A.4 | Architecture | — | `async_loader.h/.cpp` — bgfx in header, 5 Assimp headers + ozz + stb in impl |
| C.1 | scripting | Critical | `ScriptHost` observer leak → UAF on ordinary engine shutdown |
| C.2 | runtime | Critical | Failed kit hot-reload → dead entry in `KitHost` + stale adapter in `ModuleLibrary` |
| C.3 | hid | Critical | `hid_null.cpp` missing `deviceGeneration()` — link error off-macOS |
| C.4 | services | Critical | `AsyncLoader` path-key mismatch defeats cache + drops queued callbacks |
| C.5 | jobs | Critical | `JobHandle` use-after-free; safety rule undocumented in public header |
| H.1 | runtime | High | `.set<n>()` likely typo for `.set<Name>()` (possibly in two places) |
| H.2 | runtime | High | Snapshot-mode Play: spinner/animator/progress hardcoded to `m_ecs` |
| H.3 | runtime | High | `module_loader.h` raw `dlsym` breaks Windows build |
| H.4 | input | High | Unguarded `stof`/`stoi`/`get<string>()` crash on malformed config |
| H.5 | runtime | High | `m_simAccumulator` not reset across play sessions |
| H.6 | services | High | Scene preload reports failed assets as ready |
| M.1 | hid | Medium | Raw Context captures keyboard/gamepad, wastes shared ring |
| M.2 | hid | Medium | `devMu` contention: hot report path vs. hotplug reconcile |
| M.3 | runtime | Medium | `EventSweeper` rebuild trigger misses same-size type-set changes |
| M.4 | runtime | Medium | Temp module filenames collide across concurrent processes |
| M.5 | runtime | Medium | No guard ensuring `attachPlugins()` precedes `startSimulation()` |
| M.6 | runtime | Medium | Static-init order fragility around `hookFlecsAllocator()` |
| M.7 | input | Medium | `hid_keymap.h` F7–F12/RAlt/RSuper coverage gaps |
| L.1–L.8 | various | Low | See above |

---

# Suggested Order of Work

1. **Stand up the headless/server CMake target (Architecture A.1–A.4).** This is infrastructure, not a bug fix, but it converts every architecture violation into a compile error and prevents new ones — do this before or alongside the fixes below, not after.
2. **C.1 (ScriptHost UAF)** — crashes on ordinary shutdown for every game that ever simulates. Highest real-world blast radius of anything in this report.
3. **C.4 (AsyncLoader)** — silently defeats its own cache and drops callbacks; likely already causing unexplained "why did this reload" or "why did my callback never fire" reports.
4. **C.2 (KitHost/ModuleLibrary)** and **C.5 (JobHandle)** — both crash-class bugs, both cheap to fix once isolated.
5. **C.3, H.3 (link-time breaks)** — trivial fixes, currently blocking any non-macOS/non-POSIX build outright.
6. **H.1, H.2, H.5, H.6** — concrete, testable, moderate-effort correctness bugs.
7. **H.4** — crash-on-malformed-config; cheap to guard, high nuisance value if left.
8. **M.1–M.7** — good candidates for the test suite build-out in parallel with fixes, even before code changes land.
9. **L.1–L.8** — track, revisit at the documented trigger points (job system landing, multi-week server sessions, etc.) rather than fixing preemptively.