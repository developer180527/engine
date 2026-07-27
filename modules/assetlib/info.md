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
- **`CookPipeline`** — content-addressed staleness (`record.ddcKey` vs the
  key of current inputs), cooker registration by extension (`ICooker`: id +
  version + settings fingerprint feed the key), and `cookGraph` (DDC hits
  served inline on the caller thread; misses + extra tasks run as a
  TaskGraph; registry I/O stays on the calling thread — the graph's drain
  lane — so the single connection is never shared). With
  `setWorkerExecutable()` set, each cook spawns an isolated
  `engine_cook_worker` child — signal-crash/timeout containment + hard child
  `setrlimit` memory caps (see `src/assets/info.md`).
- **`TaskGraph`** (`task_graph.h`) — cost-weighted DAG scheduler: max-heap
  ready queue on estimated bytes (longest-first dispatch), dependency edges,
  memory-budget admission + QoS-demoted workers (the thermal levers moved
  here from cookMany), a serialized drain lane on the caller thread for
  done() callbacks, cancellation, and cycle detection. Dependents release
  when a task DRAINS (success or failure) — a failed asset never wedges the
  scenes referencing it.
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
