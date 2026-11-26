#pragma once
// ── Components this engine cannot read, kept (WO-071, DR-0034) ──────────────
//
// A scene can hold a component this engine does not know: one from a provider
// that is not in this build, one from a newer engine, or one saved at a revision
// no migration reaches. Dropping it would destroy the author's work the moment
// the scene is saved, so it is kept here, as the JSON it was loaded from, and
// written back unchanged by the next save.
//
// Keyed by the component's key in the entity's object. Plain data with no flecs
// meta, so the reflected path never sees it, and it survives the in-memory
// snapshot (Play, undo) the same way it survives a save.
#include <map>
#include <string>

struct UnknownComponents {
    std::map<std::string, std::string> blobs;   // component key -> JSON, as loaded
};
