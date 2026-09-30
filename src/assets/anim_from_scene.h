#pragma once
// ── anim_from_scene — ImportedScene skeletons and clips -> the engine's ───────
//
// The one conversion from the import format to animation's runtime types,
// shared by everything that builds animation from a source file: the mesh cook
// back end (skeleton + embedded clips) and the standalone-clip reader
// (ClipLibrary's source path). Before WO-015 the animation module took
// Assimp's types directly (buildOzzClip(aiAnimation*), extractSkeleton(aiScene*)),
// so a clip could only come from a file Assimp read, through code no other
// format shared.
//
// Lives in assets, not animation: it reads imp:: types, and animation must not
// depend on the import stack (assets already depends on animation). Not in
// assets/import/ either: that directory is the format, which depends on the
// standard library alone (audit LAYER-05).
#include "animation/animation_clip.h"
#include "animation/skeleton.h"
#include "assets/import/imported_scene.h"

namespace imp {

// imp::Bone -> the engine's Bone. Matrices share one memory layout
// (translation in m[12..14]) and copy across. The bind ROTATION is stored
// conjugated: the engine's Bone keeps the conjugate so that its SQT recomposes
// to localBindMatrix under bx's convention; anim::restTransform() undoes it for
// ozz. The returned skeleton has no ozz data yet: call anim::buildOzzSkeleton.
::Skeleton toAnimSkeleton(const Skeleton& in);

// One clip as a compressed ozz Animation, bound BY BONE NAME to `bound`, which
// must have its ozz data. Tracks naming no bone of `bound` are skipped and
// counted (AnimClip::mappedTracks of totalTracks); joints the clip does not
// animate hold their rest pose. Keys are in the clip's own bone frames, as
// ImportedScene keeps them; rotations are not conjugated (ozz uses the source
// convention). An invalid AnimClip on failure, already logged.
::AnimClip buildOzzClip(const Clip& clip, const ::Skeleton& bound);

}  // namespace imp
