---
status: as-built
tier: hardened
verified: 2026-08-03
parses-external-input: true
covers:
  - modules/assetlib/
tests:
  - tests/cook_infra_test.cpp
  - tests/fuzz_ddc_manifest_test.cpp
  - tests/fuzz_mesh_loader_test.cpp
  - tests/cook_hardening_test.cpp   # result framing, DDC GC, schema versioning
---
# assetlib

## Purpose
Standalone asset-database module: SQLite-backed asset registry (UUIDs,
content hashes, cook state), the cook pipeline, dependency tracking, and file
watching. Built as its own CMake library — it has no dependency on the engine
and already follows the include/src public-header split the rest of the SDK
is moving toward.

## Architecture
- **`AssetRegistry`** — `registry.db` (SQLite, WAL journal). `scan(root)`
  assigns UUIDs to new files and re-hashes changed ones (BLAKE3-256; legacy
  FNV hashes upgraded in place on scan); records carry source path, content
  hash, cooked path, DDC key of the last cook attempt, and state (used by
  the asset browser's badges).
### Cooked asset formats
`mesh_asset` / `texture_asset` / `scene_asset` / `shader_asset` / `material_asset`
are the on-disk containers cookers write and the runtime reads. They live here
rather than in the engine because a cooked format is a contract between the
offline and online halves, and `engine_core` (which hosts the cookers) must stay
GPU-free.

Every loader treats its input as **untrusted**: cooked blobs travel through a
SHARED DDC, so "another machine wrote this" is the threat model, not a
hypothetical. String lengths are capped before allocating, and offsets into a
payload are bounds-checked against it — a `.cshader` variant slice pointing past
its blob is rejected rather than handed to a GPU driver.

### The cook layer — one concern per TU
Design doc: **`docs/architecture/asset-cook-architecture.md`** — the key recipe, the
invariants that are load-bearing (and silent when broken), the
transformation-graph target, and the decisions deliberately not taken. Read §5
(invariants) and §6.2 (stage-boundary economics) before changing cook code.

`CookPipeline` orchestrates; each mechanism sits behind its own seam, so a
change to (say) the cache record format can't disturb scheduling or registry
policy. Public headers are `cooker.h` (the cooker contract), `cook_pipeline.h`,
`ddc.h`, `ddc_manifest.h`, `task_graph.h`; `src/cook_*.h` are internal.

| unit | concern |
|---|---|
| `cooker.h` | **the cooker contract** — `CookContext`/`CookResult`/`ICooker` alone, no pipeline dependency. Cooker implementations and the out-of-process worker include only this. |
| `cook_pipeline.cpp` | **orchestration** — what needs cooking, in what order, what the registry records after. `resolve()` turns a record into (cooker, key, paths) once for all three entry points; `placeOutput()` is the single temp→DDC→cache placement; `commitResult()` is the single registry writer (drain lane only). |
| `src/cook_key.*` | **identity + staleness** — DDC key = source hash ⊕ cooker id ⊕ version ⊕ settings ⊕ import settings; `cookIsStale()` is the whole "is the cooked output already correct?" policy, testable on its own. |
| `src/cook_dispatch.*` | **execution mode** — isolated `engine_cook_worker` child (crash/timeout containment, hard child `setrlimit` cap) vs in-process behind the exception net. `dispatchCook()` is the seam every cook passes through — and the natural hook for remote/farm execution. |
| `ddc.h` / `ddc.cpp` | **the store** — two-tier content-addressed blobs, atomic ingest, hardlink materialization, shared→local promotion, and budget+LRU garbage collection of the LOCAL tier. |
| `cook_result_file.h` | **the worker IPC frame** — magic/version header and an `END <lines> <digest>` trailer around the sidecar result. ONE implementation, shared by writer (`engine_cook_worker`) and reader (`cook_dispatch`). |
| `ddc_manifest.*` | **cached-output record format** — a cook's output set (primary + sibling `.ctex`) as a manifest of per-member content-hashed blobs; all-or-nothing fetch, so a hit never yields a mesh missing its textures. |
| `task_graph.*` | **scheduling** — cost-weighted DAG: max-heap ready queue on estimated bytes (longest-first dispatch), dependency edges, memory-budget admission + QoS-demoted workers (the thermal levers live here), a serialized drain lane on the caller thread for `done()` callbacks, cancellation, cycle detection. Dependents release when a task DRAINS (success or failure) — a failed asset never wedges the scenes referencing it. |
| `src/cook_env.h` | the `COOK_*` env-knob reader shared by the above. |
- **`DdcStore`** (`ddc.h`) — two-tier content-addressed Derived Data Cache:
  local `~/.engine/ddc` (`ENGINE_DDC`) + optional shared mount
  (`ENGINE_DDC_SHARED`), BLAKE3-256 keys (vendored `third_party/blake3`,
  portable + NEON). Immutable read-only blobs, atomic temp+rename ingest,
  hardlink materialization, shared→local promotion on hit. Multi-output
  cooks are manifests of member blobs (see `src/assets/info.md`).
  **Collected by budget, not by reference.** Keys derive from inputs, so every
  source edit, cooker bump or settings change mints a new key and orphans the
  old blob with no referrer left to notice — reference counting cannot collect
  that, so `collectGarbage(maxBytes, prune)` evicts LRU by mtime (which `fetch`
  touches on a local hit, making the order reflect USE rather than ingest).
  Budget is `ENGINE_DDC_MAX_MB`, default 20 GB; `0` means unbounded.
  Two invariants: a blob hardlinked into a live `.cache` (link count > 1) is
  PINNED and never evicted, because unlinking the store's copy frees zero bytes
  and would re-ingest something in active use; and the SHARED tier is never
  collected from a client, because no client can know what another machine still
  needs. `engine_cook --gc` runs it AFTER the project cache sweep, so dropped
  hardlinks un-pin blobs in the same invocation.
- **`DependencyGraph`** — asset→asset dependencies so cooking can cascade.
- **`FileWatcher`** — change notifications driving re-scan requests.
- **`uuid`** — stable asset identity that survives renames/moves.

## Concurrency Model
WAL mode: exactly one writer connection (the cook thread / CLI) plus any
number of read connections (main thread, panels). Never share one connection
across threads.

## Schema versioning
`PRAGMA user_version` (`kSchemaVersion`). The additive-ALTER migration list is
idempotent — each statement is its own `exec`, so one failing cannot skip the
next — but only the exact `duplicate column name` failure is benign; anything
else is a real error and `open()` now FAILS on it rather than handing back a
registry that silently drops writes. A database whose `user_version` exceeds this
build's is refused outright: a newer engine may have added columns this build
cannot see, and writing would discard them on every `update()`. That matters
specifically when a cache is shared between machines on different builds.

## Consumers
- Engine runtime: read connection opened in `EngineRuntime::init`.
- `CookService` (`src/io`): write connection on the cook thread.
- `engine_cook` CLI: synchronous one-shot cook.

## Future Work
- Asset dependency-driven recooks wired end to end. Two mechanisms exist and
  NEITHER is wired: `DdcKeyInputs::depHashes` is hashed by `computeDdcKey` but
  populated by nobody, and `dependents()`/`transitiveDependents()` are correct
  but never called from the cook path. Harmless today only because
  `settingsFingerprint` covers the one real multi-input cooker (`MaterialCooker`
  folds the shader manifest's hash). Either wire one or delete both — see
  `src/issues.md` O1 for why the risk is an unenforced convention rather than a
  missing architecture.
