---
status: as-built
verified: 2026-09-16
covers:
  - scripts/engine_audit.py
  - scripts/audit_cron.sh
  - scripts/audit_baseline.json
tests:
  - tests/CMakeLists.txt
---
# The architectural audit

`engine_doctor.py` checks the **document** contract — does a doc exist, is it
stale, does its tier claim have evidence. It says nothing about the **shape** of
the code. This tool checks the shape.

> **The rule behind it is the same one `engineering-standards.md` opens with:** a
> claim nobody can check is a claim that will quietly stop being true. The shape
> rules were all prose, in thirteen different `info.md` files, and prose decays
> in one specific way — not by being repealed, but by one `#include` added to fix
> one compile error in a file nobody thought of as belonging to that layer.
> That is exactly how the GPU seam drifted in four places before
> `check_gpu_seam.py` existed. This generalises that script to the other
> boundaries the engine claims to have.

## 1. Running it

```bash
python3 scripts/engine_audit.py                  # human report
python3 scripts/engine_audit.py --check          # gate: fails on NEW findings
python3 scripts/engine_audit.py --with-external  # + doctor, gpu seam, std includes
python3 scripts/engine_audit.py --list-debt      # only the accepted findings
python3 scripts/engine_audit.py --json r.json --sqlite farm.db --run-id 7
```

It is also a ctest lane: `ctest -L audit` (and it runs inside `-L unit`, so CI
gates on it like the other seam checks). No build, no GPU, ~2 seconds.

## 2. The rules

Each one is a claim some document already makes. The `source` column is where
the claim lives, because a check whose rationale is only inside the checker is a
rule nobody agreed to.

| id | rule | stated in |
|---|---|---|
| `LAYER-01` | `src/core` includes no renderer, ECS or editor header | `src/core/info.md` |
| `LAYER-02` | nothing outside `src/editor` includes the editor or ImGui | `engineering-standards.md` §7 |
| `LAYER-03` | no new module→module edge without a decision | `docs/plans/subsystem-audit.md` §2 |
| `LAYER-04` | `src/render/world` is GPU-free *and* runtime-free | `src/render/world/info.md` |
| `ABI-01` | every API group has a frozen size, a pinned offset, a client guard row and a host row | `extension-model.md` §1.3 |
| `ABI-02` | group offsets tile with no gap and no overlap | `engine_api_table.h` |
| `ABI-03` | `api_abi_compat_test`'s frozen list covers every group | `tests/api_abi_compat_test.cpp` §1 |
| `ABI-04` | every kit-visible component is in `componentLayoutHash` | `tests/component_abi_test.cpp` |
| `TEST-01` | every fuzz target has a corpus with seeds, and vice versa | soak-fuzz plan §1.4 |
| `TEST-02` | every test file is registered with ctest | `engineering-standards.md` §4.1 |
| `DET-01` | the fixed step reads no clock and no RNG | `src/runtime/docs/info.md` |
| `HDR-01` | the C ABI headers pull in nothing of ours | `extension-model.md` |
| `DOC-01` | every directory of code is covered by some document | `engineering-standards.md` §1 |

`bx` is **not** treated as a graphics dependency, matching `check_gpu_seam.py`'s
exclusion — it is the math library, and conflating the two would make
`core/transform.h` a violation for using `bx::Vec3`.

## 3. The baseline, and why the gate is a ratchet

`scripts/audit_baseline.json` records the findings that existed when each rule
was written. `--check` fails only on findings **not** in it.

This is not leniency, it is the only way adoption works. A new checker that
reports 40 pre-existing violations on day one becomes a red light everyone
learns to walk past, and then it is worse than nothing. The debt stays visible —
every report prints it, `--list-debt` prints nothing else — while new violations
are blocked.

Signatures are `rule|subject` with **no line numbers**, so moving a violation
around a file does not read as "fixed one, found another".

Shrinking the baseline is the cleanup. Adding to it requires `--update-baseline`
and a reason in the commit message; nothing else should ever write that file.

## 4. What it found, and what that means

Current state (2026-09-16): **54 findings, all baselined**, of which 51 are the
existing module→module edges recorded as the declared dependency graph. What is
left, and what the first rule already caught and closed:

- **`LAYER-01` — found 2, now FIXED.** `src/core/entity_id_util.h` and
  `src/core/transform_utils.h` both included `<flecs.h>` while `src/core/info.md`
  says core may include no ECS header. The code moved rather than the rule being
  softened: `entity_id_util.h` went to `components/` whole, and
  `transform_utils.h` was split — the matrix math stayed in core, the
  `flecs::entity` walkers became `components/transform_hierarchy.h`. The
  destination was chosen *by* `LAYER-03`: every caller already depends on
  `components/`, so it adds no module edge, where `scene/` would have added two.
- **`ABI-03` ×2** — `physics2` and `intent`, the two most recently appended API
  groups, are absent from `api_abi_compat_test`'s `frozen[]` list. The header's
  `static_assert`s still pin them, so this is a **gap in the runtime test**, not
  an unprotected ABI: the test's stated promise ("a v1 kit must run on a v5
  host") is currently checked for 13 of 15 groups.
- **`DET-01` ×1** — `hid::nowNs()` inside the fixed step, which the runtime's own
  doc already lists as an open hazard ("inert while no tier reads input, and the
  next thing to close if one does").

Everything else is clean, including two things worth stating because they are
easy to assume otherwise: the editor really is a leaf, and every component
re-exported to kits really is in the layout hash.

## 5. What this tool cannot do

Stated plainly, because a green light that is trusted too far is the failure
mode of every checker:

- **It reads text.** No C++ parse, no macro expansion, no `#ifdef` evaluation. It
  is a lint, not a compiler.
- **It only sees tracked files.** A brand-new file is invisible until `git add`.
  That is deliberate (`Kits/` and `fps_shooter/` are gitignored and not ours to
  gate) but it means the check on a work-in-progress file comes one step late.
- **It cannot judge design.** Whether a boundary *should* exist, whether an
  abstraction earns its weight, whether a 900-line file is too big, whether a
  dependency is reasonable — all human calls. `LAYER-03` does not say an edge is
  wrong; it says an edge is *new*, and asks for a decision.
- **It is not a substitute for the build.** Real layering violations that only a
  linker sees (a symbol pulled through a static library) are invisible here. The
  shipping-runtime CI leg, which asserts Assimp is not linked into the player, is
  the check for that class.

## 6. Running it periodically on the home server

The audit needs no build and no GPU, which makes it the cheapest lane the farm
has and the only one worth running on **every** commit rather than nightly.

`scripts/audit_cron.sh` is the entry point: it fetches into its own checkout,
runs the audit with `--check --with-external`, writes a report plus JSON, appends
to the SQLite results DB, and alerts only when the exit code says something
**new** appeared. Install it as a systemd timer (the header of the script has the
unit files) or a cron line.

It obeys the rule the farm plan already sets for itself
(`automated-testing-soak-fuzz-plan.md` §7.4): **the server never pushes.** It
writes a report and rows; a human decides what to do.

### The database

Two tables, added to the farm schema from that plan's Part 5 and following its
§5.3 dedup-at-the-source shape:

```sql
CREATE TABLE audit_run (
    id INTEGER PRIMARY KEY, run_id INTEGER,   -- farm run(id) when soakd drives it
    commit_sha TEXT NOT NULL, at_utc TEXT NOT NULL,
    new_count INTEGER NOT NULL, debt_count INTEGER NOT NULL);

CREATE TABLE audit_finding (
    signature TEXT PRIMARY KEY,               -- rule|subject, stable across edits
    rule TEXT, subject TEXT, detail TEXT, severity TEXT,
    first_seen TEXT, last_seen TEXT, first_commit TEXT, last_commit TEXT,
    hit_count INTEGER DEFAULT 1, baselined INTEGER NOT NULL);
```

Because a finding is keyed by signature with `first_commit`/`last_commit`, the
questions that matter are one query each: what appeared this week, what has been
debt for six months, which rule produces the most churn, and — the one a report
file cannot answer — *when* a violation first entered the tree.

```sql
-- new since a date
SELECT rule, subject, first_commit FROM audit_finding
WHERE baselined = 0 AND first_seen > '2026-09-01';

-- debt, oldest first: the cleanup queue
SELECT rule, subject, first_seen FROM audit_finding
WHERE baselined = 1 ORDER BY first_seen;
```

### Suggested cadence

| lane | cost | when |
|---|---|---|
| `engine_audit --check --with-external` | seconds, no build | every commit / hourly |
| `ctest -L unit` in the Linux container | minutes | every commit |
| `-L fuzz-regress`, `-L stress` | minutes | nightly |
| `-L soak`, fuzz explore | hours–days | the farm's long lanes |

The audit's place in that list is the point: it is the only one that answers
"has the architecture drifted?" and it costs nothing, so there is no reason for
it to be the lane that runs least often.

## 7. Adding a rule

1. Write a `rule_*()` returning a `Rule` with an id, a title, the **document that
   already states the claim**, and a `why` that names what breaks.
2. Add it to `RULES`.
3. Run `--update-baseline` if it finds existing violations, and say in the commit
   why they are accepted rather than fixed.

A rule whose `source` is empty is a rule someone invented while writing a
checker. Write the claim into the subsystem's `info.md` first, where it can be
argued with.
