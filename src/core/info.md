---
status: as-built
tier: hardened
verified: 2026-08-25
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

**TWO CONSOLES, ONE RING.** `Category` carries an `Audience`, because two
different people read logs. A game builder wants to know that THEIR content or
script is wrong; an engine debugger wants extraction phases, the job pool and
allocator growth. `elog::visibleToGame` is the rule, and its asymmetry is what
makes the split safe to get wrong: **info-level chatter is an allowlist
(`markGameFacingDefaults`), but warnings and errors from EVERY category always
reach the game console.** A tag nobody remembered to mark can therefore cost
noise or silence at info level and can never hide a failure from the person whose
build is broken. `panels/console_panel.h` is the game console;
`panels/internal_console_panel.h` is the engine instrument (subsystem targeting,
ring health) and is off by default.

**Engine detail is demand-driven in the editor.** The Internal Console is off by
default and a game developer is never meant to open it, so the 117 Info/Success
call sites were being formatted, ring-written and mirrored to stdout for a window
nobody had open, for the whole session. `armDemandGating()` (called by the editor
AFTER init, so boot logging is never lost) makes ENGINE categories record detail
only while something is watching; the panel `acquireWatch()`/`releaseWatch()`es on
its open/closed transition. Three properties that are not negotiable:

* **Warnings and errors are never gated.** The gate can only cost detail. One that
  could hide a failure would be a bug generator and nobody would trust the log
  again after the first time it ate one.
* **Game categories are untouched.** They feed the other console, which is always
  live; silencing a game's own scripts because an engine panel is shut would be
  indefensible.
* **Per-category targeting survives a sleep.** Solo Physics, close the panel,
  reopen: the Solo is still there. `idleQuiet`/`wakeVerbose` are IDEMPOTENT for
  this reason — the first version was not, and a second sleep saved the QUIET mask
  over the real one, permanently destroying what waking up restores.

The cost is real and stated in the UI, not hidden: lines from before you opened
the panel were never recorded, so a bug noticed late has no run-up. The answer is
the visible "Keep recording" pin (`pinVerbose`) and a reproduction.

**MEMORY ORDERING — the two places the first version was wrong.** Both were
data races by the standard that would only misbehave on weakly-ordered hardware,
i.e. every ARM64 phone this engine is aimed at:

* `Category::name` was a plain `const char*` written AFTER
  `count.fetch_add(acq_rel)` had already published the slot. A concurrent
  `category()` could observe the bumped count, index the slot and read `name`
  with no happens-before edge covering that write. It is now
  `std::atomic<const char*>`, published last with release and acquire-loaded by
  every reader; a null name means "reserved, not filled", and skipping it
  degrades to claiming a second slot for the same tag — which is the benign
  duplication the design always accepted.
* The in-progress poison (`stamp.store(0)`) was relaxed, so the field writes
  after it could become visible FIRST. A reader holding the previous sequence
  would then pass its first stamp check, copy bytes from the new record, and pass
  the re-check as well — returning a splice of two records and believing it. A
  `release` fence after the poison, paired with the reader's existing `acquire`
  fence, closes it: if a reader saw any new word, its re-check must see at least
  the poison. The final publish was always correctly ordered; this half was not.
* **And the deeper version of the same finding: the PAYLOAD was plain.** Ordering
  the poison correctly does not stop `s.msg` being a plain write racing a plain
  read — this is a seqlock, and the memory model has no way to spell "a racy read
  I will discard". It is UB, and TSan reported it nine times once the test grew a
  concurrent reader. The record is now copied through a local and published as
  RELAXED ATOMIC WORDS (`Rec` / `Slot::w`), which is well-defined under a race and
  compiles to the same load/store instructions on arm64 and x86. Suppressing it
  was the alternative and was rejected: a permanently red sanitizer lane is worse
  than the bug it reports, because it teaches everyone to stop reading it.

  Two side effects worth knowing. The sequence is now reserved AFTER formatting,
  so the window a reader can catch mid-write is a memcpy rather than a
  `vsnprintf` — much narrower, and a timestamp taken before reservation means seq
  order and timestamp order can invert by a hair. And **TSan detection here is
  window-dependent**: the fixed version reporting zero is consistent with being
  race-free but is not what proves it. What proves it is that every shared access
  is an atomic or ordered by one; the nine reports on the pre-fix code are the
  evidence that mattered, and a clean run on its own would not have been.

**Category overflow is a named bucket, not slot 0.** Past 63 distinct tags the
first version returned `&cats[0]` with no diagnostic, so the 64th tag inherited
slot 0's mask, bumped slot 0's counter and displayed under slot 0's NAME — a
kit's lines appearing to come from the renderer, and impossible to target or
silence on their own. That defeats the one feature the file exists for. The last
slot is reserved as `(categories exhausted)`, the pooling is counted
(`overflowHits()`) and reported once with the tag that first overflowed.

**There is no `clear()`.** Both consoles move a `viewFrom` sequence instead, which
needs no coordination with writers at all. The version that existed zeroed every
stamp under no lock, so a record whose publish landed after its slot was zeroed
survived while one that landed before did not — a clear that was not atomic across
the ring, stated nowhere, and with no callers.

**THE SHIPPING POSTURE.** `elog::quietForShipping()` drops ENGINE categories to
warnings and errors and leaves GAME categories alone — which is the payoff for
`Audience` existing. A released build must not print `Loaded material: X -> shader
Y (3 blocks, 2 textures, features 0x5)` into the player's log (there are 117
Info/Success sites in the engine), and it must not silence a game that uses our
logger as its own. `engine_player` calls it before `init()`.

Deliberately **not** a stdout switch: the ring is memory-only, so turning the
mirror off would leave a shipped build with no persistence and a crash report with
nothing in it. 0.12 µs a line is a fair price for the only log that survives.

`watchAll()` resets the sticky default as well as the existing categories —
"watch all" has to include subsystems discovered later, or a dev clicks it in the
Internal Console and everything registering afterwards stays silent while the UI
claims otherwise. That omission was caught by an unrelated assertion in
`logger_test`, which is the argument for having the default be observable at all.

**What does not belong here:** per-item telemetry. Ten thousand "this draw did X"
events a second is a trace, not a log — that is the profiler and the submit
counters. `LOG_TRACE` compiles to nothing unless `ENGINE_LOG_TRACE` is defined.

**Windows include hygiene.** The `#define WIN32_LEAN_AND_MEAN` / `NOMINMAX` pairs
in this subsystem are `#ifndef`-guarded. `NOMINMAX` is imposed globally by the
top-level CMakeLists (suppressing a macro is safe to force on anyone);
`WIN32_LEAN_AND_MEAN` deliberately is NOT, because it changes what `<windows.h>`
brings in and forcing a reduced header on third-party code broke bgfx's vendored
PIX headers. Each translation unit that includes `<windows.h>` states its own
choice, which is where that decision is visible.
