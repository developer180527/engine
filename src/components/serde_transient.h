#pragma once
// ── SerdeTransient — "this component is RUNTIME STATE, never save it" ────────
// Tag a reflected component's TYPE with this and scene serialization skips it:
//
//     w.component<combat::DamageInbox>().add<SerdeTransient>();
//
// Why it exists: reflected components serialize by default, so a scene saved
// during play captured live gameplay state — pending damage, death tags,
// battle-worn values — and replayed it as authored data on the next load
// (pre-dead zombies, self-dying walls). Persistent vs transient is an
// ARCHITECTURAL property of a component, not a convention: one-frame messages
// (damage inboxes), engine-derived caches (interpolation snapshots) and
// lifecycle markers (Died) are transient; authored tuning (Health.max) is not.
// The inspector still shows transient components — you can watch them live —
// they just never reach disk.
struct SerdeTransient {};
