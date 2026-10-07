---
status: plan
id: WO-060
title: ozz stays in the animation .cpp files; engine headers show engine types
program: assets
priority: P3
size: S
state: todo
depends: []
touches:
  - src/animation/cooked_clip.h
  - src/animation/cooked_skin.h
  - src/animation/ozz_bridge.h
  - src/systems/animator_system.h
source: platform audit 2026-10-07
---
## Why
ozz types are in `cooked_clip.h`, `cooked_skin.h`, `ozz_bridge.h` and
`animator_system.h`, so anything that includes the animator system sees ozz.
The engine already has its own pose and skeleton types. Swapping or upgrading
the animation runtime should be a change to `src/animation/*.cpp`, not to every
system that touches animation.

## Done when
- [ ] no header outside `src/animation/` includes `ozz/`, and inside it only a private (not installed) header does
- [ ] an `engine_audit` rule enforces it, mutation-checked
- [ ] the installed SDK headers contain no `ozz/` include, checked by the SDK-only job
- [ ] no measurable cost on the animator benchmark (the indirection is per clip, not per bone)

## Contract
Nothing: engine-type API unchanged for kits.
