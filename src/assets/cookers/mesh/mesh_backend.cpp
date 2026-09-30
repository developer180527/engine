// ── mesh_backend — see mesh_backend.h ────────────────────────────────────────
#include "assets/cookers/mesh/mesh_backend.h"
#include "assets/cookers/mesh/cook_common.h"
#include "assets/cookers/texture/texture_encode.h"   // BC7/BC5 + mips for .ctex
#include "assets/import/imported_scene_check.h"
#include "animation/ozz_bridge.h"                     // buildOzzSkeleton, finishOzzClip

#include <assetlib/mesh_asset.h>
#include <assetlib/texture_asset.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <stb_image.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace assetlib;

namespace meshcook {
namespace {

// ── Vertex layouts: the runtime's, byte for byte ────────────────────────────
constexpr uint32_t kStaticFlags  = VF_POSITION | VF_NORMAL | VF_TANGENT | VF_UV0;
constexpr uint32_t kSkinnedFlags = kStaticFlags | VF_JOINTS | VF_WEIGHTS;
constexpr uint32_t kMaxBones     = 256;          // joints are uint8 in a cooked vertex

struct StaticVertex {                            // render/vertex.h, 48 bytes
    float px, py, pz, nx, ny, nz, tx, ty, tz, tw, u, v;
};
struct SkinnedVertex {                           // render/skinned_vertex.h, 68 bytes
    float   px, py, pz, nx, ny, nz, tx, ty, tz, tw, u, v;
    uint8_t joints[4];
    float   weights[4];
};
static_assert(sizeof(StaticVertex)  == 48, "must match the runtime Vertex");
static_assert(sizeof(SkinnedVertex) == 68, "must match the runtime SkinnedVertex");

// ── Small linear algebra on the import format's types ───────────────────────
imp::Float3 xformPoint(const imp::Float4x4& t, imp::Float3 p) { return imp::transformPoint(t, p); }
imp::Float3 xformDir(const imp::Float4x4& t, imp::Float3 v) {
    return {t.m[0] * v.x + t.m[4] * v.y + t.m[8]  * v.z,
            t.m[1] * v.x + t.m[5] * v.y + t.m[9]  * v.z,
            t.m[2] * v.x + t.m[6] * v.y + t.m[10] * v.z};
}
imp::Float3 mul3(const float n[9], imp::Float3 v) {      // row-major 3x3 (normalMatrix's)
    return {n[0] * v.x + n[1] * v.y + n[2] * v.z,
            n[3] * v.x + n[4] * v.y + n[5] * v.z,
            n[6] * v.x + n[7] * v.y + n[8] * v.z};
}
imp::Float3 sub(imp::Float3 a, imp::Float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
imp::Float3 scale(imp::Float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
imp::Float3 cross(imp::Float3 a, imp::Float3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(imp::Float3 a, imp::Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
imp::Float3 normalize(imp::Float3 v, imp::Float3 fallback) {
    const float l = std::sqrt(dot(v, v));
    return l > 1e-8f ? scale(v, 1.0f / l) : fallback;
}
float det3(const imp::Float4x4& t) {
    const float* m = t.m;
    return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2])
         + m[8] * (m[1] * m[6] - m[5] * m[2]);
}

// Translation, rotation (the source's column-vector convention) and scale of a
// column-major matrix. A mirrored basis (negative determinant) puts the sign on
// the x scale, so the rotation stays proper.
void decompose(const imp::Float4x4& t, imp::Float3& pos, imp::Quat& rot, imp::Float3& scl) {
    const float* m = t.m;
    pos = {m[12], m[13], m[14]};
    imp::Float3 c0{m[0], m[1], m[2]}, c1{m[4], m[5], m[6]}, c2{m[8], m[9], m[10]};
    scl = {std::sqrt(dot(c0, c0)), std::sqrt(dot(c1, c1)), std::sqrt(dot(c2, c2))};
    if (det3(t) < 0) scl.x = -scl.x;
    c0 = scale(c0, scl.x != 0 ? 1.0f / scl.x : 0); c1 = scale(c1, scl.y != 0 ? 1.0f / scl.y : 0);
    c2 = scale(c2, scl.z != 0 ? 1.0f / scl.z : 0);
    // Rotation matrix R (columns c0 c1 c2) -> quaternion (Shepperd).
    const float r00 = c0.x, r11 = c1.y, r22 = c2.z, tr = r00 + r11 + r22;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2;
        rot = {(c1.z - c2.y) / s, (c2.x - c0.z) / s, (c0.y - c1.x) / s, 0.25f * s};
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2;
        rot = {0.25f * s, (c1.x + c0.y) / s, (c2.x + c0.z) / s, (c1.z - c2.y) / s};
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2;
        rot = {(c1.x + c0.y) / s, 0.25f * s, (c2.y + c1.z) / s, (c2.x - c0.z) / s};
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2;
        rot = {(c2.x + c0.z) / s, (c2.y + c1.z) / s, 0.25f * s, (c0.y - c1.x) / s};
    }
}

// ── Tangents ────────────────────────────────────────────────────────────────
// The source's, or generated when it has none: per-triangle UV derivatives
// (Lengyel), accumulated per vertex, orthogonalised against the normal. The
// handedness w makes cross(N, T) * w point along increasing v, the convention
// of the Assimp path's CalcTangentSpace. Without UVs there is nothing to derive
// a tangent from, and the old default (1, 0, 0, 1) is kept.
std::vector<imp::Float4> tangentsOf(const imp::Mesh& m) {
    if (!m.tangents.empty()) return m.tangents;
    std::vector<imp::Float4> out(m.positions.size(), imp::Float4{1, 0, 0, 1});
    if (m.uv0.empty()) return out;
    std::vector<imp::Float3> tan(m.positions.size()), bit(m.positions.size());
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const uint32_t a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
        const imp::Float3 e1 = sub(m.positions[b], m.positions[a]), e2 = sub(m.positions[c], m.positions[a]);
        const float du1 = m.uv0[b].x - m.uv0[a].x, dv1 = m.uv0[b].y - m.uv0[a].y;
        const float du2 = m.uv0[c].x - m.uv0[a].x, dv2 = m.uv0[c].y - m.uv0[a].y;
        const float d = du1 * dv2 - du2 * dv1;
        if (std::fabs(d) < 1e-12f) continue;              // degenerate UVs: no information
        const float r = 1.0f / d;
        const imp::Float3 t = scale(sub(scale(e1, dv2), scale(e2, dv1)), r);
        const imp::Float3 bb = scale(sub(scale(e2, du1), scale(e1, du2)), r);
        for (uint32_t v : {a, b, c}) {
            tan[v] = {tan[v].x + t.x, tan[v].y + t.y, tan[v].z + t.z};
            bit[v] = {bit[v].x + bb.x, bit[v].y + bb.y, bit[v].z + bb.z};
        }
    }
    for (size_t v = 0; v < out.size(); ++v) {
        const imp::Float3 n = m.normals[v];
        // Gram-Schmidt: the tangent with its normal component removed.
        imp::Float3 t = sub(tan[v], scale(n, dot(n, tan[v])));
        if (dot(t, t) < 1e-16f)                           // no UV gradient here: any perpendicular
            t = std::fabs(n.x) < 0.9f ? cross(n, {1, 0, 0}) : cross(n, {0, 1, 0});
        t = normalize(t, {1, 0, 0});
        const float w = dot(cross(n, t), bit[v]) < 0 ? -1.0f : 1.0f;
        out[v] = {t.x, t.y, t.z, w};
    }
    return out;
}

// ── Emission ────────────────────────────────────────────────────────────────
struct Emitter {
    bool                      skinned = false;
    std::vector<uint8_t>      vertexBytes;
    std::vector<uint32_t>     indices;
    std::vector<MeshSubmesh>  submeshes;
    uint32_t                  vertexCount = 0;
    float bMin[3] = { FLT_MAX,  FLT_MAX,  FLT_MAX};
    float bMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    template <class V> V* grow(size_t n) {
        const size_t at = vertexBytes.size();
        vertexBytes.resize(at + n * sizeof(V));
        return reinterpret_cast<V*>(vertexBytes.data() + at);
    }
    void bound(imp::Float3 p) {
        const float c[3] = {p.x, p.y, p.z};
        for (int i = 0; i < 3; ++i) { bMin[i] = std::min(bMin[i], c[i]); bMax[i] = std::max(bMax[i], c[i]); }
    }
    void emitIndices(const imp::Mesh& m, bool flipWinding) {
        for (const imp::Submesh& sm : m.submeshes) {
            MeshSubmesh sub{};                            // value-init: no garbage in the reserved uuid
            sub.indexOffset   = (uint32_t)indices.size();
            sub.indexCount    = sm.indexCount;
            sub.materialIndex = sm.material;
            for (uint32_t i = sm.firstIndex; i < sm.firstIndex + sm.indexCount; i += 3) {
                const uint32_t a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
                indices.push_back(vertexCount + a);
                // A mirroring transform reverses a triangle's winding in world
                // space; swapping two corners puts the front face back in front.
                indices.push_back(vertexCount + (flipWinding ? c : b));
                indices.push_back(vertexCount + (flipWinding ? b : c));
            }
            submeshes.push_back(sub);
        }
        vertexCount += (uint32_t)m.positions.size();
    }
};

// A mesh placed by `world`, written in world space (static cooks, and rigid
// meshes inside a skinned cook). `rigidBone` >= 0 writes skinned vertices bound
// wholly to that bone.
void emitBaked(Emitter& e, const imp::Mesh& m, const imp::Float4x4& world, int rigidBone) {
    float nm[9];
    normalMatrix(world.m, nm);
    const bool mirrored = det3(world) < 0;
    const std::vector<imp::Float4> tangents = tangentsOf(m);
    const bool generatedOrSource = !m.uv0.empty() || !m.tangents.empty();
    const size_t n = m.positions.size();
    auto fill = [&](auto* v, size_t i) {
        const imp::Float3 p  = xformPoint(world, m.positions[i]);
        const imp::Float3 nn = normalize(mul3(nm, m.normals[i]), {0, 1, 0});
        imp::Float3 t{tangents[i].x, tangents[i].y, tangents[i].z};
        float w = tangents[i].w;
        if (generatedOrSource) {                        // a real tangent follows the transform
            t = normalize(xformDir(world, t), {1, 0, 0});
            if (mirrored) w = -w;
        }
        v->px = p.x; v->py = p.y; v->pz = p.z;
        v->nx = nn.x; v->ny = nn.y; v->nz = nn.z;
        v->tx = t.x; v->ty = t.y; v->tz = t.z; v->tw = w;
        v->u = m.uv0.empty() ? 0.0f : m.uv0[i].x;
        v->v = m.uv0.empty() ? 0.0f : m.uv0[i].y;
        e.bound(p);
    };
    if (e.skinned) {
        SkinnedVertex* v = e.grow<SkinnedVertex>(n);
        for (size_t i = 0; i < n; ++i) {
            fill(&v[i], i);
            v[i].joints[0] = (uint8_t)std::max(rigidBone, 0);
            v[i].joints[1] = v[i].joints[2] = v[i].joints[3] = 0;
            v[i].weights[0] = 1.0f; v[i].weights[1] = v[i].weights[2] = v[i].weights[3] = 0.0f;
        }
    } else {
        StaticVertex* v = e.grow<StaticVertex>(n);
        for (size_t i = 0; i < n; ++i) fill(&v[i], i);
    }
    e.emitIndices(m, mirrored);
}

// A skinned mesh, in its own (bind) space: the skin matrices place it.
void emitSkinned(Emitter& e, const imp::Mesh& m) {
    const std::vector<imp::Float4> tangents = tangentsOf(m);
    SkinnedVertex* v = e.grow<SkinnedVertex>(m.positions.size());
    for (size_t i = 0; i < m.positions.size(); ++i) {
        const imp::Float3 p = m.positions[i];
        const imp::Float3 nn = normalize(m.normals[i], {0, 1, 0});
        v[i] = {p.x, p.y, p.z, nn.x, nn.y, nn.z,
                tangents[i].x, tangents[i].y, tangents[i].z, tangents[i].w,
                m.uv0.empty() ? 0.0f : m.uv0[i].x, m.uv0.empty() ? 0.0f : m.uv0[i].y, {}, {}};
        for (int k = 0; k < 4; ++k) v[i].joints[k] = (uint8_t)m.joints[i][k];
        v[i].weights[0] = m.weights[i].x; v[i].weights[1] = m.weights[i].y;
        v[i].weights[2] = m.weights[i].z; v[i].weights[3] = m.weights[i].w;
        e.bound(p);
    }
    e.emitIndices(m, false);
}

// ── Textures ────────────────────────────────────────────────────────────────
// ONE rule for every texture (plan §7.1): decode it, cook it to a sibling .ctex,
// deduplicated by content. A texture that cannot be read leaves its slot empty
// and says so, never a reference that resolves to nothing at run time.
std::string cookTexture(const imp::TextureRef& ref, bool isNormalMap, int slot,
                        const CookContext& ctx, SiblingDedup& dedup, std::vector<std::string>& notes) {
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = nullptr;
    std::string what;
    if (!ref.rgba.empty()) {                            // already decoded (Assimp's raw embedded texels)
        TextureAsset tex;
        if (!cook::encodeTexture(ref.rgba.data(), ref.width, ref.height, isNormalMap, tex)) {
            notes.push_back("texture pixels '" + ref.embeddedName + "' could not be encoded; the slot is left empty");
            return {};
        }
        return writeSiblingTexture(tex, ctx, slot, ref.width, ref.height, isNormalMap, "embedded", dedup);
    }
    if (!ref.embedded.empty()) {
        what = "embedded '" + ref.embeddedName + "'";
        px = stbi_load_from_memory(ref.embedded.data(), (int)ref.embedded.size(), &w, &h, &ch, 4);
    } else {
        std::filesystem::path p = ref.path;
        if (p.is_relative()) p = ctx.sourcePath.parent_path() / p;
        what = "'" + ref.path + "'";
        px = stbi_load(p.string().c_str(), &w, &h, &ch, 4);
    }
    if (!px) {
        notes.push_back("texture " + what + " could not be read; the slot is left empty");
        return {};
    }
    TextureAsset tex;
    const bool ok = cook::encodeTexture(px, (uint32_t)w, (uint32_t)h, isNormalMap, tex);
    stbi_image_free(px);
    if (!ok) {
        notes.push_back("texture " + what + " could not be encoded; the slot is left empty");
        return {};
    }
    return writeSiblingTexture(tex, ctx, slot, (uint32_t)w, (uint32_t)h, isNormalMap,
                               ref.embedded.empty() ? "external" : "embedded", dedup);
}

void emitMaterials(const imp::ImportedScene& s, MeshAsset& asset, const CookContext& ctx,
                   std::vector<std::string>& notes) {
    SiblingDedup dedup;                                // scoped to THIS asset's cook
    int slot = 0;
    for (const imp::Material& m : s.materials) {
        CookedMaterial cm{};
        cm.baseColorFactor[0] = m.baseColorFactor.x; cm.baseColorFactor[1] = m.baseColorFactor.y;
        cm.baseColorFactor[2] = m.baseColorFactor.z; cm.baseColorFactor[3] = m.baseColorFactor.w;
        cm.roughness = m.roughness;
        cm.metallic  = m.metallic;
        if (!m.baseColor.empty()) {
            const std::string fn = cookTexture(m.baseColor, false, slot++, ctx, dedup, notes);
            if (!fn.empty()) {
                std::snprintf(cm.baseColorPath, sizeof cm.baseColorPath, "%s", fn.c_str());
                cm.flags |= kMatFlag_HasBaseColor;
            }
        }
        if (!m.normal.empty()) {
            const std::string fn = cookTexture(m.normal, true, slot++, ctx, dedup, notes);
            if (!fn.empty()) {
                std::snprintf(cm.normalMapPath, sizeof cm.normalMapPath, "%s", fn.c_str());
                cm.flags |= kMatFlag_HasNormalMap;
            }
        }
        asset.materials.push_back(cm);
    }
    asset.header.materialCount = (uint32_t)asset.materials.size();
}

// ── Skeleton and clips ──────────────────────────────────────────────────────
// imp::Bone -> the engine's anim Bone. Matrices share one memory layout
// (translation in m[12..14]) and copy across. The bind ROTATION is stored
// conjugated: the engine's Bone keeps the conjugate so that its SQT recomposes
// to localBindMatrix under bx's matrix convention (assimp_skeleton_loader.h's
// decomposeAiMatrix explains it); restTransform() undoes it for ozz.
Skeleton toAnimSkeleton(const imp::Skeleton& in) {
    Skeleton out;
    out.bones.reserve(in.bones.size());
    for (const imp::Bone& b : in.bones) {
        Bone ab;
        ab.name = b.name;
        ab.parentIndex = b.parent;
        imp::Float3 p, s; imp::Quat q;
        decompose(b.bindLocal, p, q, s);
        ab.bindPosition = {p.x, p.y, p.z};
        ab.bindRotation = {-q.x, -q.y, -q.z, q.w};
        ab.bindScale    = {s.x, s.y, s.z};
        std::memcpy(ab.localBindMatrix,   b.bindLocal.m,   sizeof ab.localBindMatrix);
        std::memcpy(ab.inverseBindMatrix, b.inverseBind.m, sizeof ab.inverseBindMatrix);
        out.bones.push_back(ab);
    }
    out.buildBoneMap();
    return out;
}

bool emitSkeletonAndClips(const imp::ImportedScene& s, MeshAsset& asset, std::string& error) {
    Skeleton skel = toAnimSkeleton(*s.skeleton);
    if (!anim::buildOzzSkeleton(skel)) { error = "ozz skeleton build failed"; return false; }

    asset.bones.reserve(skel.bones.size());
    for (const Bone& b : skel.bones) {
        CookedBone cb{};
        std::snprintf(cb.name, sizeof cb.name, "%s", b.name.c_str());
        cb.parentIndex = b.parentIndex;
        cb.bindPosition[0] = b.bindPosition.x; cb.bindPosition[1] = b.bindPosition.y;
        cb.bindPosition[2] = b.bindPosition.z;
        cb.bindRotation[0] = b.bindRotation.x; cb.bindRotation[1] = b.bindRotation.y;
        cb.bindRotation[2] = b.bindRotation.z; cb.bindRotation[3] = b.bindRotation.w;
        cb.bindScale[0] = b.bindScale.x; cb.bindScale[1] = b.bindScale.y; cb.bindScale[2] = b.bindScale.z;
        std::memcpy(cb.inverseBindMatrix, b.inverseBindMatrix, 64);
        std::memcpy(cb.localBindMatrix,   b.localBindMatrix,   64);
        asset.bones.push_back(cb);
    }
    {
        ozz::io::MemoryStream ms;
        { ozz::io::OArchive a(&ms); a << *skel.ozz; }
        asset.skeletonBlob = drainOzzStream(ms);
    }

    for (const imp::Clip& c : s.clips) {
        ozz::animation::offline::RawAnimation raw;
        raw.duration = std::max(c.duration, 1e-4f);
        raw.tracks.resize((size_t)skel.ozz->num_joints());
        int mapped = 0;
        auto t = [&](float time) { return std::clamp(time, 0.0f, raw.duration); };
        for (const imp::Track& tr : c.tracks) {
            const int ours = skel.findBone(tr.bone);
            if (ours < 0) continue;                        // checkScene makes this unreachable
            auto& track = raw.tracks[(size_t)skel.ozzJointOf[ours]];
            for (const auto& k : tr.translation) track.translations.push_back({t(k.time), {k.value.x, k.value.y, k.value.z}});
            for (const auto& k : tr.rotation)    track.rotations.push_back({t(k.time), {k.value.x, k.value.y, k.value.z, k.value.w}});
            for (const auto& k : tr.scale)       track.scales.push_back({t(k.time), {k.value.x, k.value.y, k.value.z}});
            ++mapped;
        }
        AnimClip clip = anim::finishOzzClip(raw, skel, c.name, mapped, (int)c.tracks.size());
        if (!clip.valid()) { error = "clip '" + c.name + "' could not be built"; return false; }
        ozz::io::MemoryStream ms;
        { ozz::io::OArchive ar(&ms); ar << *clip.ozz; }
        CookedClipBlob blob;
        blob.name         = clip.name;
        blob.mappedTracks = clip.mappedTracks;
        blob.totalTracks  = clip.totalTracks;
        blob.blob         = drainOzzStream(ms);
        asset.clips.push_back(std::move(blob));
    }
    return true;
}

// The bone a rigid mesh on node `ni` follows: the nearest ancestor node (itself
// included) whose name is a bone's, else the root bone.
int rigidBoneFor(const imp::ImportedScene& s, size_t ni) {
    for (int32_t n = (int32_t)ni; n >= 0; n = s.nodes[(size_t)n].parent)
        for (size_t b = 0; b < s.skeleton->bones.size(); ++b)
            if (s.skeleton->bones[b].name == s.nodes[(size_t)n].name) return (int)b;
    return 0;
}

CookResult refuse(std::string why) { return {.success = false, .error = std::move(why)}; }

}  // namespace

CookResult cookImportedScene(const imp::ImportedScene& s, const CookContext& ctx, BackendReport* report) {
    std::vector<std::string> notes;
    const std::string label = ctx.sourcePath.filename().string();

    // ── Refusals, before anything is written ────────────────────────────────
    if (const auto v = imp::checkScene(s); !v.empty())
        return refuse("invalid ImportedScene (" + std::string(v[0].check) + ": " + v[0].detail + ")"
                      + (v.size() > 1 ? " and " + std::to_string(v.size() - 1) + " more" : ""));
    for (const imp::Dropped& d : s.dropped) {
        if (d.effect == imp::Dropped::Effect::Wrong)
            return refuse(std::string("cannot cook without the ") + imp::toString(d.kind) + ": " + d.what);
        notes.push_back(std::string("not cooked (") + imp::toString(d.kind) + "): " + d.what);
    }
    size_t triangles = 0;
    for (const auto& m : s.meshes) triangles += m.indices.size() / 3;
    if (triangles == 0)
        return {.success = false, .skipped = true,
                .error = "animation-only: clips are the clip cooker's input (WO-016)"};

    bool anySkinned = false;
    for (const auto& m : s.meshes) anySkinned |= m.skinned();
    if (anySkinned && s.skeleton->bones.size() > kMaxBones)
        return refuse(std::to_string(s.skeleton->bones.size()) + " bones: a cooked vertex indexes at most " +
                      std::to_string(kMaxBones));
    if (!anySkinned && !s.clips.empty())
        notes.push_back(std::to_string(s.clips.size()) + " clip(s) not cooked: no mesh is skinned");

    // ── Geometry ────────────────────────────────────────────────────────────
    Emitter e;
    e.skinned = anySkinned;
    if (anySkinned) {
        // Skinned meshes once each, in their own space; rigid meshes where their
        // nodes put them, each bound wholly to the bone it hangs from.
        for (const auto& m : s.meshes)
            if (m.skinned()) emitSkinned(e, m);
        for (size_t ni = 0; ni < s.nodes.size(); ++ni)
            for (uint32_t mi : s.nodes[ni].meshes)
                if (!s.meshes[mi].skinned()) {
                    const int bone = rigidBoneFor(s, ni);
                    emitBaked(e, s.meshes[mi], imp::worldOf(s.nodes, ni, &imp::Node::local), bone);
                    notes.push_back("mesh '" + s.meshes[mi].name + "' has no weights: bound rigidly to bone '" +
                                    s.skeleton->bones[(size_t)bone].name + "'");
                }
        // Bounds, skinned at rest. The vertices are in bind space, which is
        // not where the mesh draws: the palette (bone rest world × inverse
        // bind) moves them. CesiumMan's bind space is Z up, so bounds taken
        // from raw positions laid his box on its side, and the editor's spawn
        // (scale and ground offset from bounds) floated him 0.76 m up. Bound
        // what the renderer draws at rest, the same sum the contract suite's
        // "skinned at rest" check uses.
        std::vector<imp::Float4x4> rest(s.skeleton->bones.size());
        for (size_t b = 0; b < rest.size(); ++b)
            rest[b] = imp::mul(imp::worldOf(s.skeleton->bones, b, &imp::Bone::bindLocal), s.skeleton->bones[b].inverseBind);
        for (int k = 0; k < 3; ++k) { e.bMin[k] = FLT_MAX; e.bMax[k] = -FLT_MAX; }
        const SkinnedVertex* sv = reinterpret_cast<const SkinnedVertex*>(e.vertexBytes.data());
        for (uint32_t vi = 0; vi < e.vertexBytes.size() / sizeof(SkinnedVertex); ++vi) {
            imp::Float3 r{0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                if (sv[vi].weights[k] == 0.0f) continue;
                const imp::Float3 q = imp::transformPoint(rest[sv[vi].joints[k]], {sv[vi].px, sv[vi].py, sv[vi].pz});
                r = {r.x + sv[vi].weights[k] * q.x, r.y + sv[vi].weights[k] * q.y, r.z + sv[vi].weights[k] * q.z};
            }
            e.bound(r);
        }
    } else {
        for (size_t ni = 0; ni < s.nodes.size(); ++ni)
            for (uint32_t mi : s.nodes[ni].meshes)
                emitBaked(e, s.meshes[mi], imp::worldOf(s.nodes, ni, &imp::Node::local), -1);
    }

    MeshAsset asset;
    asset.header.magic        = 0x4D455348;
    asset.header.version      = anySkinned ? 6 : 2;   // v6: skeleton and clip digests
    asset.header.vertexFlags  = anySkinned ? kSkinnedFlags : kStaticFlags;
    asset.header.vertexStride = anySkinned ? sizeof(SkinnedVertex) : sizeof(StaticVertex);
    asset.header.boneCount    = anySkinned ? (uint32_t)s.skeleton->bones.size() : 0;
    std::memcpy(asset.header.uuid, ctx.uuid.bytes.data(), 16);
    asset.header.vertexCount  = e.vertexCount;
    asset.header.indexCount   = (uint32_t)e.indices.size();
    asset.header.submeshCount = (uint32_t)e.submeshes.size();
    for (int i = 0; i < 3; ++i) { asset.header.boundsMin[i] = e.bMin[i]; asset.header.boundsMax[i] = e.bMax[i]; }
    asset.vertexData = std::move(e.vertexBytes);
    asset.submeshes  = std::move(e.submeshes);

    const bool use16 = e.vertexCount <= 0xFFFFu;
    asset.header.indexStride = use16 ? 2 : 4;
    asset.indexData.resize(e.indices.size() * asset.header.indexStride);
    if (use16) {
        auto* d = reinterpret_cast<uint16_t*>(asset.indexData.data());
        for (size_t i = 0; i < e.indices.size(); ++i) d[i] = (uint16_t)e.indices[i];
    } else {
        std::memcpy(asset.indexData.data(), e.indices.data(), asset.indexData.size());
    }

    emitMaterials(s, asset, ctx, notes);

    if (anySkinned) {
        std::string error;
        if (!emitSkeletonAndClips(s, asset, error)) return refuse(error);
    }

    appendLodLevels(asset);                            // static only; skips small meshes
    if (!saveMesh(asset, ctx.outputPath)) return refuse("saveMesh failed");

    for (const auto& n : notes) std::printf("[MeshCooker] %s — %s\n", label.c_str(), n.c_str());
    std::printf("[MeshCooker] %s -> %s verts=%u idx=%u submeshes=%u mats=%u bones=%u clips=%zu\n",
                label.c_str(), anySkinned ? "SKINNED" : "static", asset.header.vertexCount,
                asset.header.indexCount, asset.header.submeshCount, asset.header.materialCount,
                asset.header.boneCount, asset.clips.size());
    if (report) report->notes = std::move(notes);
    return {.success = true};
}

}  // namespace meshcook
