---
status: as-built
tier: working
verified: 2026-11-26
parses-external-input: true
covers:
  - src/project/
tests:
  - tests/providers_test.cpp
  - tests/provider_resolve_test.cpp
  - tests/stress_physics.cpp
---
# Project

## Purpose
Project management (engine_core — shared by the editor hub, the
engine_project CLI, and SDK users): what a project IS on disk, creating new
ones, and the user-level list of known projects.

## Canonical layout (v2)
```
MyGame/
├── project.json      version 2, name, engine, assetRoot, lastScene, template
├── assets/           THE content root (scenes/ + scripts/autorun/ inside)
├── .cache/           generated (registry.db, cooked) — gitignored
└── .gitignore
```
v1 layouts (scenes/ + scripts/ at the project root, like the engine repo
itself) keep working — loaders fall back, nothing migrates in place.

## Contents
- **`project_context.h`** — loads/saves `project.json`; `autoDetect()`
  prefers the last-opened project; `load(root)` is the explicit form. Opened
  at `EngineRuntime::init` or later via `openProject` (editor hub flow).
  Its `"providers"` block names one provider per kind (`physics`, `scripting`,
  `audio`, `renderer`), as a name or per target (WO-086). It is kept as written
  (`Providers::raw`) and saved back unchanged; the runtime resolves it for the
  machine it runs on.
- **`provider_resolve.h/.cpp`**: `provsel::resolve`, the one function that
  picks a provider per kind for a target (platform + flavour) or refuses by
  name. `engine_build` runs it from the manifests in the source tree, a
  development runtime from what CMake compiled in. Pure: no engine headers.
  Tested by `tests/provider_resolve_test.cpp`, one case per check.
- **`project_scaffold.h/.cpp`** — `project::create(dir, name, template)`
  generates the structure. Templates: "basic3d" (camera + sun + cube scene),
  "empty".
- **`known_projects.h`** — the hub's project list at `~/.engine/projects.json`
  (name, path, lastOpened). Merges the legacy `last_project.txt` on load.
