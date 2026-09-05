#include "core/logger.h"
#include "assets/importers/assimp_importer.h"
#include "render/vertex.h"
#include "render/skinned_vertex.h"
#include "render/mesh.h"
#include "render/texture.h"
#include "render/material.h"
#include "animation/assimp_skeleton_loader.h"
#include "animation/ozz_bridge.h"
#include "animation/skeleton_registry.h"
#include "animation/clip_registry.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>

#include <stb_image.h>
#include "render/gpu.h"
#include <assetlib/texture_asset.h>   // kTexRGBA8
#include <bx/math.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>
#include <cmath>
#include <cstdlib>

// -----------------------------------------------------------------------
// Internal helpers — file-scope only
// -----------------------------------------------------------------------


// When an FBX has no texture paths baked in, search the directory for
// files matching the model's base name and common suffix conventions.
// Handles asset packs like CGTrader/Fab that export textures separately.
static std::string discoverTexture(const std::filesystem::path& dir,
                                    const std::string& baseName) {
    // Priority-ordered suffixes — _COL / _Albedo / _BaseColor = diffuse
    static const char* kSuffixes[] = {
        "_COL", "_Albedo", "_BaseColor", "_Base_Color",
        "_Diffuse", "_diffuse", "_D", "_color", nullptr
    };
    static const char* kExts[] = { ".jpg", ".png", ".jpeg", ".tga", nullptr };

    // Files that advertise a NON-color map — binding one of these as the
    // diffuse map is the failure a loose substring test invites (a normal map
    // named rock_COL_normal.png contains "_col"). Veto them outright.
    static const char* kNonDiffuse[] = {
        "normal", "_nor", "_nrm", "rough", "metal", "_ao",
        "height", "_disp", "specular", "_spec", nullptr
    };

    std::string lowerBase = baseName;
    for (auto& c : lowerBase) c = (char)std::tolower((unsigned char)c);

    try {
        for (const auto& de : std::filesystem::directory_iterator(dir)) {
            if (!de.is_regular_file()) continue;
            std::string fname = de.path().filename().string();
            std::string lf = fname;
            for (auto& c : lf) c = (char)std::tolower((unsigned char)c);

            // Must start with the model base name
            if (lf.find(lowerBase) != 0) continue;

            // Must have an image extension
            bool goodExt = false;
            for (int i = 0; kExts[i]; ++i)
                if (de.path().extension() == kExts[i]) { goodExt = true; break; }
            if (!goodExt) continue;

            // Reject anything that names a non-color map — this is the diffuse
            // slot only (importer audit: "Broad Substring Matching").
            bool nonDiffuse = false;
            for (int i = 0; kNonDiffuse[i]; ++i)
                if (lf.find(kNonDiffuse[i]) != std::string::npos) { nonDiffuse = true; break; }
            if (nonDiffuse) continue;

            // Must contain a diffuse-indicating suffix
            for (int i = 0; kSuffixes[i]; ++i) {
                std::string ls = kSuffixes[i];
                for (auto& c : ls) c = (char)std::tolower((unsigned char)c);
                if (lf.find(ls) != std::string::npos) {
                    LOG_SUCCESS("Assimp", "Texture discovered: %s", fname.c_str());
                    return de.path().string();
                }
            }
        }
    } catch (...) {}
    return {};
}

static TextureHandle importTexture(const aiScene*   scene,
                                   const char*      rawPath,
                                   const std::filesystem::path& dir,
                                   AssetStorage&    storage) {
    int w = 0, h = 0;
    stbi_uc* pixels = nullptr;
    bool     needFree = true;

    if (rawPath && rawPath[0] == '*') {
        // Embedded texture (common in FBX) — index follows the '*'
        int idx = std::atoi(rawPath + 1);
        if (idx >= 0 && (uint32_t)idx < scene->mNumTextures) {
            const aiTexture* t = scene->mTextures[idx];
            if (t->mHeight == 0) {
                // Compressed blob (PNG/JPG) stored verbatim in the file
                int ch = 0;
                pixels = stbi_load_from_memory(
                    reinterpret_cast<const stbi_uc*>(t->pcData),
                    (int)t->mWidth, &w, &h, &ch, 4);
            } else {
                // Raw ARGB8888 — copy because we can't free aiTexture memory
                w = (int)t->mWidth;
                h = (int)t->mHeight;
                size_t sz = (size_t)(w * h * 4);
                pixels = (stbi_uc*)malloc(sz);
                // aiTexel's in-memory layout is BGRA, but its .r/.g/.b/.a
                // MEMBERS name the components — reading them by name yields
                // the correct RGBA order for the GPU regardless of byte order.
                const aiTexel* src = t->pcData;
                for (int p = 0; p < w * h; ++p) {
                    pixels[p*4+0] = src[p].r;
                    pixels[p*4+1] = src[p].g;
                    pixels[p*4+2] = src[p].b;
                    pixels[p*4+3] = src[p].a;
                }
            }
        }
    } else if (rawPath && rawPath[0] != '\0') {
        auto fullPath = (dir / rawPath).string();
        int ch = 0;
        pixels = stbi_load(fullPath.c_str(), &w, &h, &ch, 4);
        if (!pixels) {
            // Absolute/relative path baked into FBX failed (common with
            // Mixamo, Maya, etc. storing server-side paths). Fall back to
            // searching for the filename in the FBX's own directory and
            // common texture subdirectories.
            std::string filename =
                std::filesystem::path(rawPath).filename().string();
            const std::filesystem::path candidates[] = {
                dir / filename,
                dir / "textures" / filename,
                dir / "Textures" / filename,
                dir / "tex"      / filename,
            };
            for (const auto& c : candidates) {
                pixels = stbi_load(c.string().c_str(), &w, &h, &ch, 4);
                if (pixels) {
                    LOG_SUCCESS("Assimp", "Texture resolved: %s",
                                c.string().c_str());
                    break;
                }
            }
            if (!pixels) {
                std::printf("[Assimp] Texture not found: %s\n",
                            fullPath.c_str());
                return {};
            }
        }
    }

    if (!pixels || w == 0 || h == 0) return {};

    gpu::Blob* mem = gpu::copy(pixels, (uint32_t)(w * h * 4));
    stbi_image_free(pixels);   // safe even for malloc'd — stbi_image_free wraps free()

    gpu::TextureHandle th = gpu::createTexture2D(
        (uint16_t)w, (uint16_t)h, 1, assetlib::kTexRGBA8, mem);

    if (!th.valid()) return {};

    Texture tex;
    tex.handle = th;
    return storage.textures.addTexture(std::move(tex));
}

static MaterialHandle importMaterial(const aiScene*    scene,
                                     const aiMaterial* aiMat,
                                     const std::filesystem::path& dir,
                                     const std::string& baseName,
                                     AssetStorage&     storage) {
    float factor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    // Diffuse / base color factor
    aiColor4D col;
    if (AI_SUCCESS == aiGetMaterialColor(aiMat, AI_MATKEY_COLOR_DIFFUSE, &col)) {
        factor[0] = col.r; factor[1] = col.g; factor[2] = col.b; factor[3] = col.a;
    }

    // Diffuse / base color texture (try DIFFUSE then BASE_COLOR for PBR FBX)
    TextureHandle base;
    aiString texPath;
    if (AI_SUCCESS == aiMat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) ||
        AI_SUCCESS == aiMat->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath)) {
        base = importTexture(scene, texPath.C_Str(), dir, storage);
    }
    // FBX exported without texture paths — discover by naming convention
    if (!base.valid() && !baseName.empty()) {
        std::string discovered = discoverTexture(dir, baseName);
        if (!discovered.empty())
            base = importTexture(scene, discovered.c_str(), dir, storage);
    }

    // The defaults the fixed path used to apply implicitly (0.7 / 0.0) are now
    // written explicitly, because a block is never sparse.
    return storage.materials.addMaterial(
        Material::standard(factor, 0.7f, 0.0f, base));
}

// A running AABB grown vertex-by-vertex while submeshes are appended.
struct Bounds {
    float mn[3] = { 1e30f,  1e30f,  1e30f};
    float mx[3] = {-1e30f, -1e30f, -1e30f};
    void grow(float x, float y, float z) {
        mn[0]=std::min(mn[0],x); mx[0]=std::max(mx[0],x);
        mn[1]=std::min(mn[1],y); mx[1]=std::max(mx[1],y);
        mn[2]=std::min(mn[2],z); mx[2]=std::max(mx[2],z);
    }
    bool valid() const { return mn[0] <= mx[0]; }
};

// Append faces of one submesh into a shared 32-bit index buffer, offsetting
// each index by the running vertex base so they address the merged VB. Returns
// the SubmeshRange spanning what was just appended.
static SubmeshRange appendIndices(const aiMesh* aiM, uint32_t vertBase,
                                  std::vector<uint32_t>& indices,
                                  MaterialHandle mat) {
    const uint32_t start = (uint32_t)indices.size();
    for (uint32_t f = 0; f < aiM->mNumFaces; ++f)
        for (uint32_t i = 0; i < aiM->mFaces[f].mNumIndices; ++i)
            indices.push_back(vertBase + aiM->mFaces[f].mIndices[i]);
    SubmeshRange r;
    r.indexOffset = start;
    r.indexCount  = (uint32_t)indices.size() - start;
    r.material    = mat;
    return r;
}

// Append one submesh's STATIC vertices, BAKING the node's world transform so a
// multi-part model assembles correctly (parts placed by scene-graph nodes, not
// piled on the origin). Positions by the full matrix, normals by the 3x3
// inverse-transpose (guarded against degenerate scale) — mirrors the cooker.
static void appendStaticVerts(const aiMesh* aiM, std::vector<Vertex>& verts,
                              Bounds& b, const aiMatrix4x4& world) {
    aiMatrix3x3 nm(world);
    const float det = nm.Determinant();
    if (std::fabs(det) > 1e-12f) { nm.Inverse(); nm.Transpose(); }
    else                         { nm = aiMatrix3x3(); }
    for (uint32_t v = 0; v < aiM->mNumVertices; ++v) {
        Vertex vtx{};
        aiVector3D P = world * aiM->mVertices[v];
        vtx.position[0]=P.x; vtx.position[1]=P.y; vtx.position[2]=P.z;
        if (aiM->HasNormals()) {
            aiVector3D N = nm * aiM->mNormals[v]; N.Normalize();
            vtx.normal[0]=N.x; vtx.normal[1]=N.y; vtx.normal[2]=N.z;
        } else vtx.normal[1] = 1.0f;              // world-up fallback
        if (aiM->mTextureCoords[0]) {
            vtx.uv[0]=aiM->mTextureCoords[0][v].x; vtx.uv[1]=aiM->mTextureCoords[0][v].y;
        }
        b.grow(P.x, P.y, P.z);
        verts.push_back(vtx);
    }
}

// Append one submesh's SKINNED vertices. A submesh with no bones (rare in a
// skinned model) would collapse to the origin under zero weights, so bind those
// verts rigidly to bone 0 (weight 1) rather than lose the geometry.
static void appendSkinnedVerts(const aiMesh* aiM, const Skeleton& skel,
                               std::vector<SkinnedVertex>& verts, Bounds& b) {
    auto boneData = anim::extractBoneWeights(aiM, skel);
    const bool hasBones = aiM->mNumBones > 0;
    for (uint32_t v = 0; v < aiM->mNumVertices; ++v) {
        SkinnedVertex vtx{};
        vtx.position[0]=aiM->mVertices[v].x; vtx.position[1]=aiM->mVertices[v].y;
        vtx.position[2]=aiM->mVertices[v].z;
        if (aiM->HasNormals()) {
            vtx.normal[0]=aiM->mNormals[v].x; vtx.normal[1]=aiM->mNormals[v].y;
            vtx.normal[2]=aiM->mNormals[v].z;
        } else vtx.normal[1] = 1.0f;
        if (aiM->HasTangentsAndBitangents()) {
            vtx.tangent[0]=aiM->mTangents[v].x; vtx.tangent[1]=aiM->mTangents[v].y;
            vtx.tangent[2]=aiM->mTangents[v].z; vtx.tangent[3]=1.0f;
        }
        if (aiM->mTextureCoords[0]) {
            vtx.uv[0]=aiM->mTextureCoords[0][v].x; vtx.uv[1]=aiM->mTextureCoords[0][v].y;
        }
        if (hasBones) {
            std::memcpy(vtx.joints,  boneData[v].joints,  4);
            std::memcpy(vtx.weights, boneData[v].weights, sizeof(float) * 4);
        } else {
            vtx.joints[0]=0; vtx.weights[0]=1.0f;  // rigid to root — don't collapse
        }
        b.grow(vtx.position[0], vtx.position[1], vtx.position[2]);
        verts.push_back(vtx);
    }
}

// Finalize a merged mesh: one VB (static or skinned) + one 32/16-bit IB, with
// per-submesh ranges. A single submesh stays on the simple single-draw path
// (mesh.material set, submeshes left empty) so common models are unchanged.
template <class VertT>
static MeshHandle finalizeMesh(const std::vector<VertT>& verts,
                               const std::vector<uint32_t>& indices,
                               std::vector<SubmeshRange> submeshes,
                               const Bounds& b, const std::string& sourcePath,
                               AssetStorage& storage) {
    const bool use32 = verts.size() > 65535;
    gpu::IndexBufferHandle ibh;
    if (use32) {
        ibh = gpu::createIndexBuffer(
            gpu::copy(indices.data(), (uint32_t)(indices.size()*4)),
            gpu::IndexFormat::U32);
    } else {
        std::vector<uint16_t> idx16(indices.begin(), indices.end());
        ibh = gpu::createIndexBuffer(
            gpu::copy(idx16.data(), (uint32_t)(idx16.size()*2)),
            gpu::IndexFormat::U16);
    }
    gpu::VertexBufferHandle vbh = gpu::createVertexBuffer(
        gpu::copy(verts.data(), (uint32_t)(verts.size()*sizeof(VertT))),
        VertT::kGpuFormat);

    // Validate before wrapping — a failed GPU allocation must not be stored as
    // a live handle (importer audit: "Unchecked GPU Buffer Creation"; the glTF
    // importer already does this).
    if (!vbh.valid() || !ibh.valid()) {
        gpu::destroy(vbh);
        gpu::destroy(ibh);
        LOG_ERROR("Assimp", "GPU buffer creation failed for %s", sourcePath.c_str());
        return MeshHandle{};
    }

    Mesh mesh(vbh, ibh, (uint32_t)indices.size());
    mesh.material   = submeshes.empty() ? MaterialHandle{} : submeshes.front().material;
    mesh.sourcePath = sourcePath;
    if (b.valid()) {
        mesh.boundsMin = {b.mn[0], b.mn[1], b.mn[2]};
        mesh.boundsMax = {b.mx[0], b.mx[1], b.mx[2]};
    }
    if (submeshes.size() > 1) mesh.submeshes = std::move(submeshes);
    return storage.meshes.addMesh(std::move(mesh));
}

// Walk the node tree accumulating world = parent * node.transform, appending
// each node's static triangle meshes at that transform (like the cooker's
// emitNode). A mesh instanced under several nodes is emitted once per node.
static void emitStaticNode(const aiScene* s, const aiNode* node,
                           const aiMatrix4x4& parentWorld,
                           std::vector<Vertex>& verts, std::vector<uint32_t>& indices,
                           std::vector<SubmeshRange>& submeshes, Bounds& b,
                           const std::vector<MaterialHandle>& matHandles) {
    const aiMatrix4x4 world = parentWorld * node->mTransformation;
    for (unsigned i = 0; i < node->mNumMeshes; ++i) {
        const aiMesh* m = s->mMeshes[node->mMeshes[i]];
        if (!(m->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;
        MaterialHandle mh = (m->mMaterialIndex < matHandles.size())
                            ? matHandles[m->mMaterialIndex] : MaterialHandle{};
        const uint32_t vertBase = (uint32_t)verts.size();
        appendStaticVerts(m, verts, b, world);
        submeshes.push_back(appendIndices(m, vertBase, indices, mh));
    }
    for (unsigned c = 0; c < node->mNumChildren; ++c)
        emitStaticNode(s, node->mChildren[c], world, verts, indices, submeshes, b, matHandles);
}

// -----------------------------------------------------------------------
// AssimpImporter
// -----------------------------------------------------------------------

bool AssimpImporter::supports(std::string_view ext) const {
    // glTF/GLB intentionally omitted — cgltf handles those.
    static const char* kExts[] = {
        "fbx","obj","dae","3ds","ply","stl","blend",
        "x","lwo","lws","md2","md3","md5mesh", nullptr
    };
    for (int i = 0; kExts[i]; ++i)
        if (ext == kExts[i]) return true;
    return false;
}

MeshImportResult AssimpImporter::load(const std::string& path,
                                       AssetStorage&      storage) {
    Assimp::Importer importer;

    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate           | // all polys → triangles
        aiProcess_GenSmoothNormals      | // generate normals if absent
        aiProcess_CalcTangentSpace      | // needed for skinned normal mapping
        aiProcess_FlipUVs               | // match the Metal/D3D UV origin
        aiProcess_JoinIdenticalVertices | // deduplicate verts
        aiProcess_SortByPType           | // separate point/line/tri prims
        0);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
        return MeshImportResult::fail(std::string("Assimp: ") + importer.GetErrorString());

    LOG_INFO("Assimp", "%s  meshes=%u  mats=%u  anims=%u",
        path.c_str(), scene->mNumMeshes, scene->mNumMaterials,
        scene->mNumAnimations);

    const auto dir = std::filesystem::path(path).parent_path();
    const std::string baseName = std::filesystem::path(path).stem().string();

    // Detect if any mesh in the scene has bones
    bool hasBones = false;
    for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
        if (scene->mMeshes[i]->mNumBones > 0) { hasBones = true; break; }

    // Extract skeleton if bones are present (+ its ozz runtime skeleton)
    Skeleton skeleton;
    SkeletonHandle skelHandle{};
    if (hasBones) {
        skeleton = anim::extractSkeleton(scene);
        if (!skeleton.bones.empty() && !anim::buildOzzSkeleton(skeleton))
            skeleton = {};   // unusable without ozz data
        if (!skeleton.bones.empty() && storage.skeletons) {
            LOG_INFO("Assimp", "Skeleton: %d bones", skeleton.boneCount());
            skelHandle = storage.skeletons->add(Skeleton(skeleton)); // copy — we keep a ref
        }
    }

    // Embedded animation clips -> compressed ozz Animations (ozz_bridge)
    std::vector<AnimClipHandle> clipHandles;
    if (hasBones && !skeleton.bones.empty() && storage.clips) {
        const std::string base = baseName;
        for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
            AnimClip clip = anim::buildOzzClip(scene->mAnimations[a], skeleton, base);
            if (!clip.valid()) continue;
            LOG_INFO("Assimp", "Clip: \"%s\" duration=%.2fs tracks=%d/%d",
                     clip.name.c_str(), clip.duration,
                     clip.mappedTracks, clip.totalTracks);
            clipHandles.push_back(storage.clips->add(std::move(clip)));
        }
    }

    // Materials first so meshes can reference handles
    std::vector<MaterialHandle> matHandles(scene->mNumMaterials);
    for (uint32_t i = 0; i < scene->mNumMaterials; ++i)
        matHandles[i] = importMaterial(scene, scene->mMaterials[i], dir, baseName, storage);

    // Meshes — MERGE every triangle submesh into ONE mesh with a shared VB/IB
    // and a SubmeshRange per source mesh (each carrying its own material). This
    // is the representation the cooked path + renderer already use; importing
    // one Mesh per submesh (and returning only the first) silently dropped all
    // the rest. The whole model is skinned iff it has a skeleton — a merged
    // buffer needs one vertex format, and characters weight every submesh.
    const bool skinnedModel = !skeleton.bones.empty();

    std::vector<Vertex>        staticVerts;
    std::vector<SkinnedVertex> skinnedVerts;
    std::vector<uint32_t>      indices;
    std::vector<SubmeshRange>  submeshes;
    Bounds bounds;

    if (skinnedModel) {
        // Skinned: the skeleton's bind pose + bones place the verts, so merge
        // meshes flat (no node bake — that would double-transform the bind pose).
        for (uint32_t i = 0; i < scene->mNumMeshes; ++i) {
            const aiMesh* m = scene->mMeshes[i];
            if (!(m->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;
            MaterialHandle mh = (m->mMaterialIndex < matHandles.size())
                                ? matHandles[m->mMaterialIndex] : MaterialHandle{};
            const uint32_t vertBase = (uint32_t)skinnedVerts.size();
            appendSkinnedVerts(m, skeleton, skinnedVerts, bounds);
            submeshes.push_back(appendIndices(m, vertBase, indices, mh));
        }
    } else {
        // Static: walk the node tree so each part lands at its scene-graph
        // world transform instead of collapsing onto the origin.
        emitStaticNode(scene, scene->mRootNode, aiMatrix4x4(),
                       staticVerts, indices, submeshes, bounds, matHandles);
    }

    if (indices.empty())
        return MeshImportResult::fail("Assimp: no triangle meshes in file");

    const size_t subCount = submeshes.size();
    MeshHandle merged = skinnedModel
        ? finalizeMesh(skinnedVerts, indices, std::move(submeshes), bounds, path, storage)
        : finalizeMesh(staticVerts,  indices, std::move(submeshes), bounds, path, storage);

    if (!merged.valid())
        return MeshImportResult::fail("Assimp: GPU buffer creation failed: " + path);

    LOG_INFO("Assimp", "Merged mesh: %zu submesh(es), %u indices%s",
             subCount, (uint32_t)indices.size(), skinnedModel ? " [skinned]" : "");

    MeshImportResult result;
    result.success  = true;
    result.mesh     = merged;
    result.skeleton = skelHandle;
    result.clips    = std::move(clipHandles);
    return result;
}
