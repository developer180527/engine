#pragma once
// ── clip_source — a standalone clip file, read and bound to a skeleton ────────
//
// ClipLibrary's source reader in dev builds (ClipLibrary::setSourceReader; the
// runtime installs it at boot). The file is imported like any other, through
// the import front end for its format, and its first clip is bound to the
// target skeleton by bone name (imp::buildOzzClip). The same reading as a mesh
// cook: the same front end, the same Mixamo name rule, the same keys.
//
// Before WO-015 ClipLibrary parsed the file with Assimp itself, with its own
// flags, so a clip read differently from the character it animated, and a glTF
// clip could not be read at all.
#include "animation/animation_clip.h"
#include "animation/skeleton.h"

#include <string>

namespace imp {

// The first clip in `sourcePath`, bound to `target` (which has its ozz data).
// Invalid, having logged why, when the file cannot be read or holds no clip.
::AnimClip readSourceClip(const std::string& sourcePath, const ::Skeleton& target);

}  // namespace imp
