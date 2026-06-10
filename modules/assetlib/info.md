# assetlib

## Purpose
Standalone asset-database module: SQLite-backed asset registry (UUIDs,
content hashes, cook state), the cook pipeline, dependency tracking, and file
watching. Built as its own CMake library — it has no dependency on the engine
and already follows the include/src public-header split the rest of the SDK
is moving toward.

## Architecture
- **`AssetRegistry`** — `registry.db` (SQLite, WAL journal). `scan(root)`
  assigns UUIDs to new files and re-hashes changed ones; records carry
  source path, content hash, cooked path, and state (used by the asset
  browser's badges).
- **`CookPipeline`** — staleness checks (source hash vs record), cooker
  registration by extension (`ICooker`), and `cookMany` (parallel cooks on a
  thread pool; registry I/O stays on the calling thread so the single
  connection is never shared).
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
