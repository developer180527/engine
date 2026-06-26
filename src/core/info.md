# Core

## Purpose
Dependency-light fundamentals used by every other subsystem. Nothing here may
include renderer, ECS, or editor headers.

## Contents
- **`handle.h`** — `Handle<Tag>`: type-safe uint32_t wrapper for registry
  slots. Slot 0 is reserved as the null handle in every registry.
- **`transform.h` / `transform_utils.h`** — position/rotation(quat)/scale
  component + matrix composition helpers (bx conventions, row-major).
- **`math_types.h`** — small shared math types.
- **`entity_id_util.h`** — stable entity id helpers for serialization.
- **`logger.h`** — LOG_INFO/WARN/ERROR/SUCCESS → stdout + editor console.
- **`profiler.h`** — extensible instrumenting profiler (hub + channel
  registry; timer is the first channel). `ENGINE_PROFILE_SCOPE("name")`.
  GPU/ECS-free so it times boot and works in engine_core tools. See
  `docs/performance.md` for the scope-granularity discipline rule.

## Rules
- Keep this layer header-only and free of engine state; it should compile in
  a unit test with no GPU, no ECS, no filesystem.
