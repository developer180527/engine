---
status: reference
---
# Kit-ABI component revisions: what changed in each, and what to do

> **For kit authors.** When the engine refuses your kit with *"Built against an
> older meaning of X"*, X is listed here. Rebuilding is enough to load again;
> what you need to check before shipping is in the section for X.

Every component a kit shares with the engine (the list in
`include/engine/game_module.h`, `ENGINE_ABI_COMPONENTS`) declares
`kAbiRevision`. The loader already refused a kit whose component *layout*
changed (size or alignment). A revision covers what the layout cannot see:
the same bytes coming to mean something different (WO-051). A bump makes every
kit built before it fail to load, by name, instead of loading and quietly
doing the wrong thing.

Each component starts at revision 0. Only bumps are listed.

## CollisionEvents

### Revision 1 (WO-048, 2026-09-30)

| | revision 0 | revision 1 |
|---|---|---|
| when a body has the component | only on a tick with a contact event | from its first contact, **for good** |
| a quiet tick | the component is removed | the component stays, `entered` and `exited` are **empty** |

**What breaks.** A system that treats "has `CollisionEvents`" as "something
happened this tick" now runs every tick for every body that has ever touched
anything:

```cpp
// revision 0 thinking: fires every tick under revision 1
world.each([](flecs::entity e, const CollisionEvents&) { playImpactSound(e); });
```

**What to do.** Test the lists, not the component:

```cpp
world.each([](flecs::entity e, const CollisionEvents& ce) {
    if (ce.hasEnter()) playImpactSound(e);
});
```

A query for bodies with the component no longer shrinks on quiet ticks, so a
"bodies in contact" count must count non-empty lists instead.

## Bumping a revision (engine developers)

1. Change the component, and raise its `kAbiRevision` by one.
2. Add a section here: the before/after table, what breaks, what to do.
3. Record the header: `python3 scripts/kit_abi_headers.py --record <header> --as revision-bumped`.
   `kit_abi_headers` (ctest) fails on any edit to a kit-ABI header until it is
   recorded, and `--as comment-only` is accepted only when the code, with
   comments removed, is unchanged.
