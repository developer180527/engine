#include "engine/async_loader.h"
#include "engine/logger.h"
#include "render/mesh.h"
#include "render/texture.h"
#include "render/material.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>

#include <stb_image.h>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cstdio>

// -----------------------------------------------------------------------
// Worker thread helpers — zero main-thread calls, bgfx::copy() IS
// thread-safe (uses bgfx's internal allocator which wraps malloc).
// All memcpy happens here, so drainOne() on the main thread is instant.
// -----------------------------------------------------------------------

static TextureGPUData loadTextureGPU(const aiScene*   scene,
                                      const char*      rawPath,
                                      const std::filesystem::path& dir,
                                      const std::string& baseName) {
    TextureGPUData out;
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = nullptr;

    auto tryLoad = [&](const std::string& p) {
        if (!px) px = stbi_load(p.c_str(), &w, &h, &ch, 4);
    };

    if (rawPath && rawPath[0] == '*') {
        // Embedded texture (FBX)
        int idx = std::atoi(rawPath + 1);
        if (idx >= 0 && (uint32_t)idx < scene->mNumTextures) {
            const aiTexture* t = scene->mTextures[idx];
            if (t->mHeight == 0) {
                px = stbi_load_from_memory(
                    reinterpret_cast<const stbi_uc*>(t->pcData),
                    (int)t->mWidth, &w, &h, &ch, 4);
            } else {
                w = (int)t->mWidth; h = (int)t->mHeight;
                px = (stbi_uc*)malloc((size_t)(w * h * 4));
                for (int p = 0; p < w * h; ++p) {
                    px[p*4+0] = t->pcData[p].r; px[p*4+1] = t->pcData[p].g;
                    px[p*4+2] = t->pcData[p].b; px[p*4+3] = t->pcData[p].a;
                }
            }
        }
    } else if (rawPath && rawPath[0] != '\0') {
        tryLoad((dir / rawPath).string());
        if (!px) {
            std::string fname = std::filesystem::path(rawPath).filename().string();
            for (auto sub : {"", "textures/", "Textures/", "tex/"})
                tryLoad((dir / sub / fname).string());
        }
    }

    // Naming-convention discovery (_COL, _Albedo, _BaseColor, _Diffuse)
    if (!px && !baseName.empty()) {
        static const char* kSuf[] = {"_COL","_Albedo","_BaseColor","_Diffuse","_D",nullptr};
        static const char* kExt[] = {".jpg",".png",".jpeg",".tga",nullptr};
        std::string lb = baseName;
        for (auto& c : lb) c = (char)std::tolower(c);
        try {
            for (const auto& de : std::filesystem::directory_iterator(dir)) {
                if (!de.is_regular_file()) continue;
                std::string fn = de.path().filename().string();
                std::string lf = fn; for (auto& c : lf) c=(char)std::tolower(c);
                if (lf.find(lb) != 0) continue;
                bool goodExt = false;
                for (int i = 0; kExt[i]; ++i)
                    if (de.path().extension()==kExt[i]) { goodExt=true; break; }
                if (!goodExt) continue;
                for (int i = 0; kSuf[i]; ++i) {
                    std::string ls=kSuf[i]; for(auto& c:ls) c=(char)std::tolower(c);
                    if (lf.find(ls) != std::string::npos) {
                        tryLoad(de.path().string());
                        if (px) { LOG_SUCCESS("Assimp","Texture discovered: %s",fn.c_str()); break; }
                    }
                }
                if (px) break;
            }
        } catch (...) {}
    }

    if (!px || w == 0 || h == 0) return out;

    // bgfx::copy() on the worker thread — this is the 64MB+ memcpy.
    // Keeps the main thread completely free during texture upload prep.
    out.mem = bgfx::copy(px, (uint32_t)(w * h * 4));
    out.w   = (uint16_t)w;
    out.h   = (uint16_t)h;
    stbi_image_free(px);
    return out;
}

// -----------------------------------------------------------------------
// processFile — runs entirely on worker thread.
// Assimp parse + stb_image decode + bgfx::copy (big memcpy).
// By the time LoadedAsset reaches the main thread, all data is in
// bgfx's internal pool — handle creation is the only main-thread work.
// -----------------------------------------------------------------------
LoadedAsset AsyncLoader::processFile(const std::string& path,
                                      const std::string& name) {
    LoadedAsset out;
    out.path = path; out.name = name;

    Assimp::Importer imp;
    const aiScene* scene = imp.ReadFile(path,
        aiProcess_Triangulate        |
        aiProcess_GenSmoothNormals   |
        aiProcess_FlipUVs            |
        aiProcess_JoinIdenticalVertices |
        aiProcess_SortByPType);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        out.error = std::string("Assimp: ") + imp.GetErrorString();
        LOG_ERROR("Assimp", "Failed: %s", out.error.c_str());
        return out;
    }

    LOG_INFO("Assimp", "Parsed %s — %u mesh(es) %u mat(s)",
             name.c_str(), scene->mNumMeshes, scene->mNumMaterials);

    const auto dir = std::filesystem::path(path).parent_path();
    const std::string bn = std::filesystem::path(path).stem().string();

    // Materials + textures (stb_image + bgfx::copy on worker)
    out.materials.resize(scene->mNumMaterials);
    for (uint32_t i = 0; i < scene->mNumMaterials; ++i) {
        const aiMaterial* ai = scene->mMaterials[i];
        MaterialGPUData& mg = out.materials[i];
        aiColor4D col;
        if (AI_SUCCESS == aiGetMaterialColor(ai, AI_MATKEY_COLOR_DIFFUSE, &col)) {
            mg.baseColorFactor[0]=col.r; mg.baseColorFactor[1]=col.g;
            mg.baseColorFactor[2]=col.b; mg.baseColorFactor[3]=col.a;
        }
        aiString tp;
        if (AI_SUCCESS == ai->GetTexture(aiTextureType_DIFFUSE,    0, &tp) ||
            AI_SUCCESS == ai->GetTexture(aiTextureType_BASE_COLOR, 0, &tp))
            mg.baseColorTexture = loadTextureGPU(scene, tp.C_Str(), dir, bn);
        if (!mg.baseColorTexture.mem)
            mg.baseColorTexture = loadTextureGPU(scene, nullptr, dir, bn);
    }

    // Meshes (vertex/index data + bgfx::copy on worker)
    for (uint32_t i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* am = scene->mMeshes[i];
        if (!(am->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;

        MeshGPUData mg;
        mg.matIndex = am->mMaterialIndex;

        // Build vertex array
        std::vector<Vertex> verts;
        verts.reserve(am->mNumVertices);
        for (uint32_t v = 0; v < am->mNumVertices; ++v) {
            Vertex vtx{};
            vtx.position[0] = am->mVertices[v].x;
            vtx.position[1] = am->mVertices[v].y;
            vtx.position[2] = am->mVertices[v].z;
            if (am->HasNormals()) {
                vtx.normal[0] = am->mNormals[v].x;
                vtx.normal[1] = am->mNormals[v].y;
                vtx.normal[2] = am->mNormals[v].z;
            } else { vtx.normal[1] = 1.0f; }
            if (am->mTextureCoords[0]) {
                vtx.uv[0] = am->mTextureCoords[0][v].x;
                vtx.uv[1] = am->mTextureCoords[0][v].y;
            }
            verts.push_back(vtx);
        }

        // bgfx::copy for vertex data — big memcpy on worker, not main thread
        mg.vertexMem = bgfx::copy(verts.data(),
                                   (uint32_t)(verts.size() * sizeof(Vertex)));

        // Build index array + bgfx::copy on worker
        mg.use32 = verts.size() > 65535;
        if (mg.use32) {
            std::vector<uint32_t> idx;
            idx.reserve(am->mNumFaces * 3);
            for (uint32_t f = 0; f < am->mNumFaces; ++f)
                for (uint32_t k = 0; k < am->mFaces[f].mNumIndices; ++k)
                    idx.push_back(am->mFaces[f].mIndices[k]);
            mg.indexCount = (uint32_t)idx.size();
            mg.indexMem   = bgfx::copy(idx.data(), mg.indexCount * 4);
        } else {
            std::vector<uint16_t> idx;
            idx.reserve(am->mNumFaces * 3);
            for (uint32_t f = 0; f < am->mNumFaces; ++f)
                for (uint32_t k = 0; k < am->mFaces[f].mNumIndices; ++k)
                    idx.push_back((uint16_t)am->mFaces[f].mIndices[k]);
            mg.indexCount = (uint32_t)idx.size();
            mg.indexMem   = bgfx::copy(idx.data(), mg.indexCount * 2);
        }

        // AABB
        if (am->mNumVertices > 0) {
            mg.hasBounds = true;
            mg.boundsMin[0] = mg.boundsMax[0] = am->mVertices[0].x;
            mg.boundsMin[1] = mg.boundsMax[1] = am->mVertices[0].y;
            mg.boundsMin[2] = mg.boundsMax[2] = am->mVertices[0].z;
            for (uint32_t v = 1; v < am->mNumVertices; ++v) {
                for (int k = 0; k < 3; ++k) {
                    float val = (&am->mVertices[v].x)[k];
                    mg.boundsMin[k] = std::min(mg.boundsMin[k], val);
                    mg.boundsMax[k] = std::max(mg.boundsMax[k], val);
                }
            }
        }

        out.meshes.push_back(std::move(mg));
    }

    out.success = !out.meshes.empty();
    if (!out.success) out.error = "No triangle meshes found";
    return out;
}

// -----------------------------------------------------------------------
// Worker thread lifecycle
// -----------------------------------------------------------------------
AsyncLoader::AsyncLoader() {
    m_worker = std::thread([this] { workerLoop(); });
}

AsyncLoader::~AsyncLoader() {
    m_running = false;
    m_pendingCV.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void AsyncLoader::load(const std::string& path, const std::string& name, OnLoaded cb) {
    {
        std::lock_guard<std::mutex> lk(m_pendingMtx);
        if (m_inFlight.count(path)) return;
        m_inFlight.insert(path);
        m_pending.push({path, name, std::move(cb)});
    }
    m_pendingCV.notify_one();
}

bool AsyncLoader::isLoading(const std::string& path) const {
    std::lock_guard<std::mutex> lk(m_pendingMtx);
    return m_inFlight.count(path) > 0;
}

bool AsyncLoader::isLoaded(const std::string& path) const {
    std::lock_guard<std::mutex> lk(m_loadedMtx);
    return m_loaded.count(path) > 0;
}

int AsyncLoader::pendingCount() const {
    std::lock_guard<std::mutex> lk(m_pendingMtx);
    return (int)(m_pending.size() + m_inFlight.size());
}

void AsyncLoader::workerLoop() {
    while (m_running) {
        LoadRequest req;
        {
            std::unique_lock<std::mutex> lk(m_pendingMtx);
            m_pendingCV.wait(lk, [this] {
                return !m_pending.empty() || !m_running;
            });
            if (!m_running && m_pending.empty()) break;
            req = std::move(m_pending.front());
            m_pending.pop();
        }

        LOG_INFO("Loader", "Worker started: %s", req.name.c_str());
        LoadedAsset asset = processFile(req.path, req.name);

        if (asset.success)
            LOG_SUCCESS("Loader", "Worker done: %s — ready to upload",
                        req.name.c_str());
        else
            LOG_ERROR("Loader", "Worker failed: %s", asset.error.c_str());

        {
            std::lock_guard<std::mutex> lk(m_readyMtx);
            m_ready.push({std::move(asset), std::move(req.cb)});
        }
        {
            std::lock_guard<std::mutex> lk(m_pendingMtx);
            m_inFlight.erase(req.path);
        }
    }
}

// -----------------------------------------------------------------------
// drainOne — main thread only.
// ALL data is pre-copied. This function only creates bgfx handles and
// spawns the entity. Typical cost: <1ms even for complex assets.
// -----------------------------------------------------------------------
bool AsyncLoader::drainOne(AssetStorage& storage) {
    UploadRequest req;
    {
        std::lock_guard<std::mutex> lk(m_readyMtx);
        if (m_ready.empty()) return false;
        req = std::move(m_ready.front());
        m_ready.pop();
    }

    if (!req.asset.success) {
        LOG_ERROR("Loader", "Upload skipped (parse failed): %s",
                  req.asset.name.c_str());
        if (req.cb) req.cb(MeshHandle{}, req.asset.name);
        return true;
    }

    // Upload materials (handle creation only — data already in bgfx pool)
    std::vector<MaterialHandle> matHandles(req.asset.materials.size());
    for (size_t i = 0; i < req.asset.materials.size(); ++i) {
        const MaterialGPUData& mg = req.asset.materials[i];
        Material mat;
        std::memcpy(mat.baseColorFactor, mg.baseColorFactor, 16);
        if (mg.baseColorTexture.mem) {
            bgfx::TextureHandle th = bgfx::createTexture2D(
                mg.baseColorTexture.w, mg.baseColorTexture.h,
                false, 1, bgfx::TextureFormat::RGBA8,
                0, mg.baseColorTexture.mem); // mem already copied — instant
            if (bgfx::isValid(th)) {
                Texture tex; tex.handle = th;
                mat.baseColorTexture = storage.textures.addTexture(std::move(tex));
            }
        }
        matHandles[i] = storage.materials.addMaterial(std::move(mat));
    }

    // Upload meshes (handle creation only — data already in bgfx pool)
    MeshHandle firstHandle{};
    for (const MeshGPUData& mg : req.asset.meshes) {
        bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(
            mg.vertexMem, Vertex::layout()); // instant — no memcpy

        bgfx::IndexBufferHandle ibh = mg.use32
            ? bgfx::createIndexBuffer(mg.indexMem, BGFX_BUFFER_INDEX32)
            : bgfx::createIndexBuffer(mg.indexMem); // instant — no memcpy

        Mesh mesh(vbh, ibh, mg.indexCount);
        mesh.doubleSided = mg.doubleSided;
        if (mg.hasBounds) {
            mesh.boundsMin = {mg.boundsMin[0], mg.boundsMin[1], mg.boundsMin[2]};
            mesh.boundsMax = {mg.boundsMax[0], mg.boundsMax[1], mg.boundsMax[2]};
        }
        if (mg.matIndex < matHandles.size())
            mesh.material = matHandles[mg.matIndex];

        mesh.sourcePath = req.asset.path;
        MeshHandle h = storage.meshes.addMesh(std::move(mesh));
        if (!firstHandle.valid()) firstHandle = h;
    }

    LOG_SUCCESS("Loader", "Uploaded: %s (handle %u)",
                req.asset.name.c_str(), firstHandle.id);

    {
        std::lock_guard<std::mutex> lk(m_loadedMtx);
        m_loaded.insert(req.asset.path);
    }

    if (req.cb) req.cb(firstHandle, req.asset.name);
    return true;
}
