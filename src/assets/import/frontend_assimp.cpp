// ── AssimpFrontend — see frontend_assimp.h ────────────────────────────────────
#include "assets/import/frontend_assimp.h"
#include "core/thread_stack.h"
#include "assets/import/frontend_assimp_skeleton.h"   // extractSkeleton / extractBoneWeights: the old paths' own

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <semaphore>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace imp {
namespace {

// The old cook paths' post-processing, unchanged. SortByPType plus SBP_REMOVE
// leaves triangle-only meshes; ImproveCacheLocality stays HERE (Assimp-side)
// so FBX output keeps its vertex order (imported-scene.md §7.1).
constexpr unsigned kImportFlags =
    aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
    aiProcess_JoinIdenticalVertices | aiProcess_ImproveCacheLocality | aiProcess_FlipUVs |
    aiProcess_SortByPType;

// ── Assimp concurrency gate (moved from mesh_cooker.cpp) ────────────────────
// Each import stands up a full Assimp scene, several times the file size in
// RAM; a dozen concurrent high-poly FBX imports is an OOM. Heavy imports pass
// through hw/4 permits, clamped to [2,4]. Blocking a cook worker here is fine:
// RAM is the bottleneck, so it becomes ordering, not lost parallelism.
std::counting_semaphore<8>& assimpGate() {
    static std::counting_semaphore<8> gate{(std::ptrdiff_t)std::clamp(
        std::thread::hardware_concurrency() / 4u, 2u, 4u)};
    return gate;
}
struct AssimpGatePass {
    AssimpGatePass()  { assimpGate().acquire(); }
    ~AssimpGatePass() { assimpGate().release(); }
};

Float4x4 toFloat4x4(const aiMatrix4x4& m) {
    Float4x4 out;
    imp::assimp::aiMat4ToFloat16(m, out.m);            // column-major, translation in m[12..14]
    return out;
}

struct Converter {
    const aiScene& sc;
    std::filesystem::path dir;
    ImportedScene& out;
    std::set<std::string> droppedTextures;
    uint32_t morphMeshes = 0, colouredMeshes = 0, extraUvMeshes = 0, unweightedVerts = 0;

    // ── Textures: the plan's rule (§4.1), plus ONE fallback. An embedded texture
    // carries its bytes (encoded) or its pixels (raw texels). A path resolves as
    // written, relative to the source file, else as its basename beside the
    // source: the one location the old cooked format assumed. FBX files routinely
    // carry an absolute path from the author's machine, and this is what keeps
    // them working. Nothing else is searched; a miss is dropped, with its path.
    TextureRef texture(const aiString& raw) {
        TextureRef ref;
        if (const aiTexture* emb = sc.GetEmbeddedTexture(raw.C_Str())) {
            ref.embeddedName = raw.C_Str();
            if (emb->mHeight == 0) {                      // compressed: PNG/JPEG bytes
                const auto* b = reinterpret_cast<const uint8_t*>(emb->pcData);
                ref.embedded.assign(b, b + emb->mWidth);
            } else {                                     // raw texels, BGRA in memory
                ref.width = emb->mWidth; ref.height = emb->mHeight;
                ref.rgba.resize((size_t)ref.width * ref.height * 4);
                for (size_t p = 0; p < (size_t)ref.width * ref.height; ++p) {
                    ref.rgba[p * 4 + 0] = emb->pcData[p].r; ref.rgba[p * 4 + 1] = emb->pcData[p].g;
                    ref.rgba[p * 4 + 2] = emb->pcData[p].b; ref.rgba[p * 4 + 3] = emb->pcData[p].a;
                }
            }
            return ref;
        }
        std::string p = raw.C_Str();
        std::replace(p.begin(), p.end(), '\\', '/');
        const std::filesystem::path asWritten = std::filesystem::path(p).is_absolute() ? std::filesystem::path(p) : dir / p;
        const std::string base = std::filesystem::path(p).filename().string();
        std::error_code ec;
        if (std::filesystem::exists(asWritten, ec)) ref.path = p;   // relative to the source, or absolute
        else if (!base.empty() && std::filesystem::exists(dir / base, ec)) ref.path = base;
        else droppedTextures.insert("'" + p + "' (not found as written, nor beside the source)");
        return ref;
    }

    void materials() {
        for (unsigned i = 0; i < sc.mNumMaterials; ++i) {
            const aiMaterial* am = sc.mMaterials[i];
            Material m;
            aiString name;
            m.name = am->Get(AI_MATKEY_NAME, name) == AI_SUCCESS && name.length ? name.C_Str()
                                                                                 : "material " + std::to_string(i);
            aiColor4D c{1, 1, 1, 1};
            if (aiGetMaterialColor(am, AI_MATKEY_COLOR_DIFFUSE, &c) == AI_SUCCESS) m.baseColorFactor = {c.r, c.g, c.b, c.a};
            aiGetMaterialFloat(am, AI_MATKEY_ROUGHNESS_FACTOR, &m.roughness);
            aiGetMaterialFloat(am, AI_MATKEY_METALLIC_FACTOR,  &m.metallic);
            aiString t;
            if (am->GetTexture(aiTextureType_DIFFUSE, 0, &t) == AI_SUCCESS ||
                am->GetTexture(aiTextureType_BASE_COLOR, 0, &t) == AI_SUCCESS)
                m.baseColor = texture(t);
            aiString n;
            if (am->GetTexture(aiTextureType_NORMALS, 0, &n) == AI_SUCCESS ||
                am->GetTexture(aiTextureType_HEIGHT,  0, &n) == AI_SUCCESS)
                m.normal = texture(n);
            out.materials.push_back(std::move(m));
        }
    }

    // One imp::Mesh per aiMesh, in scene order: the order the old skinned path
    // emitted in, and the index every node refers to.
    void mesh(const aiMesh* am, const ::Skeleton* skel) {
        Mesh m;
        m.name = am->mName.length ? am->mName.C_Str() : "mesh " + std::to_string(out.meshes.size());
        const unsigned n = am->mNumVertices;
        m.positions.resize(n); m.normals.resize(n);
        for (unsigned v = 0; v < n; ++v) {
            m.positions[v] = {am->mVertices[v].x, am->mVertices[v].y, am->mVertices[v].z};
            const aiVector3D nv = am->HasNormals() ? am->mNormals[v] : aiVector3D(0, 1, 0);
            m.normals[v] = {nv.x, nv.y, nv.z};
        }
        if (am->HasTangentsAndBitangents()) {
            m.tangents.resize(n);
            for (unsigned v = 0; v < n; ++v) {
                const aiVector3D& t = am->mTangents[v];
                const aiVector3D& b = am->mBitangents[v];
                const aiVector3D& N = am->HasNormals() ? am->mNormals[v] : aiVector3D(0, 1, 0);
                const aiVector3D c(N.y * t.z - N.z * t.y, N.z * t.x - N.x * t.z, N.x * t.y - N.y * t.x);
                // Handedness in the mesh's own space; the back end flips it for a
                // mirroring transform. (The old skinned path forced +1.)
                m.tangents[v] = {t.x, t.y, t.z, (c.x * b.x + c.y * b.y + c.z * b.z) < 0 ? -1.0f : 1.0f};
            }
        }
        if (am->HasTextureCoords(0)) {
            m.uv0.resize(n);
            for (unsigned v = 0; v < n; ++v) m.uv0[v] = {am->mTextureCoords[0][v].x, am->mTextureCoords[0][v].y};
        }
        m.indices.reserve((size_t)am->mNumFaces * 3);
        for (unsigned f = 0; f < am->mNumFaces; ++f)
            if (am->mFaces[f].mNumIndices == 3)
                for (int k = 0; k < 3; ++k) m.indices.push_back(am->mFaces[f].mIndices[k]);
        m.submeshes = {{0, (uint32_t)m.indices.size(), am->mMaterialIndex}};

        if (am->mNumBones > 0 && skel) {
            const auto bw = imp::assimp::extractBoneWeights(am, *skel);
            m.joints.resize(n); m.weights.resize(n);
            for (unsigned v = 0; v < n; ++v) {
                float w[4]; std::memcpy(w, bw[v].weights, sizeof w);
                if (w[0] + w[1] + w[2] + w[3] < 1e-6f) {  // influenced by nothing: follow the root bone
                    w[0] = 1; ++unweightedVerts;           // (it used to collapse to the origin)
                }
                m.joints[v]  = {bw[v].joints[0], bw[v].joints[1], bw[v].joints[2], bw[v].joints[3]};
                m.weights[v] = {w[0], w[1], w[2], w[3]};
            }
        }
        if (am->mNumAnimMeshes) ++morphMeshes;
        if (am->HasVertexColors(0)) ++colouredMeshes;
        if (am->HasTextureCoords(1)) ++extraUvMeshes;
        out.meshes.push_back(std::move(m));
    }

    // Depth-first pre-order from Assimp's root, which becomes node 0.
    // An explicit stack, not recursion: the depth is the file's, and a deep
    // one overflowed a 512 KB thread stack (SIGBUS, uncatchable). Children are
    // pushed in reverse, so the order is the recursive pre-order exactly.
    void node(const aiNode* top, int32_t topParent) {
        std::vector<std::pair<const aiNode*, int32_t>> stack{{top, topParent}};
        while (!stack.empty()) {
            const auto [an, parent] = stack.back();
            stack.pop_back();
            Node nd;
            nd.name   = an->mName.length ? an->mName.C_Str() : "node " + std::to_string(out.nodes.size());
            nd.parent = parent;
            nd.local  = toFloat4x4(an->mTransformation);
            for (unsigned i = 0; i < an->mNumMeshes; ++i)
                if (!out.meshes[an->mMeshes[i]].indices.empty()) nd.meshes.push_back(an->mMeshes[i]);
            const int32_t self = (int32_t)out.nodes.size();
            out.nodes.push_back(std::move(nd));
            for (unsigned c = an->mNumChildren; c-- > 0;) stack.push_back({an->mChildren[c], self});
        }
    }
};

// The engine's anim::Skeleton, as the old paths built it, carried into the
// import format unchanged: bones plus their ancestors, rest pose from the node
// hierarchy, inverse bind as the file authored it (Bone's comment).
Skeleton fromAnim(const ::Skeleton& sk) {
    Skeleton out;
    for (const ::Bone& b : sk.bones) {
        Bone ib;
        ib.name = b.name;
        ib.parent = b.parentIndex;
        std::memcpy(ib.bindLocal.m, b.localBindMatrix, sizeof ib.bindLocal.m);
        std::memcpy(ib.inverseBind.m, b.inverseBindMatrix, sizeof ib.inverseBind.m);
        out.bones.push_back(std::move(ib));
    }
    return out;
}

// An animation-only file (a Mixamo clip) has no weighted meshes to find bones
// by, so its skeleton is every animated node plus its ancestors, at rest. That
// is what WO-016's clip cooker consumes; the mesh back end skips such a scene.
Skeleton animatedNodes(const aiScene& sc) {
    std::set<std::string> animated;
    for (unsigned a = 0; a < sc.mNumAnimations; ++a)
        for (unsigned c = 0; c < sc.mAnimations[a]->mNumChannels; ++c)
            animated.insert(sc.mAnimations[a]->mChannels[c]->mNodeName.C_Str());
    std::set<const aiNode*> keep;
    std::vector<const aiNode*> stack{sc.mRootNode};
    while (!stack.empty()) {
        const aiNode* n = stack.back(); stack.pop_back();
        if (animated.count(n->mName.C_Str()))
            for (const aiNode* p = n; p && keep.insert(p).second; p = p->mParent) {}   // stop at a kept one: O(n), not O(n x depth)
        for (unsigned c = 0; c < n->mNumChildren; ++c) stack.push_back(n->mChildren[c]);
    }
    Skeleton out;
    std::map<const aiNode*, int32_t> index;
    std::vector<std::pair<const aiNode*, aiMatrix4x4>> dfs{{sc.mRootNode, aiMatrix4x4()}};
    while (!dfs.empty()) {                                   // pre-order: parents first
        auto [n, parentWorld] = dfs.back(); dfs.pop_back();
        const aiMatrix4x4 world = parentWorld * n->mTransformation;
        if (keep.count(n)) {
            Bone b;
            b.name = n->mName.C_Str();
            b.parent = n->mParent && index.count(n->mParent) ? index[n->mParent] : -1;
            b.bindLocal = toFloat4x4(n->mTransformation);
            aiMatrix4x4 inv = world; inv.Inverse();
            b.inverseBind = toFloat4x4(inv);
            index[n] = (int32_t)out.bones.size();
            out.bones.push_back(std::move(b));
        }
        for (unsigned c = n->mNumChildren; c-- > 0;) dfs.push_back({n->mChildren[c], world});
    }
    return out;
}

}  // namespace

// Assimp's readers recurse once per level of the file's node tree, and so does
// aiNode's destructor. A 10,000-deep COLLADA overflowed even an 8 MB stack, and
// on a 512 KB one (a macOS secondary thread: the job pool, where the uncooked
// preview imports) far less does. That recursion is Assimp's, not ours to
// rewrite, so the whole import, destruction included, runs on a stack sized
// for it: a reservation, committed only as touched. A file deeper still kills
// the process it runs in, which for a cook is the isolated engine_cook_worker.
constexpr size_t kAssimpStack = 256u << 20;

ImportResult AssimpFrontend::importScene(const std::filesystem::path& source, const ImportOptions& options) const {
    std::optional<ImportResult> result;
    const bool ran = engine::threads::runWithStack(kAssimpStack, [&] { result.emplace(importOnThisStack(source, options)); });
    if (!ran || !result)
        return ImportError{ImportError::Kind::Unreadable,
                           "Assimp front end could not start its import thread for " + source.string()};
    return std::move(*result);
}

ImportResult AssimpFrontend::importOnThisStack(const std::filesystem::path& source, const ImportOptions&) const {
    return guardedImport(source, "Assimp", [&]() -> ImportResult {
        const std::string src = source.string();
        AssimpGatePass gate;                                     // one resident import per permit

        Assimp::Importer imp;                                    // per call: reentrant
        imp.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
        imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        // Without this, Assimp INVENTS a mesh for any file that has none: a
        // "skeleton mesh" drawing the node hierarchy as geometry. An empty file would
        // then import as one mesh, and an animation-only COLLADA as a stick figure
        // instead of a clip. The old cook paths never set it (WO-013).
        imp.SetPropertyBool(AI_CONFIG_IMPORT_NO_SKELETON_MESHES, true);
        const aiScene* sc = imp.ReadFile(src, kImportFlags);
        if (!sc || !sc->mRootNode) {
            const char* why = imp.GetErrorString();
            return ImportError{ImportError::Kind::Unreadable,
                               "Assimp could not read " + src + ((why && *why) ? std::string(": ") + why : "")};
        }
        // Nothing in it is Empty, the contract's answer, even though Assimp also
        // flags such a scene INCOMPLETE. An animation-only file is flagged INCOMPLETE
        // too, and is valid. Anything ELSE incomplete is broken.
        if (sc->mNumMeshes == 0 && sc->mNumAnimations == 0)
            return ImportError{ImportError::Kind::Empty, "nothing to import: " + src};
        const bool animationOnly = sc->mNumMeshes == 0 && sc->mNumAnimations > 0;
        if ((sc->mFlags & AI_SCENE_FLAGS_INCOMPLETE) && !animationOnly)
            return ImportError{ImportError::Kind::Unreadable, "Assimp read an incomplete scene (" +
                               std::to_string(sc->mNumMeshes) + " meshes, " + std::to_string(sc->mNumAnimations) +
                               " animations): " + src};

        ImportedScene out;
        out.source = src;
        Converter cv{*sc, source.parent_path(), out};

        bool anyBones = false;
        for (unsigned i = 0; i < sc->mNumMeshes; ++i) anyBones |= sc->mMeshes[i]->mNumBones > 0;
        ::Skeleton animSkel;
        if (anyBones) {
            animSkel = imp::assimp::extractSkeleton(sc);
            if (animSkel.boneCount() > 0) out.skeleton = fromAnim(animSkel);
        } else if (animationOnly) {
            out.skeleton = animatedNodes(*sc);
        }
        if (out.skeleton && out.skeleton->bones.size() > kMaxSceneBones) {   // joints would wrap
            out.dropped.push_back({Dropped::Kind::Skin, Dropped::Effect::Wrong, 1,
                                   std::to_string(out.skeleton->bones.size()) + " bones: an imported skeleton holds at most " +
                                   std::to_string(kMaxSceneBones)});
            out.skeleton.reset();
        }

        cv.materials();
        for (unsigned i = 0; i < sc->mNumMeshes; ++i)
            cv.mesh(sc->mMeshes[i], out.skeleton && anyBones ? &animSkel : nullptr);
        cv.node(sc->mRootNode, -1);
        // An aiMesh with no triangles left (SBP_REMOVE) is on no node; drop it from
        // the list rather than leave a mesh nothing places. Indices shift, so remap.
        {
            std::vector<int32_t> remap(out.meshes.size(), -1);
            std::vector<Mesh> kept;
            for (size_t i = 0; i < out.meshes.size(); ++i)
                if (!out.meshes[i].indices.empty()) { remap[i] = (int32_t)kept.size(); kept.push_back(std::move(out.meshes[i])); }
            out.meshes = std::move(kept);
            for (Node& n : out.nodes) for (uint32_t& m : n.meshes) m = (uint32_t)remap[m];
        }

        // ── Clips: the skinned skeleton's, or an animation-only file's ───────────
        if (out.skeleton) {
            std::set<std::string> bones;
            for (const Bone& b : out.skeleton->bones) bones.insert(b.name);
            const std::string stem = source.stem().string();
            uint32_t strayChannels = 0;
            for (unsigned a = 0; a < sc->mNumAnimations; ++a) {
                const aiAnimation* an = sc->mAnimations[a];
                const double tps = an->mTicksPerSecond > 0.0 ? an->mTicksPerSecond : 24.0;
                Clip c;
                c.name = clipDisplayName(an->mName.length ? an->mName.C_Str() : "", stem, a, sc->mNumAnimations);
                c.duration = std::max((float)(an->mDuration / tps), 1e-4f);
                auto t = [&](double ticks) { return std::clamp((float)(ticks / tps), 0.0f, c.duration); };
                for (unsigned ch = 0; ch < an->mNumChannels; ++ch) {
                    const aiNodeAnim* na = an->mChannels[ch];
                    if (!bones.count(na->mNodeName.C_Str())) { ++strayChannels; continue; }
                    Track tr; tr.bone = na->mNodeName.C_Str();
                    for (unsigned k = 0; k < na->mNumPositionKeys; ++k) {
                        const auto& kv = na->mPositionKeys[k];
                        tr.translation.push_back({t(kv.mTime), {kv.mValue.x, kv.mValue.y, kv.mValue.z}});
                    }
                    for (unsigned k = 0; k < na->mNumRotationKeys; ++k) {   // source convention: no conjugation
                        const auto& kv = na->mRotationKeys[k];
                        Quat q{kv.mValue.x, kv.mValue.y, kv.mValue.z, kv.mValue.w};
                        normalizeRotation(q);                             // a bad key stays bad: checkScene names it
                        tr.rotation.push_back({t(kv.mTime), q});
                    }
                    for (unsigned k = 0; k < na->mNumScalingKeys; ++k) {
                        const auto& kv = na->mScalingKeys[k];
                        tr.scale.push_back({t(kv.mTime), {kv.mValue.x, kv.mValue.y, kv.mValue.z}});
                    }
                    c.tracks.push_back(std::move(tr));
                }
                out.clips.push_back(std::move(c));
            }
            if (strayChannels)
                out.dropped.push_back({Dropped::Kind::Animation, Dropped::Effect::Less, strayChannels,
                                       std::to_string(strayChannels) + " animation channel(s) on nodes that are not bones"});
        } else if (sc->mNumAnimations > 0) {
            out.dropped.push_back({Dropped::Kind::Animation, Dropped::Effect::Less, sc->mNumAnimations,
                                   std::to_string(sc->mNumAnimations) + " node animation(s) on a static model"});
        }

        // ── Everything else read but not carried ────────────────────────────────
        auto less = [&](Dropped::Kind k, uint32_t n, std::string what) {
            if (n) out.dropped.push_back({k, Dropped::Effect::Less, n, std::move(what)});
        };
        less(Dropped::Kind::MorphTargets,  cv.morphMeshes,    std::to_string(cv.morphMeshes) + " mesh(es) with morph targets");
        less(Dropped::Kind::VertexColours, cv.colouredMeshes, std::to_string(cv.colouredMeshes) + " mesh(es) with vertex colours");
        less(Dropped::Kind::ExtraUvSets,   cv.extraUvMeshes,  std::to_string(cv.extraUvMeshes) + " mesh(es) with a second UV set");
        less(Dropped::Kind::Camera, sc->mNumCameras, std::to_string(sc->mNumCameras) + " camera(s)");
        less(Dropped::Kind::Light,  sc->mNumLights,  std::to_string(sc->mNumLights) + " light(s)");
        for (const std::string& t : cv.droppedTextures) less(Dropped::Kind::Texture, 1, "texture " + t);
        if (cv.unweightedVerts)
            less(Dropped::Kind::Skin, cv.unweightedVerts,
                 std::to_string(cv.unweightedVerts) + " skinned vertex(es) had no bone influence; bound to the root bone");

        if (out.meshes.empty() && out.clips.empty())
            return ImportError{ImportError::Kind::Empty, "nothing to import (no triangles, no clips): " + src};
        return out;
    });
}

}  // namespace imp
