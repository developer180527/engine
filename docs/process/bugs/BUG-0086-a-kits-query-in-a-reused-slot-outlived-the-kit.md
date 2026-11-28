## BUG-0086 — A kit's query in a reused slot outlived the kit
- found:     2026-11-28
- status:    fixed
- class:     logic
- where:     src/capi/eng_world.cpp
- symptom:   a C kit's queries are destroyed when the kit stops (WO-094), but only on its first Play: a query created at a later Play, in a slot an earlier stop had freed, had no owner, so `engInternalDropOwned` never destroyed it. Each Stop and Play left the kit's queries of the previous Play alive (flecs queries matching its components) after the kit that made them had stopped.
- cause:     `engCreateQuery` set the query's owner on the path that appends a new slot, and not on the path that reuses a free one. Systems set it on both.
- pinned-by: tests/eng_world_scene_test.cpp
- lane:      unit
- proof:     the test creates a kit-owned query, drops the kit's work, and does it again: before the fix the second round dropped 0 queries and the handle stayed live; after, both rounds drop 1 and the handle is stale. Found by reading the code in a review, then confirmed by the test before fixing.
