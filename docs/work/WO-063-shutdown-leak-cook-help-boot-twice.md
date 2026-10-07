---
status: plan
id: WO-063
title: "Small idle-cost bugs: a program reference leaked at shutdown, `engine_cook --help` cooks, boot does work twice"
program: providers
priority: P1
size: S
state: todo
depends: []
touches:
  - src/render/shader/shader_library.cpp
  - src/render/pipeline/programs.cpp
  - src/runtime/input/input_manager.cpp
  - src/tools/engine_cook.cpp
source: review 2026-10-07, items 5 and 6, and "found on the way"
---
## Why
Every run logs `program still referenced at shutdown: standard [metal]` twice: something takes a reference to the standard shader program and never gives it back.

Three smaller things from the same review:
- **`engine_cook --help` runs a real cook.** It does not know `--help`, auto-detects a project from the working directory, and cooks it. On 2026-10-07 it re-cooked a project in another session's folder. Every other tool answers `--help` and refuses unknown options (`engine_build` exits Usage, 2).
- **Boot builds the standard shader program twice**: compiled in, then replaced by the cooked one as soon as the project opens.
- **Boot loads the input contexts twice** (`[Input] 1 context(s) loaded` appears twice in every log).

Boot is 0.32 s, so the last two are about doing work once, not speed. The review also counted about 70 000 allocations during boot, about 56 000 of them freed at once; that is recorded here as a measurement, not a target.

## Done when
- [ ] the leaked reference found and released; shutdown logs nothing about referenced programs. A test (or the existing shutdown path under a test) fails if a program is still referenced at shutdown
- [ ] `engine_cook --help` prints usage and exits 0 without touching any project; an unknown option exits Usage (2) without cooking; a test covers both
- [ ] the compiled-in standard program is built only when no cooked one will be used (or, if the project is not known yet at that point, the reason is written down and the cost measured)
- [ ] input contexts load once per project open
- [ ] boot allocations measured before and after, recorded in the log

## Contract
Nothing: no interface changes.
