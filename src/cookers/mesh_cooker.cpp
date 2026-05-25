#include "cookers/mesh_cooker.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <cstring>
#include <cstdio>
#include <cfloat>
#include <algorithm>
#include <filesystem>

using namespace assetlib;

static constexpr unsigned kImportFlags =
    aiProcess_Triangulate | aiProcess_GenSmoothNormals |
    aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices |
    aiProcess_ImproveCacheLocality | aiProcess_FlipUVs;

// Cook format matches runtime Vertex exactly: Position(12)+Normal(12)+UV(8)=32 bytes
// Tangent added when normal mapping milestone lands (bumps kVersion → re-cook auto)
// Position(12)+Normal(12)+Tangent(16)+UV(8) = 48 bytes — matches runtime Vertex exactly
static constexpr uint32_t kCookFlags = VF_POSITION|VF_NORMAL|VF_TANGENT|VF_UV0;

static void pushF(std::vector<uint8_t>& b, float v) {
    uint8_t x[4]; std::memcpy(x,&v,4); b.insert(b.end(),x,x+4);
}
static void pushF3(std::vector<uint8_t>& b,float x,float y,float z){pushF(b,x);pushF(b,y);pushF(b,z);}
static void pushF2(std::vector<uint8_t>& b,float x,float y){pushF(b,x);pushF(b,y);}
static void pushF4(std::vector<uint8_t>& b,float x,float y,float z,float w){pushF(b,x);pushF(b,y);pushF(b,z);pushF(b,w);}

CookResult MeshCooker::cook(const CookContext& ctx) {
    Assimp::Importer imp;
    const aiScene* scene = imp.ReadFile(ctx.sourcePath.string(), kImportFlags);
    if (!scene||!scene->mRootNode||(scene->mFlags&AI_SCENE_FLAGS_INCOMPLETE))
        return {.success=false,.error=std::string("Assimp: ")+imp.GetErrorString()};

    MeshAsset asset;
    asset.header.magic        = 0x4D455348;
    asset.header.version      = 1;
    asset.header.vertexFlags  = kCookFlags;
    asset.header.vertexStride = vertexStride(kCookFlags);
    // Pre-count vertices to decide 16 vs 32-bit index stride
    uint32_t totalVerts = 0;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m)
        if (scene->mMeshes[m]->HasPositions())
            totalVerts += scene->mMeshes[m]->mNumVertices;
    const bool use16 = (totalVerts <= 65535);
    asset.header.indexStride  = use16 ? 2 : 4;
    asset.header.submeshCount = scene->mNumMeshes;
    std::memcpy(asset.header.uuid, ctx.uuid.bytes.data(), 16);

    float bMin[3]={FLT_MAX,FLT_MAX,FLT_MAX};
    float bMax[3]={-FLT_MAX,-FLT_MAX,-FLT_MAX};
    uint32_t idxOffset=0;
    uint32_t vertexBase=0;

    for (unsigned m=0; m<scene->mNumMeshes; ++m) {
        aiMesh* mesh = scene->mMeshes[m];
        if (!mesh->HasPositions()) continue;
        bool hasN=mesh->HasNormals();
        bool hasT=mesh->HasTangentsAndBitangents();
        bool hasUV=mesh->HasTextureCoords(0);

        for (unsigned v=0; v<mesh->mNumVertices; ++v) {
            auto& p=mesh->mVertices[v];
            pushF3(asset.vertexData,p.x,p.y,p.z);
            bMin[0]=std::min(bMin[0],p.x); bMin[1]=std::min(bMin[1],p.y); bMin[2]=std::min(bMin[2],p.z);
            bMax[0]=std::max(bMax[0],p.x); bMax[1]=std::max(bMax[1],p.y); bMax[2]=std::max(bMax[2],p.z);
            if (hasN){auto& n=mesh->mNormals[v]; pushF3(asset.vertexData,n.x,n.y,n.z);}
            else pushF3(asset.vertexData,0,1,0);
            if (hasT) {
                auto& t=mesh->mTangents[v]; auto& bt=mesh->mBitangents[v];
                aiVector3D n2=hasN?mesh->mNormals[v]:aiVector3D(0,1,0);
                float cx=n2.y*t.z-n2.z*t.y, cy=n2.z*t.x-n2.x*t.z, cz=n2.x*t.y-n2.y*t.x;
                float sign=(cx*bt.x+cy*bt.y+cz*bt.z)<0?-1.f:1.f;
                pushF4(asset.vertexData,t.x,t.y,t.z,sign);
            } else pushF4(asset.vertexData,1,0,0,1);
            if (hasUV){auto& uv=mesh->mTextureCoords[0][v]; pushF2(asset.vertexData,uv.x,uv.y);}
            else pushF2(asset.vertexData,0,0);
        }

        MeshSubmesh sub;
        sub.indexOffset=idxOffset;
        sub.indexCount=mesh->mNumFaces*3;
        for (unsigned f=0; f<mesh->mNumFaces; ++f) {
            for (unsigned i=0; i<mesh->mFaces[f].mNumIndices; ++i) {
                uint32_t idx = mesh->mFaces[f].mIndices[i] + vertexBase;
                if (use16) {
                    auto i16 = static_cast<uint16_t>(idx);
                    uint8_t b[2]; std::memcpy(b, &i16, 2);
                    asset.indexData.insert(asset.indexData.end(), b, b+2);
                } else {
                    uint8_t b[4]; std::memcpy(b, &idx, 4);
                    asset.indexData.insert(asset.indexData.end(), b, b+4);
                }
            }
        }
        idxOffset   += sub.indexCount;
        vertexBase  += mesh->mNumVertices; // next mesh's indices start here
        asset.submeshes.push_back(sub);
    }

    asset.header.vertexCount=static_cast<uint32_t>(asset.vertexData.size()/asset.header.vertexStride);
    asset.header.indexCount = static_cast<uint32_t>(asset.indexData.size() / asset.header.indexStride);
    for (int i=0;i<3;++i){asset.header.boundsMin[i]=bMin[i]; asset.header.boundsMax[i]=bMax[i];}

    // ── Material section ──────────────────────────────────────────────
    for (uint32_t m = 0; m < scene->mNumMaterials; ++m) {
        const aiMaterial* aiMat = scene->mMaterials[m];
        assetlib::CookedMaterial cm;
        aiColor4D col{1,1,1,1};
        if (AI_SUCCESS == aiGetMaterialColor(aiMat, AI_MATKEY_COLOR_DIFFUSE, &col)) {
            cm.baseColorFactor[0]=col.r; cm.baseColorFactor[1]=col.g;
            cm.baseColorFactor[2]=col.b; cm.baseColorFactor[3]=col.a;
        }
        float rough=0.7f, metal=0.0f;
        aiGetMaterialFloat(aiMat, AI_MATKEY_ROUGHNESS_FACTOR, &rough);
        aiGetMaterialFloat(aiMat, AI_MATKEY_METALLIC_FACTOR,  &metal);
        cm.roughness = rough; cm.metallic = metal;
        aiString tp;
        if (AI_SUCCESS == aiMat->GetTexture(aiTextureType_DIFFUSE,    0, &tp) ||
            AI_SUCCESS == aiMat->GetTexture(aiTextureType_BASE_COLOR, 0, &tp)) {
            auto fn = std::filesystem::path(tp.C_Str()).filename().string();
            std::strncpy(cm.baseColorPath, fn.c_str(), 511);
            cm.flags |= assetlib::kMatFlag_HasBaseColor;
        }
        aiString np;
        if (AI_SUCCESS == aiMat->GetTexture(aiTextureType_NORMALS, 0, &np) ||
            AI_SUCCESS == aiMat->GetTexture(aiTextureType_HEIGHT,  0, &np)) {
            auto fn = std::filesystem::path(np.C_Str()).filename().string();
            std::strncpy(cm.normalMapPath, fn.c_str(), 511);
            cm.flags |= assetlib::kMatFlag_HasNormalMap;
        }
        asset.materials.push_back(cm);
    }
    asset.header.materialCount = (uint32_t)asset.materials.size();
    if (!saveMesh(asset,ctx.outputPath))
        return {.success=false,.error="saveMesh failed"};

    std::printf("[MeshCooker] %s -> verts=%u idx=%u submeshes=%u\n",
        ctx.sourcePath.filename().string().c_str(),
        asset.header.vertexCount, asset.header.indexCount, asset.header.submeshCount);
    return {.success=true};
}
