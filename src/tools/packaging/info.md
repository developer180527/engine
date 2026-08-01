---
status: as-built
tier: working
verified: 2026-08-02
covers:
  - src/tools/packaging/
tests:
  - tests/package_closure_test.cpp
---
# Packaging closure

## Purpose
Answer four questions about what a shipped `dist/` must contain, as pure
functions that can be tested without running a build:

| Function | Question |
|---|---|
| `sceneMeshRefs` | Which objects ship at all? |
| `meshClosure` | What does each mesh need in order to look right? |
| `shaderFiles` | Will the shading the project authored actually run? |
| `materialFiles` | Will the looks the project authored be available? |

## Why this exists as a module
The first three lived inline in `engine_build.cpp`'s main flow, reachable only by
running a full project build — kits compiled, player copied, the lot. So none of
them had a test, and two shipped real bugs. `materialFiles` was written here from
the start, for the same reason.

They share one failure signature, which is the reason to be careful here:

> **They succeed loudly and break silently.** The build prints `DONE`, the
> package looks plausible, the game launches and runs. What is missing is
> content, and content that is absent looks like content that was never
> authored.

| Bug | What shipped |
|---|---|
| Sibling `.ctex` copied by guessing `<meshUuid>_t*.ctex` | Every mesh present, **every surface untextured** |
| Unreadable cooked scene hit a bare `continue` | **Every object in that scene gone** |
| Shader files counted, not name-checked | Game renders on **compiled-in fallbacks**; custom shading never runs |

The first arrived from an entirely ordinary flow — a clean rebuild against a warm
shared DDC — because the DDC restores siblings under the names they were *first*
written with while the mesh output takes the *current* uuid, so the filename
prefixes stop matching. That is what a CI runner or a second dev machine does.

## The rule
> **Ship what the cooked asset REFERENCES, never what its filename suggests.**

A reference cannot drift from what the runtime resolves, because it is the same
string. A filename heuristic is a second, silent source of truth.

For shaders and materials the equivalent rule is **check names, not counts**: the
runtime resolves by the name declared *inside* each `.cshader`/`.cmat` (a dist has
no registry to map paths to uuids), so a package can hold plenty of files and
still provide nothing the game asks for. Both ship wholesale rather than by
closure — nothing in a scene references a material yet, and a `.cmat` is a few
hundred bytes — so the diagnostics below are the only thing standing between a
shadowed name and a silently wrong look.

## Diagnostics are part of the return value
Each function reports what it *could not* ship — `missing`, `unreadableScenes`,
`unreadable`, `duplicateNames` — rather than skipping past it. `engine_build`
turns each into a warning naming the file. A packager that silently drops things
is how all three bugs survived.

## Determinism
Directory walks are sorted. `directory_iterator` order is filesystem-defined, and
a package that differs run to run makes "did this build change?" unanswerable —
the difference would only ever surface as a mysterious diff in shipped bytes.

## Known limitations
- **Not everything in `engine_build` is covered.** Kit compilation, the player
  copy and `assets/` whitelisting are still inline and untested. They fail
  loudly (a missing `.so` or player binary is immediately obvious), which is why
  they were left; worth revisiting when the Windows port makes packaging
  cross-platform.
- **The ordering assertions are partly vacuous.** They detect an unsorted walk
  only on a filesystem that returns creation order; on one that returns sorted
  entries they pass either way. Stated here and in the test, because a test that
  cannot fail is worse than no test if you believe it can. Measured on APFS
  2026-08-02: deleting the sort in `meshClosure`/`shaderFiles` fails 1 assertion,
  but deleting it in `materialFiles` fails **none** — that fixture's filenames
  happen to come back ordered. So `materialFiles`' determinism rests on reading
  the code, not on the test. Fixing it needs a fixture whose creation order is
  deliberately the reverse of its sort order.

## Tier evidence (`working`)
- `tests/package_closure_test.cpp` — 58 assertions across all four functions,
  hermetic, building real cooked meshes/scenes/shaders/materials through the same
  `saveMesh`/`saveScene`/`saveShader`/`saveMaterial` the cookers use.
- **Mutation-proved**, each original bug reinstated and caught: the filename
  prefix scan fails 6 assertions, the silent `continue` fails 1, file-counting
  instead of name-reading fails 2, removing either sort fails 1. For
  `materialFiles` (proved 2026-08-02): disabling duplicate-name detection fails
  1 assertion.
- A real `engine_build` of `fps_shooter` produces an unchanged 53.7 MB dist.

Reaching `hardened` wants a fuzz lane: cooked scenes and meshes are external
input arriving from a shared DDC, and the packager parses both.
