#pragma once
// ── EditorAddable ────────────────────────────────────────────────────────────
// Tag placed on a COMPONENT TYPE's entity (not on game entities) to opt it into
// the editor's "+ Add Component" menu. Meta-registered struct components show
// up in the Inspector automatically once present on an entity; this tag only
// controls which ones the editor offers to ADD:
//
//   world.component<Health>()
//        .member<float>("current")
//        .member<float>("max")
//        .add<EditorAddable>();      // <- appears in + Add Component
//
// Kits tag their authorable components; internal bookkeeping types don't.
struct EditorAddable {};
