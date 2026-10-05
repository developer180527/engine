---
status: plan
id: WO-057
title: The RHI's home — a module that builds, tests and ships without the engine
program: renderer
priority: P1
size: S
state: todo
depends: [WO-056]
touches:
  - docs/rhi/README.md
  - CMakeLists.txt
source: DR-0012 (2026-10-05)
---
## Why
DR-0012 makes the RHI a library for more than this engine. Independence that
is only intended decays: one engine include added to fix one compile error and
it is engine code again. It has to be a build fact from the first commit.

## Done when
- [ ] `rhi/` exists with its own CMake project, configurable alone (`cmake -S rhi`), with its own public headers, tests and one sample
- [ ] an `engine_audit` rule fails if anything under `rhi/` includes engine, bgfx or bx headers, mutation-checked
- [ ] a CI job builds and tests `rhi/` alone on macOS and Linux
- [ ] the engine consumes it only through its installed public headers and C ABI
- [ ] the repository split trigger is written down (see `docs/rhi/README.md`)

## Contract
Nothing: no engine API changes until G4 ports a pipeline.
