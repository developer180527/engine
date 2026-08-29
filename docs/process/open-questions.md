---
status: reference
---
# Open — known, unpinned, and deliberately visible

**Things that are wrong, or are a deliberate cost, and have no regression test.**

The other half of `bug-ledger.md`'s honesty: the ledger says what is fixed and
proven, this says what is known and not.

## What does NOT belong here, and why the list is short

This file was 598 lines and 46 items on 2026-08-29, and a classification found
that **the title was true of seven of them.** The rest were doing three other
jobs:

| what it was | count | where it went |
|---|---|---|
| Windows / cross-ISA port findings, all FIXED | 25 | [`port-log-windows.md`](port-log-windows.md) |
| fixed defects **duplicating** a `BUG-NNNN` entry | 8 | deleted; the ledger is the single source of truth |
| fixed defects with **no ledger entry at all** | 4 | BUG-0040 … BUG-0043 |
| stale — the thing it described was no longer true | 1 | deleted |
| a note about an existing entry | 1 | folded into BUG-0009 |

So three rules, and they are what keep this file from becoming what it was:

1. **Fixed goes to the ledger, not here.** If it has a mechanism and a fix it is a
   `BUG-NNNN` with a `pinned-by`. Four defects sat only in this file, findable by
   nobody, because that rule was not written down.
2. **Nothing is duplicated from the ledger.** Eight items described a defect that
   already had a numbered entry and a test. When two records of one defect drift,
   nothing says which is true.
3. **An item that turns out to be FIXED is a finding**, exactly like a stale doc —
   and unlike a doc, nothing here is machine-checked, so it is found by reading.
   `CI has never run the unit lane` survived here long after `ci.yml` grew
   `ctest -L unit`.

## The schema

Free prose, with three optional trailers a human should be able to answer:

* **since** — when it was first recorded, so age is visible.
* **where** — the file or subsystem it concerns. `engine_doctor` checks this path
  exists, the same check the ledger's `where:` gets.
* **trigger** — what would make this worth doing. An item with no trigger is a
  complaint; an item with one is scheduled work that has not come up yet.

---

- **A portability regression can now sit on main for up to a day.** The gating
  macOS leg runs on every push; the four Linux/Windows legs run nightly and on
  `workflow_dispatch`. Deliberate — wall clock was 11m27s set entirely by
  Windows, for a verdict macOS had already given, and the test lane inside it is
  20 seconds. All five legs are `experimental: false`, so the nightly is a hard
  failure rather than an amber note, and the full matrix is one dispatch away
  before anything that matters. Revisit if a regression ever survives a night.

- **The push path is now bounded by TSan (~6m50s), not macOS (~4m53s).** Left
  there on purpose: the sanitizer lanes are the highest-yield minutes in the
  whole workflow — BUG-0001, 0002 and 0003 all came from them — so moving them
  off the push path to save under two minutes would undercut the reason most of
  this ledger exists. If pushes need to get faster again, TSan is the next
  candidate and the cost is stated here rather than discovered.

- **The build is ~110 link steps with a 97% compiler-cache hit rate.**
  Compilation is already free; the six minutes are ~85 test executables each
  statically linking the whole engine, and sccache does not cache links. Trimming
  targets cannot move that — the levers are a shared engine library or fewer,
  larger test binaries, both with real tradeoffs.

These are known, unpinned, and deliberately visible rather than tidied away.

- **Cooked formats have an independent reader now — every one of them.**
  `tests/cooked_format/` parses `.cooked`, `.cmat`, `.ctex`, `.cshader` and the
  DDC record manifest from outside C++, with no engine code. A C++ save/load
  round-trip proves the writer and reader agree WITH EACH OTHER and nothing
  more; both can move together and stay green.

  What remains open is the second half of the same idea: the readers pin
  STRUCTURE, not content equality across platforms. Diffing two machines' cooked
  bytes over the same corpus is the DDC's load-bearing assumption (assetlib
  issues.md O2) and the cross-ISA determinism lane in the soak plan — and this
  crate is now the tool that could do it without either engine running.

- **Windows kits are blocked on design, not a flag.** `hot_reload_game` is a
  MODULE that deliberately links nothing and resolves engine symbols from the
  host executable at load — a Unix idiom. A Windows DLL must resolve every symbol
  at link time, so it fails LNK2019 on flecs' globals. Making it work means
  `engine_host` EXPORTING the symbols a module may use (Windows exports nothing
  from an EXE by default, and WINDOWS_EXPORT_ALL_SYMBOLS covers DLLs only) and
  the module linking the generated import library. Both Windows legs otherwise
  report ZERO compile failures and build all 76 tests.

- **The ledger is not updated by the gate that checks it.** `engine_doctor`
  validates entries that EXIST — ids unique, class known, `pinned-by` present —
  and structurally cannot notice an entry that should exist and does not. Three
  commits fixing seven defects landed before this was spotted, and only because
  someone went looking. Nothing automated will catch the next one; the discipline
  is "a fix and its entry land together", and that is a human rule.

- **The backfill is incomplete.** Twelve `issues.md` files hold roughly 2,400
  lines of documented, already-fixed defects. The entries above are today's
  findings plus the recent ABI and audio work. Older subsystems — the renderer's
  R10–R21, the asset cookers, assetlib — are not yet indexed, and until they are
  the histogram under-reports.
