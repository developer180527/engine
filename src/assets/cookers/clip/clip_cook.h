#pragma once
// ── clip_cook — an animation-only source, cooked to a clip (WO-016) ────────────
//
// The mesh cooker imports every model file; when the scene it gets has clips
// and no triangles (a Mixamo clip FBX, a glTF of animations alone), this cooks
// it instead of the mesh back end: the first clip's keys by bone name, as an
// anim::CookedClip (animation/cooked_clip.h), at the pipeline's output path. It
// is skeleton-independent; ClipLibrary binds it to a character at load.
//
// Before WO-016 such a file was "skipped: the clip cooker's input" and cooked
// only when the editor first bound it, so a shipped build had exactly the
// clips someone had played.
#include <assetlib/cooker.h>

#include "assets/import/imported_scene.h"

namespace clipcook {

// Refuses an invalid scene or one with a Wrong loss, as the mesh back end does.
// v1 cooks ONE clip per file (ClipLibrary binds the first); any others are
// reported, not silently dropped.
assetlib::CookResult cookClip(const imp::ImportedScene& scene, const assetlib::CookContext& ctx);

}  // namespace clipcook
