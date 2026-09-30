#pragma once
// ── cook_common — what every mesh cook path shares ───────────────────────────
//
// Moved out of mesh_cooker.cpp (WO-011) so the per-parser cook paths that still
// exist (Assimp static/skinned, cgltf) and the ImportedScene back end
// (mesh_backend.cpp) use ONE copy of each. Behaviour is unchanged by the move;
// the only difference is that the sibling-texture dedup table is passed in,
// not reached as a hidden thread_local.
#include <assetlib/cooker.h>
#include <assetlib/mesh_asset.h>
#include <assetlib/texture_asset.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ozz::io { class MemoryStream; }

namespace meshcook {

// LOD chain for a cooked mesh: decimated levels, each kept only if it is
// meaningfully cheaper than its parent. Skinned meshes are skipped (R18).
void appendLodLevels(assetlib::MeshAsset& asset);

// Content -> sibling filename, scoped to ONE asset's cook (sibling names are
// qualified by that mesh's uuid stem, so an entry must never outlive the cook).
using SiblingDedup = std::unordered_map<std::string, std::string>;

// Write `tex` as "<output stem>_t<slot>.ctex" beside the cooked mesh, or return
// the name of an identical sibling already written in this cook. Returns the
// basename a CookedMaterial stores, or "" on failure.
std::string writeSiblingTexture(const assetlib::TextureAsset& tex,
                                const assetlib::CookContext& ctx, int slot,
                                uint32_t w, uint32_t h, bool isNormalMap,
                                const char* origin, SiblingDedup& dedup);

// Inverse-transpose of the upper 3x3 of a column-major 4x4 (translation in
// m[12..14]), with a SCALE-INVARIANT singularity guard: |det| is compared with
// the product of the row norms, so a mesh scaled by 0.0001 (det 1e-12) is not
// mistaken for a degenerate one. Identity when genuinely singular.
void normalMatrix(const float m[16], float n[9]);

// Drain an ozz output archive into bytes.
std::vector<uint8_t> drainOzzStream(ozz::io::MemoryStream& ms);

}  // namespace meshcook
