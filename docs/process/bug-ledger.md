---
status: reference
---
# The Bug Ledger

**Every bug this engine has had, and the test that stops it coming back.**

Append-only. Entries are never deleted and ids are never reused, because the
value of this file is that it accumulates — the same reason a B-Rep kernel keeps
the model that broke booleans in 1997. Anyone can write the algorithms; what
cannot be reproduced is thirty years of knowing which specific inputs break
them.

## The rule

> A bug is not fixed when the code changes. It is fixed when a test fails
> without the change and passes with it.

`engine_doctor.py check` enforces the half a script can enforce: **every entry
must name a `pinned-by` test that exists.** An orphaned proof is an error, not a
warning, because a ledger of unenforced good intentions reads as coverage that
is not there.

```bash
python3 scripts/engine_doctor.py bugs              # list, with a class histogram
python3 scripts/engine_doctor.py bugs --class threading
python3 scripts/engine_doctor.py check             # gate (runs in the docs lane)
```

## Why this exists next to `issues.md`

`issues.md` files are *narrative*: what was wrong with a subsystem, what the
options were, what was chosen. They are long and worth reading. This is an
*index*: one line per defect, queryable, and mechanically tied to a test.

They serve different questions. "Why is the renderer shaped like this?" →
`issues.md`. "Which bugs have no regression test?" → here.

## Fields

| field | meaning |
|---|---|
| `found` | date the defect was identified |
| `status` | `fixed` or `open` — see below; decides what `pinned-by` may claim |
| `class` | see below — decides the shape of the pinning test |
| `where` | the file the defect lived in |
| `symptom` | what was observed |
| `cause` | the actual mechanism, not the theory |
| `pinned-by` | **the test that fails without the fix** — checked to exist. `none` when `status: open` |
| `lane` | which lane surfaces it (`unit`, `asan`, `tsan`, `fuzz-regress`, …) |
| `proof` | how the regression test was verified to catch it |

### `status`, and the blindness that made it necessary

`pinned-by` used to be required unconditionally, so an entry recording a bug that
was **found but not fixed** had nowhere to say so. Two entries solved it by
writing `pinned-by: docs/process/bug-ledger.md` — the ledger pointing at itself.
The gate only checks that the path exists, and that path is the file doing the
checking, so those entries could never fail it.

The cost was real. **BUG-0023 read "NOT FIXED" for two days after its fix
shipped**, and nothing could notice, because a self-referential pin is
indistinguishable from a real proof to a gate that only tests existence. A ledger
reporting a gap that is already closed is worse than one missing the entry: the
next person re-does the work.

So the state is a field, and the gate enforces what each state may claim:

| `status` | `pinned-by` | enforced as |
|---|---|---|
| `fixed` | a test that exists | missing → **error** |
| `open` | `none` | claiming a proof → **error** |
| either | never the ledger itself | self-pin → **error** |

`engine_doctor.py bugs --open` lists the unfixed ones. An open entry that turns
out to be fixed is a finding, exactly like a stale doc — that is the failure this
schema exists to make visible, not to prevent.

### Classes, and the test each one implies

The class is not bookkeeping — it answers *"what kind of test do I write?"*
without leaving it to judgement each time.

| class | typical defect | pinned by |
|---|---|---|
| `memory` | out-of-bounds, use-after-free, leak | ASan/UBSan lane + a targeted test |
| `threading` | race, deadlock, livelock | TSan lane + stress |
| `abi` | layout, versioning, symbol contract | static asserts + an **offset** test |
| `parse` | malformed or hostile input | a corpus seed |
| `numeric` | determinism, precision, overflow | golden-value test |
| `perf` | regression in time or memory | perf test with a threshold |
| `build` | link graph, target wiring, packaging | assert on the built artifact |
| `logic` | plain wrong behaviour | ordinary unit test |
| `coverage` | a check that silently never ran | make it run, then assert it |

`parse` bugs already have a home that predates this file:
`tests/fuzz/corpus/*/regressions.seeds`, 65 seeds and counting. Those are not
duplicated here — the seed *is* the entry. This ledger exists because the other
eight classes had nowhere to go.

---

## The entries

**One file per bug, in [`bugs/`](bugs/).** Named `BUG-NNNN-short-title.md`, so the directory listing is the index and
no generated file has to be kept in sync with it — a shape this repo has
already been bitten by (BUG-0027).

That layout exists to make `grep` answer the question directly. Entries
are ~28 lines each and the single file had reached 1051 of them in five
days, so a match needed `-B5 -A8` and could still straddle two bugs:

```bash
grep -rl "operator new" docs/process/bugs/
# -> docs/process/bugs/BUG-0028-operator-new-under-promised-alignment.md
```

The filename is the answer, and every `grep -r` match line carries the id
and title with no flags. `git log` over one file is that bug's history
alone, and two branches adding entries no longer collide on one append
region.

`python3 scripts/engine_doctor.py bugs` prints the index with classes and
pinned-by; `engine_doctor check` enforces the schema below.

---

## What is not fixed

Known gaps and deliberate costs live in
[`open-questions.md`](open-questions.md) — 46 items and growing, which is why
they are not in this file. They carry no `pinned-by` because there is nothing
to pin yet; an item earns a `BUG-NNNN` when it is fixed.
