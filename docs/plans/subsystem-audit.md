---
status: plan
---
# Subsystem audit and hardening order

> **Status: plan.** An audit of what exists, what is load-bearing, and what order
> to strengthen it in. Every number below is derived from the tree on 2026-08-28 —
> `ENGINE_STATUS.md` for tiers, `docs/process/bugs/` for defect history, and an
> include-graph walk for dependencies. Nothing here is an opinion about code
> quality; it is an argument about *order*.

## 0. The one principle

> **Hardening propagates downward. A test on a foundation protects everything
> above it; a test on a leaf protects only itself.**

A fuzz test on `modules/assetlib` defends all ten subsystems that depend on it. An
identical test on `src/editor` defends nothing else, because nothing depends on
the editor. So the work order is not "worst first" — it is **highest fan-in
first**, and the audit below is mostly an exercise in measuring fan-in honestly.

The corollary is what makes this worth writing down: **the current tiers already
follow this rule for the bottom of the graph and stop halfway.** `core`,
`core/memory`, `assetlib`, `assets`, `runtime` and `scene` are `hardened` and are
genuinely the foundations. The gaps are all in the *middle* layer — the hubs that
many things depend on and that nobody has treated as foundations yet.

## 1. What "critical" means here

Three conditions, and a subsystem is critical when it meets all three:

1. **Fan-in.** Other subsystems depend on it, so its defects are not contained.
2. **Silence.** Its failure mode produces wrong behaviour rather than a crash —
   the class this repo's ledger is full of.
3. **No lane.** No fuzz, soak or stress test would surface it, so time does not
   find it either.

`runtime/jobs` meets all three and is the sharpest example: eight dependents, a
shipped **livelock** that only TSan's timing exposed (BUG-0003, every external
thread sharing enkiTS slot 0), and no endurance lane of its own.

## 2. The dependency graph

Derived by walking `#include` edges between subsystem directories. "Depended on
by" is the blast radius of a defect.

| subsystem | tier | depended on by | depends on | endurance lane | ledger defects |
|---|---|---:|---:|---|---:|
| `src/core` | hardened | **18** | 2 | ✅ stress ×2 | 2 |
| `src/render` | working ⚠️ | **11** | 10 | ❌ | 1 |
| `src/components` | working | **10** | 2 | ✅ stress_churn | 0 |
| `src/core/memory` | hardened | **10** | 0 | ✅ stress ×2 | 1 |
| `modules/assetlib` | hardened | **10** | 0 | ✅ fuzz ×3 | 0 |
| `src/animation` | working | **9** | 2 | ❌ | 0 |
| `src/runtime/jobs` | working ⚠️ | **8** | 2 | ❌ | 1 |
| `src/project` | working ⚠️ | 6 | 0 | ✅ stress_physics | 0 |
| `src/assets` | hardened | 6 | 3 | ✅ fuzz + stress | — |
| `src/runtime` | hardened | 6 | 16 | ✅ fuzz ×2 + soak | — |
| `src/runtime/platform` | *inherited* | 5 | 1 | ❌ | 0 |
| `src/runtime/services` | *inherited* | 5 | 11 | via parent | 1 |
| `src/runtime/input` | *inherited* | 4 | 3 | via parent | 1 |
| `src/render/world` | working | 4 | 2 | ❌ | 0 |
| `src/runtime/scripting` | *inherited* | 3 | 10 | ❌ | 0 |
| `src/scene` | hardened | 3 | 9 | ✅ fuzz ×2 | 0 |
| `src/plugins` | working ⚠️ | 2 | 8 | ✅ stress_physics | 0 |
| **`modules/hid`** | **none** | 2 | 0 | ❌ | 0 |
| **`src/audio`** | **none** | 1 | 3 | ✅ asan + conformance | **5** |
| `src/systems` | working | 1 | 4 | ❌ | 0 |
| `src/tools` | — | 0 | 13 | ❌ | 3 |
| `src/editor` | prototype ⚠️ | 0 | 16 | ❌ | 0 |

Read the top rows as the risk list. Read the bottom rows as the reason the editor
being `prototype` costs nothing: **nothing depends on it.**

## 3. Four findings

### 3.1 Two subsystems are invisible to the tier system

- **`src/audio` has no `info.md` at all.** It is also the **densest defect cluster
  in the engine, and it is one FILE**: BUG-0008, BUG-0022, BUG-0024, BUG-0025 and
  BUG-0026 all name `src/audio/miniaudio_provider.cpp` — five of the engine's
  thirty-nine ledger entries out of a single source file. Two more (BUG-0019,
  BUG-0020) come from its Rust conformance suite. **Seven of thirty-nine, about
  18%, from a subsystem that does not appear on the status page.**

  Densest among `src/` subsystems — five, against three for the next. One file in
  the whole repo ties `miniaudio_provider.cpp` at five entries, and it is
  `scripts/engine_doctor.py`. That is §6's caveat arriving early: the doc gate is
  not the second-buggiest thing here, it is the thing that gets re-read most, and
  five of its defects are written down because five of them were looked for.
- **`modules/hid` is a git SUBMODULE**, and `engine_doctor` deliberately skips
  submodule-owned docs — *"another repo's contract"*. It therefore cannot appear
  on `ENGINE_STATUS.md` no matter what its `info.md` says.

  > **Corrected 2026-08-29.** This entry originally read "`modules/hid/info.md`
  > exists and has no front-matter, so it parses as an unclassified document",
  > and listed it beside `src/audio` as the same kind of reporting gap. That was
  > wrong. The missing front-matter was real and has been added, but it was never
  > the reason for the absence: a tool that cannot see a submodule's git history
  > has no business asserting whether its docs are stale, so refusing to make the
  > claim is the correct behaviour and not a gap to close.
  >
  > The error is the same one §3.1 is *about* — an enumeration that assumed every
  > directory under `src/` and `modules/` plays by the same rules, and was wrong
  > by exactly the entry that does not. Third time in this document's short life
  > (see also the stale count in §4, which was seven and is eight).

Neither is a code problem. Both already have test evidence. They are *reporting*
gaps, and they are the reason this audit had to be assembled by hand rather than
read off `ENGINE_STATUS.md`.

### 3.2 The middle layer is under-hardened relative to its fan-in

Sorted by dependents, the first `working` subsystem appears at **rank 2**:
`src/render` with eleven dependents, then `components` with ten and `animation`
with nine. Three of the five highest-fan-in subsystems in the engine are not
hardened, and two of those have no endurance lane at all.

This is not an accusation that the tiers are wrong — each is honestly earned under
`engine_doctor`'s rules. It is that **the ladder was climbed bottom-up and
stopped**, and the remaining rungs are the ones with the widest blast radius.

### 3.3 `src/runtime`'s single `hardened` claim covers four unequal areas

`src/runtime/docs/info.md` declares `covers: src/runtime/`, so `input` (10 files),
`platform` (13), `scripting` (6) and `services` (8) all inherit one tier and one
`verified:` date. Only `jobs` has its own `info.md`.

`runtime/platform` is the weakest inheritor: it holds the Windows port surface,
and `ci.yml` records that `title_bar_windows.cpp` "was written without a Windows
toolchain to check it against". It claims `hardened` through a parent document
that was verified against a different set of files.

Tier inheritance is not wrong — most subdirectories do not deserve their own
document. But it should be a decision, not an accident, and `runtime/platform`
is currently the accidental case.

### 3.4 `src/render` is the one place where hardening now would be wasted

Eleven dependents and no endurance lane says "harden it". The renderer programme
says the opposite for half of it: `renderer-program.md` P7–P8 replace the
bgfx-coupled submission path outright.

The split is clean and already exists in the tree:

- **`src/render/world`** (`rworld::`) is GPU-free pure functions — sort keys, LOD
  selection, light packing, frustum math. `renderer-program.md` §4 explicitly
  keeps it, and P3/P4 build on it. **Harden this.**
- **`src/render`** proper is the bgfx-coupled part being replaced. **Refresh its
  stale doc, add no new tests**, and let the programme's own exit criteria carry
  it.

Hardening the half that is scheduled for replacement is the one piece of work in
this audit that would be thrown away.

## 4. The work order

Ranked by (fan-in × tier gap), with cheap-and-unblocking work first. Each rank is
independently worth doing.

### Rank 0 — make the invisible visible *(hours)*

| # | Work | Why first |
|---|---|---|
| 1 | `src/audio/info.md` — declare a tier against the tests that already exist (`audio_abi_conformance`, `audio_provider_asan_test`) | The densest defect cluster in the engine is not on the status page. Costs hours; no new tests needed |
| 2 | `modules/hid/info.md` — add front-matter | **Done, and it does not do what this table originally claimed.** `hid` is a submodule, so it stays off `ENGINE_STATUS.md` by design (§3.1). Worth having anyway: the module is meant to be vendored standalone, so it should be self-describing in its own repo |
| 2b | Register `hid_ring_test` in ctest | Found while doing #2. It had an `add_executable` and **no `add_test` anywhere** — compiled by every build, run by nothing, and passing the whole time. BUG-0010's shape, a third time |

**You cannot rank what you cannot see.** These two are not improvements to the
engine; they are improvements to the audit, and everything below is more
trustworthy once they land.

### Rank 1 — the high-fan-in middle layer *(the real work)*

| # | Work | Why |
|---|---|---|
| 3 | **`runtime/jobs` → hardened.** A soak or stress lane over the job graph, plus refresh the stale doc | The only subsystem meeting all three criticality conditions (§1). 8 dependents, a shipped livelock only TSan found, no endurance lane. Also a hard prerequisite for the FTL fiber swap (Phase H #34) |
| 4 | **`src/components` → hardened.** It already has `stress_churn`; this is mostly doc work plus the fuzz question | 10 dependents and the cheapest remaining rung on the ladder |
| 5 | **`src/animation` → hardened.** Needs an endurance lane it does not have | 9 dependents, and the renderer programme stacks per-frame BLAS refits for skinned meshes on top of it (`rhi-design.md` §4.5) |
| 6 | **`src/render/world` → hardened.** GPU-free, so a headless lane is possible today | 4 dependents, survives the RHI, and P3/P4 build directly on it |

### Rank 2 — granularity and signal *(cheap)*

| # | Work | Why |
|---|---|---|
| 7 | Give `runtime/platform` its own `info.md` at its earned tier | It inherits `hardened` from a parent verified against different files, and holds code written without the toolchain to check it (§3.3) |
| 8 | Clear the **8 stale docs** | Seven `info.md` — `render`, `project`, `plugins`, `jobs`, `packaging`, `cookers/material`, `editor` — plus `docs/architecture/asset-cook-architecture.md`. Each is a doc whose covered code moved. Cheap, and stale entries are how a warning list stops being read |

The stale set is whatever this prints, not a number to trust from this page:

```bash
python3 scripts/engine_doctor.py check | grep STALE
```

Worth one sentence, because the miss is instructive: this row said **seven** in
its first draft, because the list was assembled by walking `info.md` files and
`asset-cook-architecture.md` is the only stale doc that is not one. An audit that
enumerates by file *name* rather than by asking the tool that owns the answer will
be wrong by exactly the entries that do not fit the naming convention — which is
the same failure §3.1 records for `src/audio` and `modules/hid`, one level up.

### Rank 3 — deliberately deferred

| Work | Why not now |
|---|---|
| `src/render` (the bgfx half) | Scheduled for replacement by P7–P8. Refresh the doc, add no tests (§3.4) |
| `src/editor` staying `prototype` | Zero dependents. Its tier costs nothing, and `editor_undo_test` already covers the one piece of consequential logic |
| `modules/net` staying `prototype` | Netcode is post-export (Phase H #36); hardening a prototype nobody consumes yet is speculation |

## 5. Future subsystems, ordered by what they actually depend on

The repo's existing rule is that speculative subsystems get **triggers, not
schedules** (`infrastructure-backlog.md` Phase H). This section keeps that and
adds the dependency each one is really waiting on — which in two cases is not
what the backlog says.

| future subsystem | technically depends on | the real prerequisite | trigger |
|---|---|---|---|
| **Crash reporting** (`future-plans/crash-reporting.md`) | nothing — a `modules/`-level kit like `assetlib` and `hid`, zero engine deps | **`src/tools/packaging`.** The design note names *"symbol archiving keyed by build id"* as the value, and build ids come out of `engine_build`. Packaging is `working` and **stale** | first shipped build with an external user |
| **FTL fiber backend** (H #34) | `runtime/jobs` facade (ready) | **`runtime/jobs` hardened** — swapping the scheduler under 8 dependents with no endurance lane is how BUG-0003 happens again | job graphs deep enough that blocking waits dominate |
| **Dedicated / headless server** (H #31) | multi-instance runtimes | **`renderer-program.md` P1** — de-contaminating the 5 files that mix loading with GPU upload *is* the headless-server refactor. Already scheduled, under another name | PIE, second viewport, or a server target |
| **Memory budgets + forensics** (H #33) | `core/memory` (hardened ✅) | none — genuinely unblocked | first real memory hunt |
| **C# scripting / netcode** (H #36) | `runtime/scripting`, `modules/net` | `modules/net` is `prototype` with one test; `runtime/scripting` has no endurance lane and inherits its tier | after the export milestone |

Two things worth pulling out of that table:

**Crash reporting is not blocked on crash capture.** Crashpad already captures.
It is blocked on the packaging path being trustworthy enough to archive symbols
against a build id — so `src/tools/packaging` (currently `working`, stale since
2026-08-02, one test) is on the critical path to a subsystem nobody has connected
it to.

**The headless server and the RHI de-contamination are the same work.** P1 exists
in the renderer programme and pays for itself even if the RHI is abandoned; it is
also the only thing standing between here and a server target. It should be
counted once, not twice.

## 6. What this audit does not claim

- **No judgement about code quality.** Tier is a statement about *evidence*, not
  craft. `src/editor` is `prototype` because nothing depends on it, not because it
  is bad.
- **Fan-in is measured from `#include` edges**, which over-counts header-only
  utility dependencies and under-counts runtime coupling through the API table.
  It is a good proxy and not a proof.
- **Defect counts are ledger entries, not defects.** A subsystem with five entries
  may simply have been looked at harder — `src/audio` has a Rust conformance suite
  and an ASan lane pointed at it, which is exactly why its defects are *written
  down*. Density here partly measures attention, and the honest reading is that
  the subsystems with zero entries are the ones nobody has aimed a lane at yet.

That last point cuts against the audit's own headline finding and belongs in it.
