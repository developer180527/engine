---
status: decided
verified: 2026-07-31
covers:
  - scripts/engine_doctor.py
tests:
  - tests/CMakeLists.txt
---
# Engineering Standards

How work on this engine is documented, tested, and declared done. Enforced by 
`scripts/engine_doctor.py`, which runs in CI and as the `docs` ctest lane.

> **The rule behind every rule here:** a claim nobody can check is a claim that
> will quietly stop being true. This document exists because that already
> happened — a design doc advertised a 107 ms cache restore that held only on the
> day it was measured, and a CLI reported "0 cooked" after every successful cook
> for weeks. Neither was caught by review, because review reads intent. Machines
> read the tree.

## 1\. The document contract

Every `.md` in the repo (outside `third_party/`, build dirs, and the generated `
ENGINE_STATUS.md`) carries front-matter:

```markdown
---
status: as-built            # required
tier: hardened              # required in info.md only
verified: 2026-07-31        # required when status is as-built
parses-external-input: true # optional; raises the bar at hardened+
covers:                     # code this document describes (paths or globs)
  - src/assets/
tests:                      # tests that keep this document's claims true
  - tests/cooker_test.cpp
perf-test: tests/cook_infra_test.cpp   # optional; required in spirit at production
---
```
### `status` — what kind of claim is being made


|value     |meaning                                            |staleness checked?|
|----------|---------------------------------------------------|------------------|
|`as-built`|describes code that exists; must match the tree    |**yes**           |
|`target`  |intended design, not yet built                     |no                |
|`decided` |a decision + its rationale; frozen unless revisited|no                |
|`reference`|external/API reference material                    |no                |
|`plan`    |roadmap/backlog; expected to churn                 |no                |
|`unreviewed`|bootstrapped, nobody has classified it             |no                |

A document may only describe reality *or* intent, not both silently. The cook
architecture doc is `as-built` and says in its own header which section is the
exception — that is the pattern to copy.

### Staleness is mechanical

For `as-built` docs the checker compares `verified:` against the last commit
touching `covers:`. Code newer than the verification date means **STALE**, and
the fix is to re-read the doc against the code and bump `verified:` — not to
edit the date. Bumping the date without re-reading is the one dishonest move
this system cannot detect, so it is the one thing to hold yourself to.

## 2\. The tier ladder

Declared per subsystem in its `info.md`. Every tier's evidence is checked by `
engine_doctor.py check`; you cannot claim a tier you have not earned.


|tier      |means                                |mechanically requires                                                                                 |
|----------|-------------------------------------|------------------------------------------------------------------------------------------------------|
|`prototype`|it exists and runs                   |nothing                                                                                               |
|`working` |correct on the paths we use          |≥1 real test listed in `tests:`                                                                       |
|`hardened`|survives hostile input and time      |`working` \+ ≥1 fuzz/soak/stress test \+ specifically a FUZZ test when `parses-external-input: true` \+ doc not stale|
|`production`|trustworthy on every platform we ship|`hardened` \+ exercised on all CI platforms + `perf-test:` naming the test that holds its performance claim|

Two deliberate consequences:

- **A subsystem that parses external input cannot reach `hardened` on unit tests
  alone.** Anything eating FBX, glTF, PNG, JSON, scene binaries or DDC manifests
  meets untrusted bytes, and the only honest evidence is a fuzzer.
- **A stale doc caps the tier.** If the code moved and nobody re-verified the
  description, the subsystem is not `hardened` — because nobody currently knows
  what it does.

## 3\. Claims are tests

Any *number* published in a document is either backed by a test, or explicitly
marked as a dated one-off measurement.

- Backed: "a warm pass recooks nothing" → `cook_infra_test`. This is the
  strongest kind of claim, because it fails loudly when it stops being true.
- Dated one-off: "cold cook 8 min → 1.8 s (measured 2026-07-28, M-series
  laptop)". Fine, as long as the date and machine are attached — a bare number
  reads as an invariant and rots into a lie.

When you cannot test a claim, prefer deleting it over publishing it unbacked.

## 4\. Landing checklist

What "landed" means for a change of any size:

1.  **Tests in the right lane.** `unit` for correctness (fast, gates CI), `
    fuzz-regress` for the seeded corpus, `stress`/`soak` for endurance, `asset`
    for anything needing real content.
2.  **A regression proof for bug fixes.** Reintroduce the bug, watch the test
    fail, restore. An untested fix is a hypothesis.
3.  `info.md`** updated** if behavior, invariants, or limitations moved — and `
    verified:` bumped after actually re-reading it.
4.  `engine_doctor.py check`** clean.**
5.  `ENGINE_STATUS.md`** regenerated** if any tier or doc metadata changed.

## 5\. Where truth lives


|question                  |answer lives in                                  |
|--------------------------|-------------------------------------------------|
|What is stable right now? |`ENGINE_STATUS.md` (generated — never hand-edit) |
|How does subsystem X work?|that subsystem's `info.md`                       |
|Why is it built this way? |a `decided`/`as-built` doc in `docs/`            |
|What are we going to do?  |`docs/infrastructure-backlog.md` (`plan`)        |
|What is broken in X?      |`issues.md` beside the code, with resolution status|

One rule: `ENGINE_STATUS.md`** is generated.** Editing it by hand reintroduces
exactly the hand-maintained-status-that-drifts problem it exists to remove.

## 6\. Using the tool

```bash
python3 scripts/engine_doctor.py check      # validate the contract
python3 scripts/engine_doctor.py status     # regenerate ENGINE_STATUS.md
python3 scripts/engine_doctor.py status --check   # CI: fail if out of date
python3 scripts/engine_doctor.py bootstrap  # add placeholders to new docs
```
`check` currently treats missing front-matter as a warning so the contract can
be adopted incrementally. Once `unreviewed` reaches zero, flip CI to `
\--strict-missing --warnings-as-errors` and it becomes a ratchet.

## 7\. Adoption state

Bootstrapped 2026-07-31: 49 documents under contract; **all 14 in-repo
subsystems classified** against real test evidence — 4 `hardened`, 7 `working`,
3 `prototype`. Live numbers are in `ENGINE_STATUS.md`; the remaining
`unreviewed` docs are issue logs and plans, which make no tier claim.

Git **submodules are excluded** from the contract: `modules/hid` is a genuine
engine subsystem but a separate repo, and this tool must not gate or edit files
another project owns. Classify it there.

Three subsystems sit at `prototype` because the evidence says so, and each
`info.md` records exactly what would raise it:

- `src/render` — the pipeline has no test at all (no GPU harness); registries
  are only exercised incidentally by asset tests.
- `src/systems` — `AnimatorSystem` has no direct test; notably its
  `>kMaxBones` guard is a silent-corruption path.
- `src/editor` — zero automated tests, deliberately contained (nothing else
  depends on it).

And the most-tested subsystem in the tree is deliberately **not** `hardened`:
`src/runtime` has 11 tests including a days-long soak, but its own parsers
(`input.json`, cooked scenes) have never been fuzzed. Volume of tests is not
adversarial coverage — that distinction is the entire value of the ladder.

