#include "io/assimp_importer.h"
#include "render/vertex.h"
#include "render/mesh.h"
#include "render/texture.h"
#include "render/material.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>

#include <stb_image.h>
#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

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

    std::string lowerBase = baseName;
    for (auto& c : lowerBase) c = (char)std::tolower(c);

    try {
        for (const auto& de : std::filesystem::directory_iterator(dir)) {
            if (!de.is_regular_file()) continue;
            std::string fname = de.path().filename().string();
            std::string lf = fname;
            for (auto& c : lf) c = (char)std::tolower(c);

            // Must start with the model base name
            if (lf.find(lowerBase) != 0) continue;

            // Must have an image extension
            bool goodExt = false;
            for (int i = 0; kExts[i]; ++i)
                if (de.path().extension() == kExts[i]) { goodExt = true; break; }
            if (!goodExt) continue;

            // Must contain a diffuse-indicating suffix
            for (int i = 0; kSuffixes[i]; ++i) {
                std::string ls = kSuffixes[i];
                for (auto& c : ls) c = (char)std::tolower(c);
                if (lf.find(ls) != std::string::npos) {
                    std::printf("[Assimp] Texture discovered: %s\n", fname.c_str());
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
                // Assimp stores as ARGB; reorder to RGBA for bgfx
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
                    std::printf("[Assimp] Texture resolved: %s\n",
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

    const bgfx::Memory* mem = bgfx::copy(pixels, (uint32_t)(w * h * 4));
    stbi_image_free(pixels);   // safe even for malloc'd — stbi_image_free wraps free()

    bgfx::TextureHandle th = bgfx::createTexture2D(
        (uint16_t)w, (uint16_t)h, false, 1,
        bgfx::TextureFormat::RGBA8, 0, mem);

    if (!bgfx::isValid(th)) return {};

    Texture tex;
    tex.handle = th;
    return storage.textures.addTexture(std::move(tex));
}

static MaterialHandle importMaterial(const aiScene*    scene,
                                     const aiMaterial* aiMat,
                                     const std::filesystem::path& dir,
                                     const std::string& baseName,
                                     AssetStorage&     storage) {
    Material mat;
    mat.baseColorFactor[0] = mat.baseColorFactor[1] =
    mat.baseColorFactor[2] = mat.baseColorFactor[3] = 1.0f;

    // Diffuse / base color factor
    aiColor4D col;
    if (AI_SUCCESS == aiGetMaterialColor(aiMat, AI_MATKEY_COLOR_DIFFUSE, &col)) {
        mat.baseColorFactor[0] = col.r;
        mat.baseColorFactor[1] = col.g;
        mat.baseColorFactor[2] = col.b;
        mat.baseColorFactor[3] = col.a;
    }

    // Diffuse / base color texture (try DIFFUSE then BASE_COLOR for PBR FBX)
    aiString texPath;
    if (AI_SUCCESS == aiMat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) ||
        AI_SUCCESS == aiMat->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath)) {
        mat.baseColorTexture = importTexture(scene, texPath.C_Str(), dir, storage);
    }
    // FBX exported without texture paths — discover by naming convention
    if (!mat.baseColorTexture.valid() && !baseName.empty()) {
        std::string discovered = discoverTexture(dir, baseName);
        if (!discovered.empty())
            mat.baseColorTexture = importTexture(scene, discovered.c_str(), dir, storage);
    }

    return storage.materials.addMaterial(std::move(mat));
}

static MeshHandle importMesh(const aiMesh*  aiM,
                              MaterialHandle matH,
                              AssetStorage&  storage) {
    // Build vertex buffer
    std::vector<Vertex> verts;
    verts.reserve(aiM->mNumVertices);

    for (uint32_t v = 0; v < aiM->mNumVertices; ++v) {
        Vertex vtx{};
        vtx.position[0] = aiM->mVertices[v].x;
        vtx.position[1] = aiM->mVertices[v].y;
        vtx.position[2] = aiM->mVertices[v].z;

        if (aiM->HasNormals()) {
            vtx.normal[0] = aiM->mNormals[v].x;
            vtx.normal[1] = aiM->mNormals[v].y;
            vtx.normal[2] = aiM->mNormals[v].z;
        } else {
            vtx.normal[1] = 1.0f; // world-up fallback
        }

        if (aiM->mTextureCoords[0]) {
            vtx.uv[0] = aiM->mTextureCoords[0][v].x;
            vtx.uv[1] = aiM->mTextureCoords[0][v].y;
        }
        verts.push_back(vtx);
    }

    // Build index buffer — use 32-bit when vertex count exceeds uint16 range
    const bool use32 = verts.size() > 65535;
    uint32_t   indexCount = 0;
    bgfx::IndexBufferHandle ibh = BGFX_INVALID_HANDLE;

    if (use32) {
        std::vector<uint32_t> idx;
        idx.reserve(aiM->mNumFaces * 3);
        for (uint32_t f = 0; f < aiM->mNumFaces; ++f)
            for (uint32_t i = 0; i < aiM->mFaces[f].mNumIndices; ++i)
                idx.push_back(aiM->mFaces[f].mIndices[i]);
        indexCount = (uint32_t)idx.size();
        ibh = bgfx::createIndexBuffer(
            bgfx::copy(idx.data(), indexCount * 4), BGFX_BUFFER_INDEX32);
    } else {
        std::vector<uint16_t> idx;
        idx.reserve(aiM->mNumFaces * 3);
        for (uint32_t f = 0; f < aiM->mNumFaces; ++f)
            for (uint32_t i = 0; i < aiM->mFaces[f].mNumIndices; ++i)
                idx.push_back((uint16_t)aiM->mFaces[f].mIndices[i]);
        indexCount = (uint32_t)idx.size();
        ibh = bgfx::createIndexBuffer(
            bgfx::copy(idx.data(), indexCount * 2));
    }

    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(
        bgfx::copy(verts.data(), (uint32_t)(verts.size() * sizeof(Vertex))),
        Vertex::layout());

    Mesh mesh(vbh, ibh, indexCount);
    mesh.material = matH;

    // AABB bounds for frustum culling
    if (aiM->mNumVertices > 0) {
        float mnX = aiM->mVertices[0].x, mxX = mnX;
        float mnY = aiM->mVertices[0].y, mxY = mnY;
        float mnZ = aiM->mVertices[0].z, mxZ = mnZ;
        for (uint32_t v = 1; v < aiM->mNumVertices; ++v) {
            mnX = std::min(mnX, aiM->mVertices[v].x);
            mxX = std::max(mxX, aiM->mVertices[v].x);
            mnY = std::min(mnY, aiM->mVertices[v].y);
            mxY = std::max(mxY, aiM->mVertices[v].y);
            mnZ = std::min(mnZ, aiM->mVertices[v].z);
            mxZ = std::max(mxZ, aiM->mVertices[v].z);
        }
        mesh.boundsMin = {mnX, mnY, mnZ};
        mesh.boundsMax = {mxX, mxY, mxZ};
    }

    return storage.meshes.addMesh(std::move(mesh));
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
        aiProcess_FlipUVs               | // match bgfx/Metal UV origin
        aiProcess_JoinIdenticalVertices | // deduplicate verts
        aiProcess_SortByPType           | // separate point/line/tri prims
        0);  // CalcTangentSpace removed — expensive, unused until normal mapping

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode)
        return MeshImportResult::fail(std::string("Assimp: ") + importer.GetErrorString());

    std::printf("[Assimp] %s  meshes=%u  mats=%u\n",
        path.c_str(), scene->mNumMeshes, scene->mNumMaterials);

    const auto dir = std::filesystem::path(path).parent_path();
    const std::string baseName = std::filesystem::path(path).stem().string();

    // Materials first so meshes can reference handles
    std::vector<MaterialHandle> matHandles(scene->mNumMaterials);
    for (uint32_t i = 0; i < scene->mNumMaterials; ++i)
        matHandles[i] = importMaterial(scene, scene->mMaterials[i], dir, baseName, storage);

    // Meshes
    MeshHandle first{};
    for (uint32_t i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* m = scene->mMeshes[i];
        if (!(m->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;
        MaterialHandle mh = (m->mMaterialIndex < matHandles.size())
                            ? matHandles[m->mMaterialIndex] : MaterialHandle{};
        MeshHandle h = importMesh(m, mh, storage);
        if (h.valid() && !first.valid()) first = h;
    }

    if (!first.valid())
        return MeshImportResult::fail("Assimp: no triangle meshes in file");

    return MeshImportResult::ok(first);
}
