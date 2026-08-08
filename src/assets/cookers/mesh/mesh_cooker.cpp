#include "assets/cookers/mesh/mesh_cooker.h"
#include "assets/cookers/mesh/decimate.h"
#include "assets/cookers/texture/texture_encode.h"   // BC7/BC5 + mips for .ctex
#include <assetlib/ddc.h>                    // blake3Bytes — sibling dedup
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <assimp/matrix4x4.h>
#include <assimp/matrix3x3.h>
#include <cgltf.h>   // glTF/GLB cook path (Assimp is built without glTF)

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <semaphore>
#include <thread>

// MeshCooker::cook is a pure function: it owns its Assimp importer and writes a
// single unique output file, sharing no mutable state. That — and nothing more
// exotic — is what lets the cook pipeline run many cooks concurrently.

using namespace assetlib;

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>

#include "animation/assimp_skeleton_loader.h"   // extractSkeleton/BoneWeights
#include "animation/ozz_bridge.h"               // buildOzzSkeleton/Clip
#include "animation/animation_clip.h"
#include <assetlib/texture_asset.h>
#include <stb_image.h>

static constexpr unsigned kImportFlags =
    aiProcess_Triangulate | aiProcess_GenSmoothNormals |
    aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices |
    aiProcess_ImproveCacheLocality | aiProcess_FlipUVs |
    aiProcess_SortByPType;   // with SBP_REMOVE below -> triangle-only meshes

static constexpr uint32_t kCookFlags = VF_POSITION | VF_NORMAL | VF_TANGENT | VF_UV0;

// Layout for kCookFlags: pos(3) + normal(3) + tangent(4, w=handedness) + uv(2) = 48 bytes.
struct CookVertex {
    float px, py, pz;
    float nx, ny, nz;
    float tx, ty, tz, tw;
    float u, v;
};

// Buffers + running cursors threaded through the node-tree emit walk.
struct EmitState {
    CookVertex* verts     = nullptr;
    uint8_t*    idxBytes  = nullptr;
    uint32_t    vWrite    = 0;     // vertex write cursor
    uint32_t    iByteOff  = 0;     // index byte cursor
    uint32_t    vBase     = 0;     // running base for global index rebasing
    bool        use16     = true;
    uint32_t    idxStride = 2;
    float       bMin[3]   = { FLT_MAX,  FLT_MAX,  FLT_MAX};
    float       bMax[3]   = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    std::vector<MeshSubmesh>* submeshes = nullptr;
};

// Count (node, mesh) occurrences — a mesh instanced under several nodes is
// emitted once per node, so it must be sized that way too.
static void countNode(const aiScene* s, const aiNode* node,
                      uint32_t& verts, uint32_t& indices) {
    for (unsigned i = 0; i < node->mNumMeshes; ++i) {
        const aiMesh* m = s->mMeshes[node->mMeshes[i]];
        if (m->HasPositions()) { verts += m->mNumVertices; indices += m->mNumFaces * 3; }
    }
    for (unsigned c = 0; c < node->mNumChildren; ++c)
        countNode(s, node->mChildren[c], verts, indices);
}

aiMatrix3x3 cookNormalMatrix(const aiMatrix4x4& world) {
    aiMatrix3x3 nm(world);
    // Scale-INVARIANT singularity test (cooker audit "Determinant Trap"):
    // a bare |det| > 1e-12 check collapses for small uniform scales —
    // 0.0001^3 IS 1e-12, so perfectly valid heavily-scaled-down assets were
    // flagged degenerate and had their normal matrix replaced with identity:
    // normals stopped following node rotation, breaking shading at runtime.
    // Normalizing by the row-norm product makes uniform scale s give a
    // ratio of ~1 at ANY s; only genuinely flattened/collinear bases (true
    // singularity) approach 0. Normals are re-normalized after transform,
    // so the inverse's large magnitudes at tiny scales are harmless.
    const float r0 = std::sqrt(nm.a1*nm.a1 + nm.a2*nm.a2 + nm.a3*nm.a3);
    const float r1 = std::sqrt(nm.b1*nm.b1 + nm.b2*nm.b2 + nm.b3*nm.b3);
    const float r2 = std::sqrt(nm.c1*nm.c1 + nm.c2*nm.c2 + nm.c3*nm.c3);
    const float det     = nm.Determinant();
    const float normPrd = r0 * r1 * r2;
    if (normPrd > 0.0f && std::fabs(det) > 1e-6f * normPrd) {
        nm.Inverse(); nm.Transpose();
        return nm;
    }
    return aiMatrix3x3();   // genuinely singular — identity fallback
}

static void emitMesh(const aiMesh* mesh, const aiMatrix4x4& world, EmitState& st) {
    if (!mesh->HasPositions()) return;

    const aiMatrix3x3 w3(world);                 // linear part — directions
    const aiMatrix3x3 nm = cookNormalMatrix(world); // normals — see helper

    const bool hasN  = mesh->HasNormals();
    const bool hasT  = mesh->HasTangentsAndBitangents();
    const bool hasUV = mesh->HasTextureCoords(0);

    for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
        CookVertex& vtx = st.verts[st.vWrite++];

        aiVector3D P = world * mesh->mVertices[v];        // point transform (incl. translation)
        vtx.px = P.x; vtx.py = P.y; vtx.pz = P.z;
        st.bMin[0]=std::min(st.bMin[0],P.x); st.bMin[1]=std::min(st.bMin[1],P.y); st.bMin[2]=std::min(st.bMin[2],P.z);
        st.bMax[0]=std::max(st.bMax[0],P.x); st.bMax[1]=std::max(st.bMax[1],P.y); st.bMax[2]=std::max(st.bMax[2],P.z);

        aiVector3D N = hasN ? (nm * mesh->mNormals[v]) : aiVector3D(0, 1, 0);
        N.Normalize();
        vtx.nx = N.x; vtx.ny = N.y; vtx.nz = N.z;

        if (hasT) {
            aiVector3D T = w3 * mesh->mTangents[v];
            aiVector3D B = w3 * mesh->mBitangents[v];
            aiVector3D c(N.y*T.z - N.z*T.y, N.z*T.x - N.x*T.z, N.x*T.y - N.y*T.x);
            float sign = (c.x*B.x + c.y*B.y + c.z*B.z) < 0.0f ? -1.0f : 1.0f; // world-space handedness
            T.Normalize();
            vtx.tx = T.x; vtx.ty = T.y; vtx.tz = T.z; vtx.tw = sign;
        } else {
            vtx.tx = 1.0f; vtx.ty = 0.0f; vtx.tz = 0.0f; vtx.tw = 1.0f;
        }

        if (hasUV) { const auto& uv = mesh->mTextureCoords[0][v]; vtx.u = uv.x; vtx.v = uv.y; }
        else       { vtx.u = 0.0f; vtx.v = 0.0f; }
    }

    MeshSubmesh sub{};                               // value-init: no garbage in uuid/pad
    sub.indexOffset   = st.iByteOff / st.idxStride;
    sub.indexCount    = mesh->mNumFaces * 3;
    sub.materialIndex = mesh->mMaterialIndex;         // -> MeshAsset::materials[idx]

    if (st.use16) {
        auto* d = reinterpret_cast<uint16_t*>(&st.idxBytes[st.iByteOff]); uint32_t o = 0;
        for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            d[o++] = (uint16_t)(face.mIndices[0] + st.vBase);
            d[o++] = (uint16_t)(face.mIndices[1] + st.vBase);
            d[o++] = (uint16_t)(face.mIndices[2] + st.vBase);
        }
        st.iByteOff += sub.indexCount * 2;
    } else {
        auto* d = reinterpret_cast<uint32_t*>(&st.idxBytes[st.iByteOff]); uint32_t o = 0;
        for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            d[o++] = face.mIndices[0] + st.vBase;
            d[o++] = face.mIndices[1] + st.vBase;
            d[o++] = face.mIndices[2] + st.vBase;
        }
        st.iByteOff += sub.indexCount * 4;
    }

    st.vBase += mesh->mNumVertices;
    st.submeshes->push_back(sub);
}

static void emitNode(const aiScene* s, const aiNode* node,
                     const aiMatrix4x4& parentWorld, EmitState& st) {
    const aiMatrix4x4 world = parentWorld * node->mTransformation;
    for (unsigned i = 0; i < node->mNumMeshes; ++i)
        emitMesh(s->mMeshes[node->mMeshes[i]], world, st);
    for (unsigned c = 0; c < node->mNumChildren; ++c)
        emitNode(s, node->mChildren[c], world, st);
}

// Mirror of the runtime SkinnedVertex (render/skinned_vertex.h): 68 bytes.
// Layout for kSkinnedFlags: pos(3)+normal(3)+tangent(4)+uv(2)+joints(u8x4)+weights(4).
static constexpr uint32_t kSkinnedFlags = kCookFlags | VF_JOINTS | VF_WEIGHTS;
struct CookSkinnedVertex {
    float   px, py, pz;
    float   nx, ny, nz;
    float   tx, ty, tz, tw;
    float   u, v;
    uint8_t joints[4];
    float   weights[4];
};
static_assert(sizeof(CookSkinnedVertex) == 68, "must match runtime SkinnedVertex");

// Drain an ozz output archive into a byte vector.
static std::vector<uint8_t> drainOzzStream(ozz::io::MemoryStream& ms) {
    const int size = ms.Tell();
    std::vector<uint8_t> out((size_t)size);
    ms.Seek(0, ozz::io::Stream::kSet);
    ms.Read(out.data(), (size_t)size);
    return out;
}

// ── Sibling texture writing, with CONTENT DEDUP ─────────────────────────────
// Materials routinely share images: the fps_shooter pistol has several
// materials naming the same base-colour and normal maps, and the cooker used to
// encode and write each one to its OWN slot file. Measured cost: t0 and t2 were
// byte-identical (10.7 MB each) and t1 and t3 were byte-identical (21.3 MB
// each) — 32 MB duplicated on disk, in the shipped dist, AND in VRAM, for a
// three-mesh scene against a 128 MB target budget.
//
// Note the runtime cache CANNOT fix this: the duplicates have different
// filenames, so path identity does not see them. Content identity does, and the
// cheapest place to apply it is here — one write instead of two, which shrinks
// the cook, the DDC, the dist and the GPU upload at once.
//
// Keyed by the ENCODED bytes (post BC compression + mips), so two source images
// that compress identically also collapse.

// ── LOD levels ──────────────────────────────────────────────────────────────
// R20 shipped LOD selection that bought nothing measurable, because nothing
// could produce a cheaper mesh. This is that missing half: each level is
// decimated toward a triangle ratio and only KEPT if it is meaningfully
// cheaper than its parent — a level that is not cheaper costs memory and a
// swap for no benefit, which is exactly the state that was measured.
static void appendLodLevels(assetlib::MeshAsset& asset) {
    // Ratios, not grid resolutions: a fixed grid reduces a dense mesh by 96%
    // and a low-poly prop by 0%, so it cannot define a level across mixed
    // content. See decimate.h for the measurements.
    static constexpr float kLevelRatios[] = { 0.40f, 0.15f, 0.05f };

    // Nothing to gain below this: the per-level vertex/index buffers and the
    // swap cost more than the triangles saved.
    static constexpr uint32_t kMinTrianglesForLod = 2000;

    asset.lods.clear();
    // Skinned meshes are excluded. Clustering carries whole vertices, so joints
    // and weights would survive — but the renderer does not expand skinned items
    // for LOD (R18), so a level would never be selected. Revisit together.
    if (asset.header.boneCount > 0) return;
    if (asset.header.indexCount / 3 < kMinTrianglesForLod) return;
    if (asset.header.indexStride != 2 && asset.header.indexStride != 4) return;

    // MOST PROPS USE 16-BIT INDICES — the first real content this ran on was
    // skipped entirely by a 32-bit-only path. Widen here rather than in the
    // decimator, which stays single-format: expand to 32-bit, decimate, and
    // narrow again below if the level still fits.
    std::vector<uint32_t> idx32;
    const uint32_t* indices = nullptr;
    if (asset.header.indexStride == 2) {
        const auto* src = reinterpret_cast<const uint16_t*>(asset.indexData.data());
        idx32.assign(src, src + asset.header.indexCount);
        indices = idx32.data();
    } else {
        indices = reinterpret_cast<const uint32_t*>(asset.indexData.data());
    }

    meshcook::DecimateInput in;
    in.vertices    = asset.vertexData.data();
    in.vertexCount = asset.header.vertexCount;
    in.stride      = asset.header.vertexStride;
    in.posOffset   = assetlib::vertexAttributeOffset(asset.header.vertexFlags,
                                                     assetlib::VF_POSITION);
    in.indices     = indices;
    in.indexCount  = asset.header.indexCount;

    uint32_t parentTris = asset.header.indexCount / 3;
    for (float ratio : kLevelRatios) {
        const auto r = meshcook::decimateToRatio(in, ratio);
        if (!r.ok || r.triangles == 0) break;
        // Each level must beat its PARENT, not the original: once the search
        // stops making progress, further levels are pure cost.
        if ((float)r.triangles > meshcook::kMinReductionRatio * (float)parentTris)
            break;

        assetlib::MeshAsset::LodLevel lvl;
        lvl.vertexCount = r.vertexCount(in.stride);
        lvl.indexCount  = (uint32_t)r.indices.size();
        lvl.vertexData  = r.vertices;
        // Levels inherit the parent's index width. A level always has FEWER
        // vertices than its parent, so a 16-bit parent's level always fits —
        // but it is checked rather than assumed.
        if (asset.header.indexStride == 2 && lvl.vertexCount <= 0xFFFFu) {
            lvl.indexData.resize(r.indices.size() * 2);
            auto* dst = reinterpret_cast<uint16_t*>(lvl.indexData.data());
            for (size_t k = 0; k < r.indices.size(); ++k)
                dst[k] = (uint16_t)r.indices[k];
        } else if (asset.header.indexStride == 2) {
            break;      // cannot narrow: stop the chain rather than mis-encode
        } else {
            lvl.indexData.resize(r.indices.size() * 4);
            std::memcpy(lvl.indexData.data(), r.indices.data(), lvl.indexData.size());
        }
        asset.lods.push_back(std::move(lvl));
        parentTris = r.triangles;
    }
    if (!asset.lods.empty()) {
        asset.header.version = 4;
        std::printf("[MeshCooker]   lods: %u tris ->", asset.header.indexCount / 3);
        for (const auto& l : asset.lods) std::printf(" %u", l.indexCount / 3);
        std::printf("\n");
    }
}


namespace {
// Per-cook, per-thread: cleared at the start of every MeshCooker::cook(). It
// MUST NOT persist across assets — sibling names are qualified by the mesh's
// own uuid stem, so a stale entry would hand out a filename belonging to a
// different mesh. (Out-of-process workers are one asset per process anyway;
// this keeps the in-process fallback correct too.)
thread_local std::unordered_map<std::string, std::string> g_siblingByContent;

std::string writeSiblingTexture(const assetlib::TextureAsset& tex,
                                const CookContext& ctx, int slot,
                                uint32_t w, uint32_t h, bool isNormalMap,
                                const char* origin) {
    // Hash the payload plus the header fields that change interpretation, so
    // two textures with identical blocks but different dimensions/format can
    // never collide.
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "%u|%u|%u|%u|", tex.header.width,
                  tex.header.height, tex.header.format, tex.header.mipCount);
    std::string blob(hdr);
    blob.append(reinterpret_cast<const char*>(tex.pixels.data()),
                tex.pixels.size());
    const std::string key = assetlib::blake3Bytes(blob.data(), blob.size());

    if (auto it = g_siblingByContent.find(key); it != g_siblingByContent.end()) {
        std::printf("[MeshCooker] %s texture -> %s (DEDUP: identical to an "
                    "earlier slot, %.1f MB saved)\n",
                    origin, it->second.c_str(),
                    (double)tex.pixels.size() / (1024.0 * 1024.0));
        return it->second;          // reference the existing sibling
    }

    char name[64];
    std::snprintf(name, sizeof(name), "%s_t%d.ctex",
                  ctx.outputPath.stem().string().c_str(), slot);
    const auto outPath = ctx.outputPath.parent_path() / name;
    if (!assetlib::saveTexture(tex, outPath)) return {};
    if (ctx.addOutput) ctx.addOutput(outPath);   // travels with the DDC record

    g_siblingByContent.emplace(key, name);
    std::printf("[MeshCooker] %s texture -> %s (%ux%u %s, %u mips, %.1f MB)\n",
                origin, name, w, h, isNormalMap ? "BC5" : "BC7",
                tex.header.mipCount,
                (double)tex.pixels.size() / (1024.0 * 1024.0));
    return name;
}
} // namespace

// ── Materials (shared by static + skinned cooks) ────────────────────────────
// Embedded FBX textures have no on-disk source for the TextureCooker to see;
// they are decoded HERE (stb) and written as sibling .ctex files (TextureAsset
// format) next to the mesh's cooked output. CookedMaterial then references the
// .ctex basename; the loader resolves that against the cooked file's own
// directory — cooked skinned meshes render textured with zero Assimp.
static std::string resolveCookTexture(const aiScene* scene, const aiString& tp,
                                      const CookContext& ctx, int slot,
                                      bool isNormalMap) {
    const aiTexture* emb = scene->GetEmbeddedTexture(tp.C_Str());
    if (!emb)
        return std::filesystem::path(tp.C_Str()).filename().string();

    std::vector<uint8_t> rgba;
    uint32_t tw = 0, th = 0;
    if (emb->mHeight == 0) {   // compressed (png/jpg bytes)
        int w = 0, h = 0, ch = 0;
        stbi_uc* px = stbi_load_from_memory(
            reinterpret_cast<const stbi_uc*>(emb->pcData),
            (int)emb->mWidth, &w, &h, &ch, 4);
        if (!px) return {};
        tw = (uint32_t)w; th = (uint32_t)h;
        rgba.assign(px, px + (size_t)w * h * 4);
        stbi_image_free(px);
    } else {                    // raw BGRA texels
        tw = emb->mWidth; th = emb->mHeight;
        rgba.resize((size_t)tw * th * 4);
        for (size_t p = 0; p < (size_t)tw * th; ++p) {
            rgba[p*4+0] = emb->pcData[p].r;
            rgba[p*4+1] = emb->pcData[p].g;
            rgba[p*4+2] = emb->pcData[p].b;
            rgba[p*4+3] = emb->pcData[p].a;
        }
    }

    // Block-compress + mips: the material slot tells us the usage exactly
    // (BC5 for normal maps, BC7 for color) — no filename guessing here.
    assetlib::TextureAsset tex;
    if (!cook::encodeTexture(rgba.data(), tw, th, isNormalMap, tex)) return {};

    return writeSiblingTexture(tex, ctx, slot, tw, th, isNormalMap, "embedded");
}

static void emitMaterials(const aiScene* scene, MeshAsset& asset,
                          const CookContext& ctx) {
    asset.materials.reserve(scene->mNumMaterials);
    int texSlot = 0;
    for (uint32_t m = 0; m < scene->mNumMaterials; ++m) {
        const aiMaterial* aiMat = scene->mMaterials[m];
        CookedMaterial cm{};
        aiColor4D col{1.0f, 1.0f, 1.0f, 1.0f};
        if (aiGetMaterialColor(aiMat, AI_MATKEY_COLOR_DIFFUSE, &col) == AI_SUCCESS) {
            cm.baseColorFactor[0]=col.r; cm.baseColorFactor[1]=col.g;
            cm.baseColorFactor[2]=col.b; cm.baseColorFactor[3]=col.a;
        }
        float rough = 0.7f, metal = 0.0f;
        aiGetMaterialFloat(aiMat, AI_MATKEY_ROUGHNESS_FACTOR, &rough);
        aiGetMaterialFloat(aiMat, AI_MATKEY_METALLIC_FACTOR,  &metal);
        cm.roughness = rough; cm.metallic = metal;

        aiString tp;
        if (aiMat->GetTexture(aiTextureType_DIFFUSE, 0, &tp) == AI_SUCCESS ||
            aiMat->GetTexture(aiTextureType_BASE_COLOR, 0, &tp) == AI_SUCCESS) {
            const std::string fn = resolveCookTexture(scene, tp, ctx, texSlot++,
                                                      /*isNormalMap*/ false);
            if (!fn.empty()) {
                std::snprintf(cm.baseColorPath, sizeof(cm.baseColorPath), "%s", fn.c_str());
                cm.flags |= kMatFlag_HasBaseColor;
            }
        }
        aiString np;
        if (aiMat->GetTexture(aiTextureType_NORMALS, 0, &np) == AI_SUCCESS ||
            aiMat->GetTexture(aiTextureType_HEIGHT,  0, &np) == AI_SUCCESS) {
            const std::string fn = resolveCookTexture(scene, np, ctx, texSlot++,
                                                      /*isNormalMap*/ true);
            if (!fn.empty()) {
                std::snprintf(cm.normalMapPath, sizeof(cm.normalMapPath), "%s", fn.c_str());
                cm.flags |= kMatFlag_HasNormalMap;
            }
        }
        asset.materials.push_back(cm);
    }
    asset.header.materialCount = (uint32_t)asset.materials.size();
}

// ── Skinned cook ─────────────────────────────────────────────────────────────
// Vertices stay in MESH space (the skin matrices place them at runtime), so
// node transforms are NOT baked. Import settings must match the runtime
// loader exactly (PRESERVE_PIVOTS=false) or clip tracks land on synthetic
// node names the cooked skeleton doesn't have.
static CookResult cookSkinned(const CookContext& ctx) {
    Assimp::Importer imp;
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    imp.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
                           aiPrimitiveType_POINT | aiPrimitiveType_LINE);
    const aiScene* scene = imp.ReadFile(ctx.sourcePath.string(), kImportFlags);
    if (!scene || !scene->mRootNode)
        return {.success = false, .error = "Assimp (skinned re-import) failed"};

    Skeleton skel = anim::extractSkeleton(scene);
    if (skel.boneCount() == 0)
        return {.success = false, .error = "skinned mesh with no extractable skeleton"};
    if (!anim::buildOzzSkeleton(skel))
        return {.success = false, .error = "ozz skeleton build failed"};

    MeshAsset asset;
    asset.header.magic        = 0x4D455348;
    asset.header.version      = 3;
    asset.header.vertexFlags  = kSkinnedFlags;
    asset.header.vertexStride = sizeof(CookSkinnedVertex);
    asset.header.boneCount    = (uint32_t)skel.boneCount();
    std::memcpy(asset.header.uuid, ctx.uuid.bytes.data(), 16);

    // 1. Size from skinned meshes only.
    uint32_t totalVerts = 0, totalIndices = 0;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        if (mesh->HasPositions() && mesh->mNumBones > 0) {
            totalVerts   += mesh->mNumVertices;
            totalIndices += mesh->mNumFaces * 3;
        }
    }
    if (totalVerts == 0)
        return {.success = false, .error = "no skinned geometry"};
    const bool use16 = (totalVerts <= 65535);
    asset.header.indexStride = use16 ? 2 : 4;
    asset.vertexData.resize((size_t)totalVerts * sizeof(CookSkinnedVertex));
    asset.indexData.resize((size_t)totalIndices * asset.header.indexStride);

    // 2. Emit (mesh space, submesh per source mesh).
    auto* verts = reinterpret_cast<CookSkinnedVertex*>(asset.vertexData.data());
    uint8_t* idxBytes = asset.indexData.data();
    uint32_t vWrite = 0, iByteOff = 0, vBase = 0;
    float bMin[3] = { FLT_MAX,  FLT_MAX,  FLT_MAX};
    float bMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        if (!mesh->HasPositions() || mesh->mNumBones == 0) continue;
        const auto boneData = anim::extractBoneWeights(mesh, skel);
        const bool hasN = mesh->HasNormals();
        const bool hasT = mesh->HasTangentsAndBitangents();
        const bool hasUV = mesh->HasTextureCoords(0);
        for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
            CookSkinnedVertex& o = verts[vWrite++];
            const aiVector3D& P = mesh->mVertices[v];
            o.px = P.x; o.py = P.y; o.pz = P.z;
            for (int i = 0; i < 3; ++i) {
                const float c = (&P.x)[i];
                bMin[i] = std::min(bMin[i], c);
                bMax[i] = std::max(bMax[i], c);
            }
            const aiVector3D N = hasN ? mesh->mNormals[v] : aiVector3D(0, 1, 0);
            o.nx = N.x; o.ny = N.y; o.nz = N.z;
            if (hasT) {
                const aiVector3D& T = mesh->mTangents[v];
                o.tx = T.x; o.ty = T.y; o.tz = T.z; o.tw = 1.0f;
            } else { o.tx = 1; o.ty = 0; o.tz = 0; o.tw = 1; }
            if (hasUV) { o.u = mesh->mTextureCoords[0][v].x;
                         o.v = mesh->mTextureCoords[0][v].y; }
            else       { o.u = o.v = 0.0f; }
            std::memcpy(o.joints,  boneData[v].joints,  4);
            std::memcpy(o.weights, boneData[v].weights, 16);
        }
        MeshSubmesh sub{};
        sub.indexOffset   = iByteOff / asset.header.indexStride;
        sub.indexCount    = mesh->mNumFaces * 3;
        sub.materialIndex = mesh->mMaterialIndex;
        for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            for (int k = 0; k < 3; ++k) {
                const uint32_t idx = face.mIndices[k] + vBase;
                if (use16) *reinterpret_cast<uint16_t*>(idxBytes + iByteOff)
                               = (uint16_t)idx;
                else       *reinterpret_cast<uint32_t*>(idxBytes + iByteOff) = idx;
                iByteOff += asset.header.indexStride;
            }
        }
        vBase += mesh->mNumVertices;
        asset.submeshes.push_back(sub);
    }
    asset.header.vertexCount  = totalVerts;
    asset.header.indexCount   = totalIndices;
    asset.header.submeshCount = (uint32_t)asset.submeshes.size();
    for (int i = 0; i < 3; ++i) {
        asset.header.boundsMin[i] = bMin[i];
        asset.header.boundsMax[i] = bMax[i];
    }

    emitMaterials(scene, asset, ctx);

    // 3. Bones + ozz skeleton archive (opaque blob to assetlib).
    asset.bones.reserve(skel.bones.size());
    for (const Bone& b : skel.bones) {
        CookedBone cb{};
        std::snprintf(cb.name, sizeof(cb.name), "%s", b.name.c_str());
        cb.parentIndex = b.parentIndex;
        cb.bindPosition[0]=b.bindPosition.x; cb.bindPosition[1]=b.bindPosition.y;
        cb.bindPosition[2]=b.bindPosition.z;
        cb.bindRotation[0]=b.bindRotation.x; cb.bindRotation[1]=b.bindRotation.y;
        cb.bindRotation[2]=b.bindRotation.z; cb.bindRotation[3]=b.bindRotation.w;
        cb.bindScale[0]=b.bindScale.x; cb.bindScale[1]=b.bindScale.y;
        cb.bindScale[2]=b.bindScale.z;
        std::memcpy(cb.inverseBindMatrix, b.inverseBindMatrix, 64);
        std::memcpy(cb.localBindMatrix,   b.localBindMatrix,   64);
        asset.bones.push_back(cb);
    }
    {
        ozz::io::MemoryStream ms;
        { ozz::io::OArchive a(&ms); a << *skel.ozz; }
        asset.skeletonBlob = drainOzzStream(ms);
    }

    // 4. Embedded clips (each an ozz animation archive).
    const std::string stem = ctx.sourcePath.stem().string();
    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        AnimClip clip = anim::buildOzzClip(scene->mAnimations[a], skel,
            scene->mNumAnimations > 1 ? stem + "_" + std::to_string(a) : stem);
        if (!clip.valid() || clip.mappedTracks == 0) continue;
        ozz::io::MemoryStream ms;
        { ozz::io::OArchive ar(&ms); ar << *clip.ozz; }
        CookedClipBlob blob;
        blob.name         = clip.name;
        blob.mappedTracks = clip.mappedTracks;
        blob.totalTracks  = clip.totalTracks;
        blob.blob         = drainOzzStream(ms);
        asset.clips.push_back(std::move(blob));
    }

    appendLodLevels(asset);
    if (!saveMesh(asset, ctx.outputPath))
        return {.success = false, .error = "saveMesh failed"};
    std::printf("[MeshCooker] %s -> SKINNED verts=%u idx=%u bones=%u clips=%zu\n",
        ctx.sourcePath.filename().string().c_str(),
        totalVerts, totalIndices, asset.header.boneCount, asset.clips.size());
    return {.success = true};
}

// ── Assimp concurrency gate ──────────────────────────────────────────────────
// cookMany fans cook() across every core; each mesh cook stands up a full
// Assimp import (node hierarchies, split vertex channels, JoinIdenticalVertices
// caches — routinely several × the file size in RAM). A dozen concurrent
// high-poly FBX imports multiply that peak into OOM territory (cooker audit:
// "Assimp Parallel Memory Explosion"). Heavy mesh imports serialize through
// this gate — hw/4 permits, clamped to [2,4] — while texture/scene cooks
// keep scaling across all cores. Blocking a worker here is fine: it just
// becomes ordering, not lost parallelism, since the gate IS the bottleneck
// resource (RAM).
namespace {
std::counting_semaphore<8>& assimpGate() {
    static std::counting_semaphore<8> gate{(std::ptrdiff_t)std::clamp(
        std::thread::hardware_concurrency() / 4u, 2u, 4u)};
    return gate;
}
struct AssimpGatePass {
    AssimpGatePass()  { assimpGate().acquire(); }
    ~AssimpGatePass() { assimpGate().release(); }
};
} // namespace

// ── glTF cook (cgltf) ────────────────────────────────────────────────────────
// Assimp is deliberately built WITHOUT glTF (cgltf owns the format engine-
// wide), so .gltf/.glb never reached the cooker: those scene meshes simply
// didn't exist in shipped builds. Static-only for now (mirrors the runtime
// GltfImporter's coverage); node world transforms are baked exactly like
// the Assimp path.

// Column-major world × point (glTF matrix convention).
static void gltfXformPoint(const float m[16], const float p[3], float* o) {
    o[0] = m[0]*p[0] + m[4]*p[1] + m[8]*p[2]  + m[12];
    o[1] = m[1]*p[0] + m[5]*p[1] + m[9]*p[2]  + m[13];
    o[2] = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14];
}
static void gltfXformDir(const float m[16], const float v[3], float* o) {
    o[0] = m[0]*v[0] + m[4]*v[1] + m[8]*v[2];
    o[1] = m[1]*v[0] + m[5]*v[1] + m[9]*v[2];
    o[2] = m[2]*v[0] + m[6]*v[1] + m[10]*v[2];
}
// Inverse-transpose of the upper 3x3 with the SAME scale-invariant
// singularity guard as cookNormalMatrix (|det| vs row-norm product — a
// bare epsilon collapses for small uniform scales; see the audit note).
static void gltfNormalMatrix(const float m[16], float n[9]) {
    const float a=m[0],b=m[4],c=m[8], d=m[1],e=m[5],f=m[9], g=m[2],h=m[6],i=m[10];
    const float A=e*i-f*h, B=f*g-d*i, C=d*h-e*g;
    const float det  = a*A + b*B + c*C;
    const float r0   = std::sqrt(a*a + b*b + c*c);
    const float r1   = std::sqrt(d*d + e*e + f*f);
    const float r2   = std::sqrt(g*g + h*h + i*i);
    const float norm = r0 * r1 * r2;
    if (norm <= 0.0f || std::fabs(det) <= 1e-6f * norm) {
        n[0]=n[4]=n[8]=1.0f; n[1]=n[2]=n[3]=n[5]=n[6]=n[7]=0.0f;
        return;
    }
    const float s = 1.0f / det;
    n[0]=A*s;         n[1]=B*s;         n[2]=C*s;
    n[3]=(c*h-b*i)*s; n[4]=(a*i-c*g)*s; n[5]=(b*g-a*h)*s;
    n[6]=(b*f-c*e)*s; n[7]=(c*d-a*f)*s; n[8]=(a*e-b*d)*s;
}

static const cgltf_accessor* gltfAttr(const cgltf_primitive* prim,
                                      cgltf_attribute_type type) {
    for (cgltf_size i = 0; i < prim->attributes_count; ++i)
        if (prim->attributes[i].type == type && prim->attributes[i].index == 0)
            return prim->attributes[i].data;
    return nullptr;
}

// Cook a glTF image (external file or embedded buffer view) to a sibling
// .ctex — same contract as the FBX embedded-texture flow above.
static std::string gltfCookTexture(const cgltf_image* img,
                                   const std::filesystem::path& gltfDir,
                                   const CookContext& ctx, int slot,
                                   bool isNormalMap) {
    if (!img) return {};
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = nullptr;
    if (img->uri && std::strncmp(img->uri, "data:", 5) != 0) {
        char decoded[1024];
        cgltf_decode_uri(img->uri);   // %20 → ' ' etc. (in place)
        std::snprintf(decoded, sizeof decoded, "%s", img->uri);
        px = stbi_load((gltfDir / decoded).string().c_str(), &w, &h, &ch, 4);
    } else if (img->buffer_view && img->buffer_view->buffer->data) {
        const auto* bytes = (const stbi_uc*)img->buffer_view->buffer->data
                          + img->buffer_view->offset;
        px = stbi_load_from_memory(bytes, (int)img->buffer_view->size,
                                   &w, &h, &ch, 4);
    }
    if (!px) return {};

    // Block-compress + mips — usage from the material slot (BC5 normals).
    assetlib::TextureAsset tex;
    const bool ok = cook::encodeTexture(px, (uint32_t)w, (uint32_t)h,
                                        isNormalMap, tex);
    stbi_image_free(px);
    if (!ok) return {};

    return writeSiblingTexture(tex, ctx, slot, (uint32_t)w, (uint32_t)h,
                               isNormalMap, "glTF");
}

static CookResult cookGltf(const CookContext& ctx) {
    cgltf_options options{};
    cgltf_data*   data = nullptr;
    const std::string src = ctx.sourcePath.string();
    if (cgltf_parse_file(&options, src.c_str(), &data) != cgltf_result_success)
        return {.success = false, .error = "cgltf: parse failed"};
    struct Guard { cgltf_data* d; ~Guard() { cgltf_free(d); } } guard{data};
    if (cgltf_load_buffers(&options, data, src.c_str()) != cgltf_result_success)
        return {.success = false, .error = "cgltf: buffer load failed"};

    const cgltf_scene* scene = data->scene ? data->scene
                             : (data->scenes_count ? &data->scenes[0] : nullptr);
    if (!scene) return {.success = false, .error = "cgltf: no scene"};

    // Pass 1 — size buffers over the node tree (instancing-aware).
    uint32_t totalVerts = 0, totalIndices = 0;
    std::function<void(const cgltf_node*)> countN = [&](const cgltf_node* n) {
        if (n->mesh)
            for (cgltf_size pi = 0; pi < n->mesh->primitives_count; ++pi) {
                const cgltf_primitive& prim = n->mesh->primitives[pi];
                if (prim.type != cgltf_primitive_type_triangles) continue;
                const cgltf_accessor* pos = gltfAttr(&prim, cgltf_attribute_type_position);
                if (!pos || !prim.indices) continue;
                totalVerts   += (uint32_t)pos->count;
                totalIndices += (uint32_t)prim.indices->count;
            }
        for (cgltf_size c = 0; c < n->children_count; ++c) countN(n->children[c]);
    };
    for (cgltf_size i = 0; i < scene->nodes_count; ++i) countN(scene->nodes[i]);
    if (totalVerts == 0)
        return {.success = false, .error = "cgltf: no triangle geometry"};

    MeshAsset asset;
    asset.header.magic        = 0x4D455348;
    asset.header.version      = 2;
    asset.header.vertexFlags  = kCookFlags;
    asset.header.vertexStride = sizeof(CookVertex);
    std::memcpy(asset.header.uuid, ctx.uuid.bytes.data(), 16);

    const bool use16 = (totalVerts <= 65535);
    asset.header.indexStride = use16 ? 2 : 4;
    asset.vertexData.resize((size_t)totalVerts   * sizeof(CookVertex));
    asset.indexData.resize ((size_t)totalIndices * asset.header.indexStride);

    auto* verts = reinterpret_cast<CookVertex*>(asset.vertexData.data());
    uint8_t* idxBytes = asset.indexData.data();
    uint32_t vWrite = 0, iByteOff = 0, vBase = 0;
    float bMin[3] = { FLT_MAX,  FLT_MAX,  FLT_MAX};
    float bMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    // Pass 2 — bake node world transforms into the vertices.
    std::function<void(const cgltf_node*)> emitN = [&](const cgltf_node* n) {
        if (n->mesh) {
            float world[16];
            cgltf_node_transform_world(n, world);
            float nm[9];
            gltfNormalMatrix(world, nm);   // scale-invariant guard inside

            for (cgltf_size pi = 0; pi < n->mesh->primitives_count; ++pi) {
                const cgltf_primitive& prim = n->mesh->primitives[pi];
                if (prim.type != cgltf_primitive_type_triangles) continue;
                const cgltf_accessor* pos = gltfAttr(&prim, cgltf_attribute_type_position);
                const cgltf_accessor* nrm = gltfAttr(&prim, cgltf_attribute_type_normal);
                const cgltf_accessor* uv  = gltfAttr(&prim, cgltf_attribute_type_texcoord);
                const cgltf_accessor* tan = gltfAttr(&prim, cgltf_attribute_type_tangent);
                if (!pos || !prim.indices) continue;

                for (cgltf_size v = 0; v < pos->count; ++v) {
                    CookVertex& vtx = verts[vWrite++];
                    float p[3] = {0,0,0}, o[3];
                    cgltf_accessor_read_float(pos, v, p, 3);
                    gltfXformPoint(world, p, o);
                    vtx.px = o[0]; vtx.py = o[1]; vtx.pz = o[2];
                    for (int k = 0; k < 3; ++k) {
                        bMin[k] = std::min(bMin[k], o[k]);
                        bMax[k] = std::max(bMax[k], o[k]);
                    }

                    float nv[3] = {0,1,0}, no[3] = {0,1,0};
                    if (nrm && cgltf_accessor_read_float(nrm, v, nv, 3)) {
                        no[0] = nm[0]*nv[0]+nm[1]*nv[1]+nm[2]*nv[2];
                        no[1] = nm[3]*nv[0]+nm[4]*nv[1]+nm[5]*nv[2];
                        no[2] = nm[6]*nv[0]+nm[7]*nv[1]+nm[8]*nv[2];
                        const float l = std::sqrt(no[0]*no[0]+no[1]*no[1]+no[2]*no[2]);
                        if (l > 1e-8f) { no[0]/=l; no[1]/=l; no[2]/=l; }
                    }
                    vtx.nx = no[0]; vtx.ny = no[1]; vtx.nz = no[2];

                    float t4[4] = {1,0,0,1};
                    if (tan && cgltf_accessor_read_float(tan, v, t4, 4)) {
                        float to[3];
                        gltfXformDir(world, t4, to);
                        const float l = std::sqrt(to[0]*to[0]+to[1]*to[1]+to[2]*to[2]);
                        if (l > 1e-8f) { to[0]/=l; to[1]/=l; to[2]/=l; }
                        vtx.tx = to[0]; vtx.ty = to[1]; vtx.tz = to[2];
                        vtx.tw = t4[3] < 0 ? -1.0f : 1.0f;
                    } else {
                        vtx.tx = 1.0f; vtx.ty = 0.0f; vtx.tz = 0.0f; vtx.tw = 1.0f;
                    }

                    float uvv[2] = {0,0};
                    if (uv) cgltf_accessor_read_float(uv, v, uvv, 2);
                    vtx.u = uvv[0]; vtx.v = uvv[1];
                }

                MeshSubmesh sub{};
                sub.indexOffset   = iByteOff / asset.header.indexStride;
                sub.indexCount    = (uint32_t)prim.indices->count;
                sub.materialIndex = prim.material
                    ? (uint32_t)(prim.material - data->materials) : 0;
                if (use16) {
                    auto* d = reinterpret_cast<uint16_t*>(&idxBytes[iByteOff]);
                    for (cgltf_size i = 0; i < prim.indices->count; ++i)
                        d[i] = (uint16_t)(cgltf_accessor_read_index(prim.indices, i) + vBase);
                    iByteOff += sub.indexCount * 2;
                } else {
                    auto* d = reinterpret_cast<uint32_t*>(&idxBytes[iByteOff]);
                    for (cgltf_size i = 0; i < prim.indices->count; ++i)
                        d[i] = (uint32_t)(cgltf_accessor_read_index(prim.indices, i) + vBase);
                    iByteOff += sub.indexCount * 4;
                }
                vBase += (uint32_t)pos->count;
                asset.submeshes.push_back(sub);
            }
        }
        for (cgltf_size c = 0; c < n->children_count; ++c) emitN(n->children[c]);
    };
    for (cgltf_size i = 0; i < scene->nodes_count; ++i) emitN(scene->nodes[i]);

    asset.header.vertexCount  = totalVerts;
    asset.header.indexCount   = totalIndices;
    asset.header.submeshCount = (uint32_t)asset.submeshes.size();
    for (int i = 0; i < 3; ++i) {
        asset.header.boundsMin[i] = bMin[i];
        asset.header.boundsMax[i] = bMax[i];
    }

    // Materials — submesh.materialIndex indexes data->materials order.
    const auto gltfDir = ctx.sourcePath.parent_path();
    int texSlot = 0;
    for (cgltf_size m = 0; m < data->materials_count; ++m) {
        const cgltf_material& gm = data->materials[m];
        CookedMaterial cm{};
        cm.baseColorFactor[0] = cm.baseColorFactor[1] =
        cm.baseColorFactor[2] = cm.baseColorFactor[3] = 1.0f;
        cm.roughness = 0.7f; cm.metallic = 0.0f;
        if (gm.has_pbr_metallic_roughness) {
            const auto& pbr = gm.pbr_metallic_roughness;
            std::memcpy(cm.baseColorFactor, pbr.base_color_factor, 16);
            cm.roughness = pbr.roughness_factor;
            cm.metallic  = pbr.metallic_factor;
            if (pbr.base_color_texture.texture) {
                const std::string fn = gltfCookTexture(
                    pbr.base_color_texture.texture->image, gltfDir, ctx,
                    texSlot++, /*isNormalMap*/ false);
                if (!fn.empty()) {
                    std::snprintf(cm.baseColorPath, sizeof(cm.baseColorPath),
                                  "%s", fn.c_str());
                    cm.flags |= kMatFlag_HasBaseColor;
                }
            }
        }
        if (gm.normal_texture.texture) {
            const std::string fn = gltfCookTexture(
                gm.normal_texture.texture->image, gltfDir, ctx,
                texSlot++, /*isNormalMap*/ true);
            if (!fn.empty()) {
                std::snprintf(cm.normalMapPath, sizeof(cm.normalMapPath),
                              "%s", fn.c_str());
                cm.flags |= kMatFlag_HasNormalMap;
            }
        }
        asset.materials.push_back(cm);
    }
    if (asset.materials.empty()) asset.materials.push_back(CookedMaterial{});
    asset.header.materialCount = (uint32_t)asset.materials.size();

    appendLodLevels(asset);
    if (!saveMesh(asset, ctx.outputPath))
        return {.success = false, .error = "saveMesh failed"};
    std::printf("[MeshCooker] %s -> GLTF verts=%u idx=%u submeshes=%zu mats=%zu\n",
                ctx.sourcePath.filename().string().c_str(),
                totalVerts, totalIndices, asset.submeshes.size(),
                asset.materials.size());
    return {.success = true};
}

std::string MeshCooker::settingsFingerprint(const CookContext&) const {
    // Embedded/material textures share cook::encodeTexture, so the quality
    // tier changes this cooker's .ctex outputs (mirrors texture_encode.cpp).
    const char* hqEnv = std::getenv("COOK_TEX_HQ");
    const bool  hq    = hqEnv && *hqEnv && hqEnv[0] != '0';
    return std::string("hq=") + (hq ? "1" : "0");
}

void MeshCooker::enumerateOutputs(const std::filesystem::path& primary,
                                  std::vector<std::filesystem::path>& out) const {
    // Read the sibling set back out of the cooked mesh itself. cook() writes
    // each embedded texture as "<uuid>_tN.ctex" and stores that BASENAME in the
    // material record, so the material table is an exact list of what this cook
    // produced — unlike a glob, which would also match stale siblings that an
    // earlier cooker version left behind (nothing prunes .cache).
    assetlib::MeshAsset mesh;
    if (!assetlib::loadMesh(mesh, primary)) return;

    const auto dir = primary.parent_path();
    auto add = [&](const char* name, uint32_t flag, uint32_t flags) {
        if (!(flags & flag) || name[0] == '\0') return;
        auto p = dir / name;
        if (std::find(out.begin(), out.end(), p) == out.end())
            out.push_back(std::move(p));   // materials commonly share a texture
    };
    for (const auto& m : mesh.materials) {
        add(m.baseColorPath, assetlib::kMatFlag_HasBaseColor, m.flags);
        add(m.normalMapPath, assetlib::kMatFlag_HasNormalMap, m.flags);
    }
}

CookResult MeshCooker::cook(const CookContext& ctx) {
    // Sibling-texture dedup is scoped to ONE asset: entries map content to a
    // filename qualified by THIS mesh's uuid stem, so carrying them into the
    // next cook would hand out another mesh's sibling. Cleared here rather
    // than trusted to process lifetime, because the in-process fallback
    // (COOK_INPROC=1) reuses threads across assets.
    g_siblingByContent.clear();

    // glTF/GLB goes through cgltf — Assimp is built without those importers
    // (cgltf owns the format engine-wide). Everything else: Assimp.
    {
        std::string ext = ctx.sourcePath.extension().string();
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".gltf" || ext == ".glb") return cookGltf(ctx);
    }

    // One permit covers the whole cook, including the skinned re-import —
    // both Assimp scenes of this asset count as ONE resident import.
    AssimpGatePass gate;

    Assimp::Importer imp;   // isolated per cook — safe inside the worker pool
    imp.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
                           aiPrimitiveType_POINT | aiPrimitiveType_LINE);
    const aiScene* scene = imp.ReadFile(ctx.sourcePath.string(), kImportFlags);
    if (!scene || !scene->mRootNode) {
        const char* why = imp.GetErrorString();
        return {.success = false, .error = std::string("Assimp: ")
                + ((why && why[0]) ? why : "importer returned no scene")};
    }

    // Animation-only files (Mixamo clip FBXs: skeleton + animation, ZERO
    // meshes) load fine but Assimp flags them INCOMPLETE. They are valid
    // assets — clips import via the runtime Assimp path until the animation
    // cooker lands — so classify as skipped-by-design, not failed.
    if (scene->mNumMeshes == 0 && scene->mNumAnimations > 0) {
        std::printf("[MeshCooker] %s — animation-only, skipping cook (runtime Assimp path)\n",
                    ctx.sourcePath.filename().string().c_str());
        return {.success = false, .skipped = true,
                .error = "animation-only file — clips load via the runtime Assimp path"};
    }
    if (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) {
        const char* why = imp.GetErrorString();
        return {.success = false, .error = std::string("Assimp: incomplete scene")
                + (((why && why[0])) ? (std::string(" — ") + why) : std::string()) };
    }

    // Skinned meshes (bones + animation) cannot be cooked yet — the cook format
    // doesn't store SkinnedVertex, skeleton, or animation clips. The runtime
    // Assimp path in async_loader.cpp handles those correctly. Skip cooking so
    // the binary fast path falls through to Assimp for these files.
    {
        bool hasBones = false;
        for (unsigned m = 0; m < scene->mNumMeshes && !hasBones; ++m)
            if (scene->mMeshes[m]->mNumBones > 0) hasBones = true;
        if (hasBones) return cookSkinned(ctx);   // v3 skinned payload
    }

    MeshAsset asset;
    asset.header.magic        = 0x4D455348;
    asset.header.version      = 2;
    asset.header.vertexFlags  = kCookFlags;
    asset.header.vertexStride = sizeof(CookVertex);
    std::memcpy(asset.header.uuid, ctx.uuid.bytes.data(), 16);

    // 1. Size buffers from the full node tree (instancing-aware).
    uint32_t totalVerts = 0, totalIndices = 0;
    countNode(scene, scene->mRootNode, totalVerts, totalIndices);

    const bool use16 = (totalVerts <= 65535);
    asset.header.indexStride = use16 ? 2 : 4;
    asset.vertexData.resize((size_t)totalVerts  * sizeof(CookVertex));
    asset.indexData.resize ((size_t)totalIndices * asset.header.indexStride);

    // 2. Emit: bake each node's accumulated world transform into its meshes.
    EmitState st;
    st.verts     = reinterpret_cast<CookVertex*>(asset.vertexData.data());
    st.idxBytes  = asset.indexData.data();
    st.use16     = use16;
    st.idxStride = asset.header.indexStride;
    st.submeshes = &asset.submeshes;
    if (totalVerts > 0)
        emitNode(scene, scene->mRootNode, aiMatrix4x4(), st);

    asset.header.vertexCount  = totalVerts;
    asset.header.indexCount   = totalIndices;
    asset.header.submeshCount = (uint32_t)asset.submeshes.size();
    for (int i = 0; i < 3; ++i) {
        asset.header.boundsMin[i] = totalVerts ? st.bMin[i] : 0.0f;
        asset.header.boundsMax[i] = totalVerts ? st.bMax[i] : 0.0f;
    }

    // 3. Materials — submesh.materialIndex indexes this array in source order.
    asset.materials.reserve(scene->mNumMaterials);
    for (uint32_t m = 0; m < scene->mNumMaterials; ++m) {
        const aiMaterial* aiMat = scene->mMaterials[m];
        CookedMaterial cm{};

        aiColor4D col{1.0f, 1.0f, 1.0f, 1.0f};
        if (aiGetMaterialColor(aiMat, AI_MATKEY_COLOR_DIFFUSE, &col) == AI_SUCCESS) {
            cm.baseColorFactor[0]=col.r; cm.baseColorFactor[1]=col.g;
            cm.baseColorFactor[2]=col.b; cm.baseColorFactor[3]=col.a;
        }
        float rough = 0.7f, metal = 0.0f;
        aiGetMaterialFloat(aiMat, AI_MATKEY_ROUGHNESS_FACTOR, &rough);
        aiGetMaterialFloat(aiMat, AI_MATKEY_METALLIC_FACTOR,  &metal);
        cm.roughness = rough; cm.metallic = metal;

        aiString tp;
        if (aiMat->GetTexture(aiTextureType_DIFFUSE, 0, &tp) == AI_SUCCESS ||
            aiMat->GetTexture(aiTextureType_BASE_COLOR, 0, &tp) == AI_SUCCESS) {
            auto fn = std::filesystem::path(tp.C_Str()).filename().string();
            std::snprintf(cm.baseColorPath, sizeof(cm.baseColorPath), "%s", fn.c_str());
            cm.flags |= kMatFlag_HasBaseColor;
        }
        aiString np;
        if (aiMat->GetTexture(aiTextureType_NORMALS, 0, &np) == AI_SUCCESS ||
            aiMat->GetTexture(aiTextureType_HEIGHT,  0, &np) == AI_SUCCESS) {
            auto fn = std::filesystem::path(np.C_Str()).filename().string();
            std::snprintf(cm.normalMapPath, sizeof(cm.normalMapPath), "%s", fn.c_str());
            cm.flags |= kMatFlag_HasNormalMap;
        }
        asset.materials.push_back(cm);
    }
    asset.header.materialCount = (uint32_t)asset.materials.size();

    appendLodLevels(asset);
    if (!saveMesh(asset, ctx.outputPath))
        return {.success = false, .error = "saveMesh failed"};

    std::printf("[MeshCooker] %s -> verts=%u idx=%u submeshes=%u mats=%u\n",
        ctx.sourcePath.filename().string().c_str(),
        asset.header.vertexCount, asset.header.indexCount,
        asset.header.submeshCount, asset.header.materialCount);
    return {.success = true};
}

