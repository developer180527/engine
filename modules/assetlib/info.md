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
### The cook layer — one concern per TU
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
| `ddc.h` / `ddc.cpp` | **the store** — two-tier content-addressed blobs, atomic ingest, hardlink materialization, shared→local promotion. |
| `ddc_manifest.*` | **cached-output record format** — a cook's output set (primary + sibling `.ctex`) as a manifest of per-member content-hashed blobs; all-or-nothing fetch, so a hit never yields a mesh missing its textures. |
| `task_graph.*` | **scheduling** — cost-weighted DAG: max-heap ready queue on estimated bytes (longest-first dispatch), dependency edges, memory-budget admission + QoS-demoted workers (the thermal levers live here), a serialized drain lane on the caller thread for `done()` callbacks, cancellation, cycle detection. Dependents release when a task DRAINS (success or failure) — a failed asset never wedges the scenes referencing it. |
| `src/cook_env.h` | the `COOK_*` env-knob reader shared by the above. |
- **`DdcStore`** (`ddc.h`) — two-tier content-addressed Derived Data Cache:
  local `~/.engine/ddc` (`ENGINE_DDC`) + optional shared mount
  (`ENGINE_DDC_SHARED`), BLAKE3-256 keys (vendored `third_party/blake3`,
  portable + NEON). Immutable read-only blobs, atomic temp+rename ingest,
  hardlink materialization, shared→local promotion on hit. Multi-output
  cooks are manifests of member blobs (see `src/assets/info.md`).
- **`DependencyGraph`** — asset→asset dependencies so cooking can cascade.
- **`FileWatcher`** — change notifications driving re-scan requests.
- **`uuid`** — stable asset identity that survives renames/moves.

## Concurrency Model
WAL mode: exactly one writer connection (the cook thread / CLI) plus any
number of read connections (main thread, panels). Never share one connection
across threads.

## Consumers
- Engine runtime: read connection opened in `EngineRuntime::init`.
- `CookService` (`src/io`): write connection on the cook thread.
- `engine_cook` CLI: synchronous one-shot cook.

## Future Work
- Asset dependency-driven recooks wired end to end (graph exists; cascade
  triggering is partial).
