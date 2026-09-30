#pragma once
// ── mesh_backend — ImportedScene -> cooked mesh, written ONCE (WO-011) ────────
//
// The one back end of docs/plans/imported-scene.md. Every front end (cgltf,
// Assimp, and later USD or the FBX SDK) produces an imp::ImportedScene. This
// turns it into a MeshAsset plus sibling .ctex textures, so baking, tangents,
// materials, textures, the skeleton, clips and LODs are written once instead of
// once per parser. It includes no parser: it never sees aiScene or cgltf_data.
//
// ADDITIVE for now: MeshCooker::cook still dispatches to the per-parser paths.
// WO-012 (cgltf) and WO-013 (Assimp) switch each format over, and that is where
// the old and new output are compared on real files.
//
// ── What it decides, and where that differs from the old paths ─────────────
// The old paths disagreed with each other, so one rule had to be chosen for
// each; the choices, and why, are docs/plans/imported-scene.md §7.1:
//   * every texture, external or embedded, is decoded and cooked to a
//     content-deduplicated sibling .ctex: the one reference form a shipped
//     build (no registry) can resolve;
//   * tangents keep the source's handedness; missing ones are generated;
//   * a mirroring node transform (negative determinant) flips the winding and
//     the tangent handedness, so a mirrored instance is not inside-out;
//   * a mesh with no weights in a skinned scene is bound rigidly to the nearest
//     ancestor bone, not silently dropped.
#include <assetlib/cooker.h>

#include <string>
#include <vector>

#include "assets/import/imported_scene.h"

namespace meshcook {

struct BackendReport {
    std::vector<std::string> notes;      // what was cooked LESS, each logged once
};

// Refuses (success=false, a named reason, nothing written) a scene that fails
// imp::checkScene, carries a Dropped entry with effect Wrong, has more bones than
// a cooked vertex can index (256), or has no triangles (that is the clip
// cooker's input, WO-016: skipped=true).
assetlib::CookResult cookImportedScene(const imp::ImportedScene& scene,
                                       const assetlib::CookContext& ctx,
                                       BackendReport* report = nullptr);

}  // namespace meshcook
