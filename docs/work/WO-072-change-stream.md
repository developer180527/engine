---
status: plan
id: WO-072
title: One change stream per registered component — free without subscribers, merged per read
program: renderer
priority: P1
rank: 3
size: M
state: todo
depends: [WO-071]
contracts: [render-scene]
touches:
  - src/render/world/render_scene.h
source: docs/plans/engine-architecture.md §5; engine-api.md §5 (marked writes, merged changes); roadmap §2.8
---
## Why
Every provider that keeps its own copy of the world (the renderer's GPU scene, physics bodies, audio voices, replication) needs to know what changed since it last looked. WO-019 built this for the renderer alone, as a rebuild-and-diff table; every provider moving onto the C API needs it general.

## Done when
- [ ] subscribe, read and unsubscribe per registered component type (the draft's `engSubscribeChanges`, `engReadChanges`), in core
- [ ] **free with no subscriber**: an empty game records nothing, checked by `empty_game_budget_test` (DR-0011)
- [ ] merged per read: an entity appears once between two reads (changed many times is one CHANGED, added then removed is nothing), and an id is not reused while a stream could still report it
- [ ] declared writes: a writing system marks a whole column, or with `ENG_ACCESS_WRITE_MARKED` only the rows it reports; a test shows sleeping physics bodies do not appear when only awake ones are marked
- [ ] the retained render scene (WO-019) reads it instead of its own capture, and its rebuild-and-diff check still passes on the fuzz scene
- [ ] mutations: a write that does not mark, and a dropped REMOVED, are each caught by a test

## Contract
Nothing: `render-scene`'s rebuild-and-diff mode stays as the oracle; a provider that does not subscribe sees no change.
