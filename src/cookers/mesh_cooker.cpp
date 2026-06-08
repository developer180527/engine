#include "cookers/mesh_cooker.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <assimp/matrix4x4.h>
#include <assimp/matrix3x3.h>
#include <cstring>
#include <cstdio>
#include <cfloat>
#include <cmath>
#include <algorithm>
#include <filesystem>

// MeshCooker::cook is a pure function: it owns its Assimp importer and writes a
// single unique output file, sharing no mutable state. That — and nothing more
// exotic — is what lets the cook pipeline run many cooks concurrently.

using namespace assetlib;

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

static void emitMesh(const aiMesh* mesh, const aiMatrix4x4& world, EmitState& st) {
    if (!mesh->HasPositions()) return;

    const aiMatrix3x3 w3(world);                 // linear part — directions
    aiMatrix3x3 nm(world);                        // normals — inverse-transpose,
    float det = nm.Determinant();                 // guarded against degenerate scale
    if (std::fabs(det) > 1e-12f) { nm.Inverse(); nm.Transpose(); }
    else                         { nm = aiMatrix3x3(); }

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

CookResult MeshCooker::cook(const CookContext& ctx) {
    Assimp::Importer imp;   // isolated per cook — safe inside the worker pool
    imp.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
                           aiPrimitiveType_POINT | aiPrimitiveType_LINE);
    const aiScene* scene = imp.ReadFile(ctx.sourcePath.string(), kImportFlags);
    if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE))
        return {.success = false, .error = std::string("Assimp: ") + imp.GetErrorString()};

    // Skinned meshes (bones + animation) cannot be cooked yet — the cook format
    // doesn't store SkinnedVertex, skeleton, or animation clips. The runtime
    // Assimp path in async_loader.cpp handles those correctly. Skip cooking so
    // the binary fast path falls through to Assimp for these files.
    {
        bool hasBones = false;
        for (unsigned m = 0; m < scene->mNumMeshes && !hasBones; ++m)
            if (scene->mMeshes[m]->mNumBones > 0) hasBones = true;
        if (hasBones) {
            std::printf("[MeshCooker] %s — skinned mesh, skipping cook (runtime Assimp path)\n",
                        ctx.sourcePath.filename().string().c_str());
            return {.success = false, .error = "Skinned mesh — runtime Assimp path handles bones"};
        }
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

    if (!saveMesh(asset, ctx.outputPath))
        return {.success = false, .error = "saveMesh failed"};

    std::printf("[MeshCooker] %s -> verts=%u idx=%u submeshes=%u mats=%u\n",
        ctx.sourcePath.filename().string().c_str(),
        asset.header.vertexCount, asset.header.indexCount,
        asset.header.submeshCount, asset.header.materialCount);
    return {.success = true};
}
