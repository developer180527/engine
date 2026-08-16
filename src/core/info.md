---
status: as-built
tier: hardened
verified: 2026-08-11
covers:
  - src/core/
tests:
  - tests/profiler_test.cpp
  - tests/arena_test.cpp
  - tests/stress_deep_tree.cpp
  - tests/stress_swarm.cpp
---
# Core

## Purpose
Dependency-light fundamentals used by every other subsystem. Nothing here may
include renderer, ECS, or editor headers.

## Contents
- **`handle.h`** — `Handle<Tag>`: type-safe uint32_t wrapper for registry
  slots. Slot 0 is reserved as the null handle in every registry.
- **`transform.h` / `transform_utils.h`** — position/rotation(quat)/scale
  component + matrix composition helpers (bx conventions, row-major).
  `Transform::getMatrix` writes the SRT matrix directly rather than composing
  it with two `bx::mtxMul`s: same 16 floats, but the multiply form spent 128
  multiplies computing them, three quarters against the structural zeros of a
  diagonal S and a near-identity T. It is the most-called function in the
  engine, and it was the bulk of the renderer's extraction cost at 20 000
  objects. Bit-exact equivalence to the two-multiply reference is asserted over
  randomised transforms in `tests/extract_partition_test.cpp` — including
  negative, non-uniform and zero scale.
  `getMatrix`'s standing contract is unchanged and load-bearing: `m[12..14]`
  equals `position` exactly, whatever the scale or rotation, which is how the
  gizmo reads position back out.
  `localMatrixLerp` / `getWorldMatrixLerpFrom` take components the caller
  already holds; the entity-only `getWorldMatrixLerp` remains for callers that
  only have an entity. Passing them in rather than looking them up is worth
  several ms per frame at scene scale — see `src/render/issues.md` R14.
- **`math_types.h`** — small shared math types.
- **`entity_id_util.h`** — stable entity id helpers for serialization.
  `findById` is O(n) and is for ONE-OFF lookups only (undo, an editor click). Loaders
  must use **`EntityIdIndex`**: built once with a single query, then O(1) collision
  checks. Calling `findById` per entity is what made scene load quadratic — sampling a
  50 000-object load put 97.6% of its 22 seconds inside it, reached from
  `EntitySerde::createEntity` (runtime issues.md H.0b, fixed 2026-08-05: 22.0 s ->
  4.78 s, and per-entity cost stopped growing with N). The index is seeded from the
  world AND inserted into as entities are created, so ids colliding within one load are
  still caught.
- **`logger.h`** — LOG_INFO/WARN/ERROR/SUCCESS → stdout + editor console.
- **`profiler.h`** — extensible instrumenting profiler (hub + channel
  registry; timer is the first channel). `ENGINE_PROFILE_SCOPE("name")`.
  GPU/ECS-free so it times boot and works in engine_core tools. Hardened:
  fixed-capacity buffers (overflow drops+warns), parent IDs, platform clock
  seam, cache-line-aligned recorders. See `docs/guides/performance.md`.
- **`mem_counters.h/.cpp`** — process-wide counting `operator new`/`delete`
  (COUNTS + forwards to malloc/free; never pools — ASan/leaks keep working).
  Gated by ENGINE_MEM_COUNT. Read per-frame deltas via runtime's MemoryChannel.
- **`frame_arena.h`** — linear bump allocator for per-frame transient data;
  reset() frees everything in O(1). Opt-in/explicit (our code uses it, it
  intercepts nothing). Lifetime: valid only within the frame; trivial
  destruction only; one arena per thread. EngineRuntime owns one (4 MB),
  reset each frameBegin; reach it via `engine.frameArena()`.
- **`json_read.h`** — bounds- and type-safe reads out of `nlohmann::json`.
  Exists because the SAME bug was written four times: nlohmann's CONST
  `operator[](size_type)` is UNDEFINED BEHAVIOUR out of range (it is not
  `at()` — no check, no exception), and `value(key, default)` THROWS rather
  than falling back when the key exists with the wrong type. The two hide each
  other: the throw gets there first, so the UB only surfaces once the throw is
  fixed. Found in `scene/entity_serializer.h` (five sites), `scene_serializer`'s
  parent pass, `editor/editor_prefs.h` (a live segfault on the project-open
  path) and `editor/undo_stack.h`. Non-finite values are refused too — JSON has
  no NaN literal but `1e999` parses to +inf, and an infinite scale corrupts a
  frame far from the load that caused it. The contract is uniform: a missing,
  wrong-typed, short, or non-finite value leaves the destination ALONE, so
  callers never get a zero they did not ask for.
  The same treatment now covers INTEGERS and STRINGS (`readU64`, `readU32`,
  `readString`): `j.value(key, 0u)` throws on a wrong-typed key exactly as the
  float form did, and an `"id": "3"` in a hand-edited scene must not become an
  exception escaping a cook thread. `readU32` additionally refuses a value that
  does not fit rather than truncating it into a different id.

## Rules
- Keep this layer header-only and free of engine state; it should compile in
  a unit test with no GPU, no ECS, no filesystem.


## The log ring (2026-08-11 rewrite)

`logger.h` is `elog::` — a fixed-slot, lock-free ring. The old `Logger` cost
**2.44 µs per line**, measured with stdout to `/dev/null` so terminal I/O was
excluded, and the decomposition is the whole story:

```
vsnprintf only            0.157 us
+ printf to /dev/null     0.280 us  (+0.123)
+ mutex + vector shift    2.431 us  (+2.151)   <- 88% of the cost
```

It was a `std::vector<LogEntry>` doing `erase(begin())` at 1024: an O(n) shift of
a thousand entries each holding two `std::string`s, plus two heap allocations per
line. A thousand lines in a frame cost 2.4 ms.

After, on the same machine:

| | before | after |
|---|---|---|
| a recorded line | 2.437 µs | **0.163 µs** — essentially just the `vsnprintf` |
| a line whose category is FILTERED | 2.437 µs | **0.0007 µs** |
| console redraw | 24.9 µs for 1 024 records | 11.4 µs for **4 096** |
| 8 threads writing | one mutex, one memmove each | 0.186 µs/line aggregate, lock-free |

**The filtered number is the one that matters.** Per-subsystem targeting is only
usable if the subsystems you are *not* watching cost nothing, and 0.7 ns is that.
Each call site resolves its category once through a function-local static, then
tests one relaxed atomic load — so the arguments are not even evaluated when
suppressed. `elog::solo(cat)` streams one subsystem at every level and drops the
rest to warnings and errors.

**`elog`, not `log`:** a namespace named `log` makes every unqualified `log(x)`
ambiguous against `::log` from `<cmath>`, which broke `third_party/imgui` as soon
as both landed in one TU.

**Loss is counted, not hidden.** `evicted()` is exact (written − slots) and the
console shows it; a ring that silently omits the line you are hunting is worse
than one that admits losing 1 342. Records carry a sequence stamp checked before
and after the copy, so a reader that gets lapped mid-record discards it rather
than displaying a splice of two writers.

**What does not belong here:** per-item telemetry. Ten thousand "this draw did X"
events a second is a trace, not a log — that is the profiler and the submit
counters. `LOG_TRACE` compiles to nothing unless `ENGINE_LOG_TRACE` is defined.
