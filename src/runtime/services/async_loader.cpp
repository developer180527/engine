#include "runtime/services/async_loader.h"
#include "core/logger.h"
#include "render/mesh.h"
#include "render/vertex.h"
#include "render/skinned_vertex.h"
#include "render/texture.h"
#include "render/material.h"
#include "animation/assimp_skeleton_loader.h"
#include "animation/ozz_bridge.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include "assetlib/mesh_asset.h"
#include <assimp/material.h>
#include <assetlib/mesh_asset.h>
#include <assetlib/texture_asset.h>
#include <filesystem>

#include <stb_image.h>
#include <filesystem>
#include <algorithm>
#include <cfloat>
#include <cstring>
#include <cstdio>

// Normalize path separators → forward slashes for consistent cache keys.
// On Windows, string() returns backslashes; generic_string() returns /.
// Using a single canonical form prevents cache misses on the same asset.
static std::string normalizeKey(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

// -----------------------------------------------------------------------
// Worker thread helpers — zero main-thread calls, bgfx::copy() IS
// thread-safe (uses bgfx's internal allocator which wraps malloc).
// All memcpy happens here, so drainOne() on the main thread is instant.
// -----------------------------------------------------------------------

// Try to load a texture from its cooked binary. Returns empty TextureGPUData on miss.
static TextureGPUData tryLoadCookedTexture(
    const std::filesystem::path& absTexPath,
    assetlib::AssetRegistry*     registry,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& cacheRoot)
{
    if (!registry || projectRoot.empty()) return {};
    std::error_code ec;
    auto rel = std::filesystem::relative(absTexPath, projectRoot, ec);
    if (ec) return {};
    auto rec = registry->findBySourcePath(rel.generic_string());
    if (!rec || rec->state != assetlib::AssetState::Ready || rec->cookedPath.empty()) return {};
    auto cookedAbs = cacheRoot / rec->cookedPath;
    if (!std::filesystem::exists(cookedAbs)) return {};
    assetlib::TextureAsset asset;
    if (!assetlib::loadTexture(asset, cookedAbs)) return {};
    TextureGPUData out;
    out.mem = bgfx::copy(asset.pixels.data(), (uint32_t)asset.pixels.size());
    out.w   = (uint16_t)asset.header.width;
    out.h   = (uint16_t)asset.header.height;
    return out;
}

static TextureGPUData loadTextureGPU(const aiScene*   scene,
                                      const char*      rawPath,
                                      const std::filesystem::path& dir,
                                      const std::string& baseName,
                                      assetlib::AssetRegistry* registry = nullptr,
                                      const std::filesystem::path& projectRoot = {},
                                      const std::filesystem::path& cacheRoot = {}) {
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
        // Fallback: Assimp may reference embedded textures by filename instead
        // of "*N" index.  Search the scene's embedded texture list by matching
        // the filename portion of the reference path.
        if (!px && scene && scene->mNumTextures > 0) {
            std::string refName = std::filesystem::path(rawPath).filename().string();
            std::string refLower = refName;
            for (auto& c : refLower) c = (char)std::tolower(c);
            for (uint32_t ti = 0; ti < scene->mNumTextures; ++ti) {
                const aiTexture* t = scene->mTextures[ti];
                std::string embName = t->mFilename.C_Str();
                std::string embLower = std::filesystem::path(embName).filename().string();
                for (auto& c : embLower) c = (char)std::tolower(c);
                if (embLower == refLower) {
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
                    if (px) {
                        LOG_SUCCESS("Assimp","Embedded texture matched: %s", refName.c_str());
                        break;
                    }
                }
            }
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
                        // Prefer cooked binary — no stb_image decode needed
                        auto cooked = tryLoadCookedTexture(
                            de.path(), registry, projectRoot, cacheRoot);
                        if (cooked.mem) {
                            LOG_INFO("BinaryLoader","Texture: %s", fn.c_str());
                            return cooked;
                        }
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

    // ── Binary fast path ───────────────────────────────────────────────
    // If a cooked version exists, read raw bytes directly — no Assimp.
    if (m_registry) {
        // Key is relative to project root e.g. "assets/Foo.fbx"
        std::string relKey;
        if (!m_projectRoot.empty()) {
            auto rel = std::filesystem::relative(
                std::filesystem::path(path), m_projectRoot);
            relKey = rel.generic_string();
        } else {
            relKey = std::filesystem::path(path).filename().string();
        }
        auto rec = m_registry->findBySourcePath(relKey);
        // cooked_path in DB is relative to .cache/ dir
        std::filesystem::path cookedAbs;
        if (rec && !rec->cookedPath.empty())
            cookedAbs = m_projectRoot / ".cache" / rec->cookedPath;
        if (rec && !rec->cookedPath.empty() &&
            rec->state == assetlib::AssetState::Ready &&
            std::filesystem::exists(cookedAbs)) {
            assetlib::MeshAsset asset;
            if (assetlib::loadMesh(asset, cookedAbs)) {
                const auto& h = asset.header;
                // Guard: cooked stride must match runtime Vertex exactly.
                // If not, fall through to Assimp (stale .cooked file).
                if (h.vertexStride != sizeof(Vertex)) {
                    LOG_WARN("BinaryLoader",
                        "Stride mismatch cooked=%u runtime=%zu — falling back to Assimp",
                        h.vertexStride, sizeof(Vertex));
                } else {
                MeshGPUData gd;
                gd.vertexMem  = bgfx::copy(asset.vertexData.data(),
                                           (uint32_t)asset.vertexData.size());
                gd.indexMem   = bgfx::copy(asset.indexData.data(),
                                           (uint32_t)asset.indexData.size());
                gd.indexCount  = h.indexCount;
                gd.use32       = (h.indexStride == 4);
                gd.doubleSided = false;
                gd.hasBounds   = true;
                std::memcpy(gd.boundsMin, h.boundsMin, sizeof(gd.boundsMin));
                std::memcpy(gd.boundsMax, h.boundsMax, sizeof(gd.boundsMax));
                // Submesh ranges — populated BEFORE move into out.meshes
                if (asset.submeshes.size() > 1) {
                    for (const auto& sub : asset.submeshes) {
                        SubRange sr;
                        sr.indexOffset = sub.indexOffset;
                        sr.indexCount  = sub.indexCount;
                        sr.matIndex    = 0; // MaterialCooker wires per-submesh mats
                        gd.subRanges.push_back(sr);
                    }
                }
                out.meshes.push_back(std::move(gd));
                // Use cooked material section if present;
                // otherwise fall back to auto-discovery by asset stem name.
                const std::filesystem::path srcDir =
                    std::filesystem::path(path).parent_path();
                const std::string            stem =
                    std::filesystem::path(path).stem().string();
                if (!asset.materials.empty()) {
                    for (uint32_t m = 0; m < (uint32_t)asset.materials.size(); ++m) {
                    const auto& cm = asset.materials[m];
                        MaterialGPUData mg;
                        std::memcpy(mg.baseColorFactor, cm.baseColorFactor, 16);
                        mg.roughness = cm.roughness;
                        mg.metallic  = cm.metallic;
                        if (cm.flags & assetlib::kMatFlag_HasBaseColor)
                            mg.baseColorTexture = loadTextureGPU(
                                nullptr, cm.baseColorPath, srcDir, stem,
                                m_registry, m_projectRoot, m_projectRoot / ".cache");
                        if (cm.flags & assetlib::kMatFlag_HasNormalMap)
                            mg.normalMapTexture = loadTextureGPU(
                                nullptr, cm.normalMapPath, srcDir, stem,
                                m_registry, m_projectRoot, m_projectRoot / ".cache");
                        mg.baseColorName = (cm.flags & assetlib::kMatFlag_HasBaseColor) ? cm.baseColorPath : "";
                        mg.normalMapName = (cm.flags & assetlib::kMatFlag_HasNormalMap) ? cm.normalMapPath : "";
                        LOG_INFO("BinaryLoader", "Mat[%u] base=%s nm=%s",
                            m,
                            mg.baseColorTexture.mem ? "ok" : "none",
                            mg.normalMapTexture.mem ? "ok" : "none");
                        if (!mg.baseColorTexture.mem)  // fallback: auto-discover
                            mg.baseColorTexture = loadTextureGPU(
                                nullptr, nullptr, srcDir, stem,
                                m_registry, m_projectRoot, m_projectRoot / ".cache");
                        out.materials.push_back(std::move(mg));
                    }
                } else {
                    // Legacy / geometry-only cook: auto-discover one texture
                    MaterialGPUData mg;
                    mg.baseColorTexture = loadTextureGPU(
                        nullptr, nullptr, srcDir, stem,
                        m_registry, m_projectRoot, m_projectRoot / ".cache");
                    out.materials.push_back(std::move(mg));
                }
                out.success = true;
                LOG_INFO("BinaryLoader", "%-30s verts=%u idx=%u tex=%s submeshes=%zu",
                       name.c_str(), h.vertexCount, h.indexCount,
                       out.materials.empty() || !out.materials[0].baseColorTexture.mem ? "none" : "ok",
                       out.meshes.empty() ? 0 : out.meshes[0].subRanges.size());
                return out;
                } // stride check
            }
        }
    }
    // ── Assimp fallback (uncooked assets) ─────────────────────────────
    // Wrapped in try-catch: a crash inside Assimp or skeleton extraction
    // must not bring down the engine — report it as a load error.
    try {
    Assimp::Importer imp;
    // Tell Assimp NOT to decompose FBX pre-rotation/translation/scaling into
    // separate $AssimpFbx$ intermediate nodes. Without this, a typical 68-bone
    // Mixamo skeleton explodes to 184+ nodes, overflowing kMaxBones (128).
    // With this flag false, Assimp bakes pivots into each bone's local
    // transform and remaps animation channels accordingly.
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* scene = imp.ReadFile(path,
        aiProcess_Triangulate        |
        aiProcess_GenSmoothNormals   |
        aiProcess_CalcTangentSpace   |
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
            AI_SUCCESS == ai->GetTexture(aiTextureType_BASE_COLOR, 0, &tp)) {
            LOG_INFO("Assimp", "Mat[%u] texPath=\"%s\" embeddedCount=%u",
                     i, tp.C_Str(), scene->mNumTextures);
            mg.baseColorTexture = loadTextureGPU(scene, tp.C_Str(), dir, bn,
                m_registry, m_projectRoot, m_projectRoot / ".cache");
        }
        if (!mg.baseColorTexture.mem)
            mg.baseColorTexture = loadTextureGPU(scene, nullptr, dir, bn,
                m_registry, m_projectRoot, m_projectRoot / ".cache");
        // Normal map
        aiString nmPath;
        if (AI_SUCCESS == ai->GetTexture(aiTextureType_NORMALS, 0, &nmPath) ||
            AI_SUCCESS == ai->GetTexture(aiTextureType_HEIGHT,  0, &nmPath)) {
            mg.normalMapTexture = loadTextureGPU(scene, nmPath.C_Str(), dir, bn,
                m_registry, m_projectRoot, m_projectRoot / ".cache");
            mg.normalMapName = std::filesystem::path(nmPath.C_Str()).filename().string();
            LOG_INFO("NormalMap", "Found: %s -> %s",
                     bn.c_str(), nmPath.C_Str());
        }
        // Base color display name
        aiString bcp; mg.baseColorName = "";
        if (AI_SUCCESS == ai->GetTexture(aiTextureType_DIFFUSE,    0, &bcp) ||
            AI_SUCCESS == ai->GetTexture(aiTextureType_BASE_COLOR, 0, &bcp))
            mg.baseColorName = std::filesystem::path(bcp.C_Str()).filename().string();
    }

    // ── Skeleton + animation extraction (worker-safe — pure CPU) ────────
    {
        bool hasBones = false;
        for (uint32_t i = 0; i < scene->mNumMeshes && !hasBones; ++i)
            if (scene->mMeshes[i]->mNumBones > 0) hasBones = true;

        if (hasBones) {
            out.skeleton    = anim::extractSkeleton(scene);
            out.hasSkeleton = out.skeleton.boneCount() > 0
                           && anim::buildOzzSkeleton(out.skeleton);
            if (out.hasSkeleton && scene->mNumAnimations > 0) {
                // Embedded clips -> compressed ozz Animations (same bridge the
                // standalone-clip ClipLibrary uses).
                std::string base = std::filesystem::path(path).stem().string();
                out.animClips.reserve(scene->mNumAnimations);
                for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
                    AnimClip c = anim::buildOzzClip(scene->mAnimations[a],
                                                    out.skeleton, base);
                    if (c.valid()) out.animClips.push_back(std::move(c));
                }
            }
            LOG_INFO("Assimp", "%s — %d bones, %u clip(s)",
                     name.c_str(), out.skeleton.boneCount(), (uint32_t)out.animClips.size());
        }
    }

    // Lambda: fill common vertex attributes shared by Vertex and SkinnedVertex
    auto fillCommon = [](const aiMesh* am, uint32_t v, float pos[3], float nrm[3],
                         float tan[4], float uv[2]) {
        pos[0] = am->mVertices[v].x;
        pos[1] = am->mVertices[v].y;
        pos[2] = am->mVertices[v].z;
        if (am->HasNormals()) {
            nrm[0] = am->mNormals[v].x;
            nrm[1] = am->mNormals[v].y;
            nrm[2] = am->mNormals[v].z;
        } else { nrm[0]=0; nrm[1]=1; nrm[2]=0; }
        if (am->HasTangentsAndBitangents()) {
            const auto& t  = am->mTangents[v];
            const auto& bt = am->mBitangents[v];
            const auto& n  = am->mNormals[v];
            float cx = n.y*t.z-n.z*t.y, cy = n.z*t.x-n.x*t.z, cz = n.x*t.y-n.y*t.x;
            float sign = (cx*bt.x+cy*bt.y+cz*bt.z) < 0.0f ? -1.0f : 1.0f;
            tan[0]=t.x; tan[1]=t.y; tan[2]=t.z; tan[3]=sign;
        } else {
            tan[0]=1; tan[1]=0; tan[2]=0; tan[3]=1;
        }
        if (am->mTextureCoords[0]) {
            uv[0] = am->mTextureCoords[0][v].x;
            uv[1] = am->mTextureCoords[0][v].y;
        } else { uv[0]=0; uv[1]=0; }
    };

    // ── Skinned FBX: combine all body-part meshes into ONE mesh ───────────
    // Mixamo FBX files often split the character into many meshes (jacket,
    // head, shoes…). They all share the same skeleton but become separate
    // bgfx vertex buffers if loaded individually — and only the first handle
    // is returned. Combine them here into a single VB/IB with submesh ranges
    // so the whole character renders as one entity.
    if (out.hasSkeleton) {
        // Count totals
        uint32_t totalVerts = 0, totalIndices = 0;
        for (uint32_t i = 0; i < scene->mNumMeshes; ++i) {
            const aiMesh* am = scene->mMeshes[i];
            if (!(am->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;
            if (am->mNumBones == 0) continue;
            totalVerts   += am->mNumVertices;
            totalIndices += am->mNumFaces * 3;
        }

        if (totalVerts > 0) {
            const bool use32 = totalVerts > 65535;
            std::vector<SkinnedVertex> allVerts;
            allVerts.reserve(totalVerts);
            std::vector<uint32_t> allIdx32;
            std::vector<uint16_t> allIdx16;
            if (use32) allIdx32.reserve(totalIndices);
            else       allIdx16.reserve(totalIndices);

            MeshGPUData mg;
            mg.skinned   = true;
            mg.use32     = use32;
            mg.hasBounds = true;
            float bMin[3] = {  FLT_MAX,  FLT_MAX,  FLT_MAX };
            float bMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

            uint32_t vertBase = 0;
            uint32_t idxBase  = 0;
            for (uint32_t i = 0; i < scene->mNumMeshes; ++i) {
                const aiMesh* am = scene->mMeshes[i];
                if (!(am->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;
                if (am->mNumBones == 0) continue;

                auto boneData = anim::extractBoneWeights(am, out.skeleton);
                int zeroWeightVerts = 0, maxBoneIdx = 0;
                for (uint32_t v = 0; v < am->mNumVertices; ++v) {
                    SkinnedVertex sv{};
                    fillCommon(am, v, sv.position, sv.normal, sv.tangent, sv.uv);
                    std::memcpy(sv.joints,  boneData[v].joints,  4);
                    std::memcpy(sv.weights, boneData[v].weights, sizeof(float)*4);
                    float wsum = sv.weights[0]+sv.weights[1]+sv.weights[2]+sv.weights[3];
                    if (wsum < 1e-6f) ++zeroWeightVerts;
                    for (int j = 0; j < 4; ++j)
                        if (sv.joints[j] > maxBoneIdx) maxBoneIdx = sv.joints[j];
                    allVerts.push_back(sv);
                    for (int k = 0; k < 3; ++k) {
                        bMin[k] = std::min(bMin[k], sv.position[k]);
                        bMax[k] = std::max(bMax[k], sv.position[k]);
                    }
                }
                LOG_INFO("Assimp", "  sub[%u] verts=%u zeroWeight=%d maxBoneIdx=%d skelBones=%d",
                    i, am->mNumVertices, zeroWeightVerts, maxBoneIdx, out.skeleton.boneCount());

                // Submesh for this body part
                SubRange sr;
                sr.indexOffset = idxBase;
                sr.indexCount  = am->mNumFaces * 3;
                sr.matIndex    = am->mMaterialIndex;
                mg.subRanges.push_back(sr);

                for (uint32_t f = 0; f < am->mNumFaces; ++f) {
                    for (uint32_t k = 0; k < am->mFaces[f].mNumIndices; ++k) {
                        uint32_t idx = am->mFaces[f].mIndices[k] + vertBase;
                        if (use32) allIdx32.push_back(idx);
                        else       allIdx16.push_back((uint16_t)idx);
                    }
                }
                idxBase  += am->mNumFaces * 3;
                vertBase += am->mNumVertices;
            }

            mg.vertexMem = bgfx::copy(allVerts.data(),
                (uint32_t)(allVerts.size() * sizeof(SkinnedVertex)));
            if (use32) {
                mg.indexCount = (uint32_t)allIdx32.size();
                mg.indexMem   = bgfx::copy(allIdx32.data(), mg.indexCount * 4);
            } else {
                mg.indexCount = (uint32_t)allIdx16.size();
                mg.indexMem   = bgfx::copy(allIdx16.data(), mg.indexCount * 2);
            }
            std::memcpy(mg.boundsMin, bMin, sizeof(bMin));
            std::memcpy(mg.boundsMax, bMax, sizeof(bMax));
            out.meshes.push_back(std::move(mg));

            LOG_INFO("Assimp", "%s — combined %u skinned sub-meshes: %u verts, %u indices",
                     name.c_str(), (uint32_t)out.meshes.back().subRanges.size(),
                     totalVerts, totalIndices);
        }
    }

    // ── Static meshes (non-skinned) ──────────────────────────────────────
    for (uint32_t i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* am = scene->mMeshes[i];
        if (!(am->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;
        // Skip skinned meshes — already combined above
        if (am->mNumBones > 0 && out.hasSkeleton) continue;

        MeshGPUData mg;
        mg.matIndex = am->mMaterialIndex;

        uint32_t vertCount = am->mNumVertices;
        std::vector<Vertex> verts(vertCount);
        for (uint32_t v = 0; v < vertCount; ++v)
            fillCommon(am, v, verts[v].position, verts[v].normal,
                       verts[v].tangent, verts[v].uv);
        mg.vertexMem = bgfx::copy(verts.data(),
                                   (uint32_t)(verts.size() * sizeof(Vertex)));

        // Build index array + bgfx::copy on worker
        mg.use32 = vertCount > 65535;
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

    } catch (const std::exception& ex) {
        out.error = std::string("Assimp exception: ") + ex.what();
        LOG_ERROR("Assimp", "Exception loading %s: %s", name.c_str(), ex.what());
        return out;
    } catch (...) {
        out.error = "Unknown exception during Assimp import";
        LOG_ERROR("Assimp", "Unknown exception loading %s", name.c_str());
        return out;
    }
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
    // Drain pending uploads — bgfx::Memory refs released by bgfx at shutdown
    std::lock_guard<std::mutex> lk(m_readyMtx);
    while (!m_ready.empty()) m_ready.pop();
}

void AsyncLoader::load(const std::string& path, const std::string& name, OnLoaded cb) {
    // Fast path: already fully loaded — return cached result immediately
    {
        std::lock_guard<std::mutex> lk(m_loadedMtx);
        auto it = m_loadedResults.find(normalizeKey(path));
        if (it != m_loadedResults.end()) {
            if (cb) cb(it->second, name);
            return;
        }
    }
    // In-flight: queue callback for when current load completes
    {
        std::lock_guard<std::mutex> lk(m_pendingMtx);
        if (m_inFlight.count(normalizeKey(path))) {
            m_waiters[normalizeKey(path)].push_back(std::move(cb));
            return;
        }
        m_inFlight.insert(normalizeKey(path));
        m_pending.push({path, name, std::move(cb)});
    }
    m_pendingCV.notify_one();
}

void AsyncLoader::unload(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_loadedMtx);
    m_loadedResults.erase(path);
}
bool AsyncLoader::isLoading(const std::string& path) const {
    std::lock_guard<std::mutex> lk(m_pendingMtx);
    return m_inFlight.count(path) > 0;
}

bool AsyncLoader::isLoaded(const std::string& path) const {
    std::lock_guard<std::mutex> lk(m_loadedMtx);
    return m_loadedResults.count(path) > 0;
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

        const bool succeeded = asset.success;
        {
            std::lock_guard<std::mutex> lk(m_readyMtx);
            m_ready.push({std::move(asset), std::move(req.cb)});
        }
        // Intentionally do NOT touch m_loadedHandles or m_inFlight here.
        // drainOne() (main thread) sets the real handle then erases inFlight
        // atomically, closing the window where load() could find an invalid
        // placeholder handle and call a callback prematurely.
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
        // Erase from inFlight so the path can be retried or waited on cleanly
        { std::lock_guard<std::mutex> lk(m_pendingMtx);
          m_inFlight.erase(normalizeKey(req.asset.path)); }
        LOG_ERROR("Loader", "Upload skipped (parse failed): %s",
                  req.asset.name.c_str());
        AsyncLoadResult failResult{};
        if (req.cb) req.cb(failResult, req.asset.name);
        // Drain waiters with invalid result so they don't block forever
        std::vector<OnLoaded> failWaiters;
        {
            std::lock_guard<std::mutex> lk(m_pendingMtx);
            auto it = m_waiters.find(req.asset.path);
            if (it != m_waiters.end()) {
                failWaiters = std::move(it->second);
                m_waiters.erase(it);
            }
        }
        for (auto& w : failWaiters) if (w) w(failResult, req.asset.name);
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
        if (mg.normalMapTexture.mem) {
            bgfx::TextureHandle th = bgfx::createTexture2D(
                mg.normalMapTexture.w, mg.normalMapTexture.h,
                false, 1, bgfx::TextureFormat::RGBA8,
                0, mg.normalMapTexture.mem);
            if (bgfx::isValid(th)) {
                Texture tex; tex.handle = th;
                mat.normalMapTexture = storage.textures.addTexture(std::move(tex));
            }
        }
        mat.baseColorName = mg.baseColorName;
        mat.normalMapName = mg.normalMapName;
        matHandles[i] = storage.materials.addMaterial(std::move(mat));
    }

    // Upload meshes (handle creation only — data already in bgfx pool)
    MeshHandle firstHandle{};
    for (const MeshGPUData& mg : req.asset.meshes) {
        if (!mg.vertexMem || !mg.indexMem) {
            LOG_ERROR("Loader", "Skipping mesh with null GPU memory: %s",
                      req.asset.name.c_str());
            continue;
        }
        // Select vertex layout based on whether this mesh has bone data
        bgfx::VertexBufferHandle vbh = mg.skinned
            ? bgfx::createVertexBuffer(mg.vertexMem, SkinnedVertex::layout())
            : bgfx::createVertexBuffer(mg.vertexMem, Vertex::layout());

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
        // Wire submesh ranges for multi-draw rendering
        for (const auto& sr : mg.subRanges) {
            SubmeshRange range;
            range.indexOffset = sr.indexOffset;
            range.indexCount  = sr.indexCount;
            range.material    = sr.matIndex < matHandles.size()
                                ? matHandles[sr.matIndex] : mesh.material;
            mesh.submeshes.push_back(range);
        }
        MeshHandle h = storage.meshes.addMesh(std::move(mesh));
        if (!firstHandle.valid()) firstHandle = h;
    }

    // ── Register skeleton + clips (main-thread registry operations) ───────
    AsyncLoadResult result;
    result.mesh = firstHandle;

    if (req.asset.hasSkeleton && storage.skeletons) {
        result.skeleton = storage.skeletons->add(
            Skeleton(req.asset.skeleton));   // copy into registry
        for (auto& clip : req.asset.animClips) {
            if (storage.clips)
                result.clips.push_back(storage.clips->add(std::move(clip)));
        }
    }

    LOG_SUCCESS("Loader", "Uploaded: %s (handle %u)%s",
                req.asset.name.c_str(), firstHandle.id,
                result.skeleton.valid() ? " [skinned]" : "");

    {
        std::lock_guard<std::mutex> lk(m_loadedMtx);
        m_loadedResults[req.asset.path] = result;
    }

    if (req.cb) req.cb(result, req.asset.name);
    // Drain any callbacks that queued while this path was in-flight
    std::vector<OnLoaded> waiters;
    {
        std::lock_guard<std::mutex> lk(m_pendingMtx);
        auto it = m_waiters.find(req.asset.path);
        if (it != m_waiters.end()) {
            waiters = std::move(it->second);
            m_waiters.erase(it);
        }
        m_inFlight.erase(normalizeKey(req.asset.path)); // clear in-flight on success
    }
    for (auto& w : waiters)
        if (w) w(result, req.asset.name);
    return true;
}
