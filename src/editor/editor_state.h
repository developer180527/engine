#pragma once

#include <flecs.h>

// Editor state.
//
// Holds runtime state that's specific to the editor UI: which entity is
// selected, which panels are visible, undo stack (later), etc. Deliberately
// kept separate from the World — the World holds engine state that runs in
// both editor and runtime; EditorState only exists in the editor.
//
// Why this matters: when the engine eventually has a "play mode" that runs
// the same simulation as a shipped game, the EditorState doesn't get touched.
// Selection, panel visibility, and similar UI concerns shouldn't bleed into
// gameplay state.
//
// flecs::entity is just an ID + a pointer to the world; default construction
// gives a "null" entity (id=0). entity::is_alive() checks if it's still valid
// (not destroyed since we got the handle).
struct EditorState {
    flecs::entity selected;
};
