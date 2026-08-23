#include "runtime/services/asset_service.h"

#include <assetlib/material_asset.h>

#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "render/mesh.h"
#include "render/texture.h"
#include "render/material.h"
#include "render/vertex.h"
#include "render/skinned_vertex.h"
#include "render/cooked_texture.h"   // format-aware BC upload
#include "animation/cooked_skin.h"        // v3 skinned payload decode
#include "animation/skeleton_registry.h"
#include "animation/clip_registry.h"
#include "core/logger.h"

#include <assetlib/mesh_asset.h>
#include <assetlib/texture_asset.h>
#include <assetlib/asset_registry.h>

#include <bgfx/bgfx.h>
#include <cstring>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <set>
#include <unordered_map>
#include <atomic>

// ═══════════════════════════════════════════════════════════════════════════
// Async internals — GPU-ready intermediate types + worker state
// ═══════════════════════════════════════════════════════════════════════════

// Pre-copied texture pixels (bgfx::copy on worker thread, handle on main).
// Carries the cooked format + mip count so block-compressed (BC7/BC5)
// payloads upload as-is — the runtime never decodes texels.
struct TexGPU {
    const bgfx::Memory* mem = nullptr;
    uint16_t w = 0, h = 0;
    uint32_t format = 0;      // assetlib::TextureFormatId
    uint32_t mips   = 1;
};

// Worker side: stage a cooked texture's payload for main-thread creation.
static TexGPU stageTexGPU(const assetlib::TextureAsset& t) {
    TexGPU out;
    out.mem    = bgfx::copy(t.pixels.data(), (uint32_t)t.pixels.size());
    out.w      = (uint16_t)t.header.width;
    out.h      = (uint16_t)t.header.height;
    out.format = t.header.format;
    out.mips   = t.header.mipCount ? t.header.mipCount : 1;
    return out;
}

// Main thread: one create path for every cooked-texture drain site.
static bgfx::TextureHandle createTexFromGPU(const TexGPU& t) {
    return bgfx::createTexture2D(t.w, t.h, t.mips > 1, 1,
                                 cookedTexBgfxFormat(t.format), 0, t.mem);
}

// Pre-copied material data ready for main-thread finalization
struct MatGPU {
    float       baseColorFactor[4] = {1,1,1,1};
    float       roughness          = 0.7f;
    float       metallic           = 0.0f;
    TexGPU      baseColor;
    TexGPU      normalMap;
    std::string baseColorName;
    std::string normalMapName;
};

struct SubGPU {
    uint32_t indexOffset    = 0;
    uint32_t indexCount     = 0;
    uint32_t materialIndex  = 0;
};

// Tagged result pushed from worker → main
struct ReadyAsset {
    enum Type { kMesh, kTexture } type;
    std::string key;         // normalized path (cache key)
    bool        success = false;
    std::string error;

    // ── Mesh fields (type == kMesh) ──
    const bgfx::Memory* vertexMem  = nullptr;
    const bgfx::Memory* indexMem   = nullptr;
    uint32_t            indexCount  = 0;
    uint32_t            indexStride = 4;
    float               boundsMin[3]{};
    float               boundsMax[3]{};
    std::string         sourcePath;
    std::vector<MatGPU> materials;
    std::vector<SubGPU> submeshes;

    // ── Texture fields (type == kTexture) ──
    TexGPU texData;
};

struct AssetService::AsyncState {
    std::thread             worker;
    std::atomic<bool>       running{true};

    // Request queue (main → worker)
    struct Request {
        ReadyAsset::Type type;
        std::string      key;
        std::string      absPath;
    };
    std::mutex              pendingMtx;
    std::condition_variable pendingCV;
    std::queue<Request>     pending;
    std::set<std::string>   inFlight;   // keys currently being processed

    // Result queue (worker → main)
    std::mutex              readyMtx;
    std::queue<ReadyAsset>  ready;

    // Loaded cache — written by main thread in drain, read by query*().
    // Meshes carry residency bookkeeping (bytes + LRU stamp) so
    // evictOverBudget can keep the cache bounded (audit Q6).
    // loadedMeshes / its mutex / the LRU clock moved to AssetService itself —
    // see the note there. A cache that only exists once a worker thread has been
    // spawned is a cache the synchronous path cannot use.
    // Guarded by AssetService::m_loadedMtx, which covers both loaded maps.
    std::unordered_map<std::string, TextureHandle>  loadedTextures;
    // Keys whose load FAILED (parse error, bad file, bgfx buffer failure).
    // Without this a failed asset has no handle and is no longer in flight —
    // indistinguishable from "never requested" to pollers like isSceneReady,
    // which then reported scenes containing assets that will never appear
    // (audit H.6). A fresh load*Async() request clears the key (retry).
    std::set<std::string>                           failedKeys;
};

// ═══════════════════════════════════════════════════════════════════════════
// Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════════

AssetService::AssetService(Config cfg)
    : m_meshes(cfg.meshes)
    , m_textures(cfg.textures)
    , m_materials(cfg.materials)
    , m_skeletons(cfg.skeletons)
    , m_clips(cfg.clips)
    , m_assetLib(cfg.assetLib)
    , m_projectRoot(cfg.projectRoot)
    , m_cacheRoot(cfg.projectRoot.empty() ? std::filesystem::path{}
                                          : cfg.projectRoot / ".cache")
    // Eviction hands the resource back to the registry, whose RAII destroys
    // the bgfx texture. The cache never calls bgfx itself — that is what keeps
    // it testable without a GPU. No budget yet: eviction needs the reference
    // counts to be complete first (only the sync path is wired), and evicting
    // against partial refcounts would drop textures that are still in use.
    , m_texCache([this](const TextureHandle& h) { m_textures.removeTexture(h); })
{}

AssetService::~AssetService() {
    if (m_async) {
        m_async->running = false;
        m_async->pendingCV.notify_all();
        if (m_async->worker.joinable())
            m_async->worker.join();
        // Undrained bgfx::Memory refs are freed by bgfx at shutdown.
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Path helpers
// ═══════════════════════════════════════════════════════════════════════════

std::string AssetService::resolvePath(const char* cookedPath) const {
    std::filesystem::path p(cookedPath);
    if (p.is_relative() && !m_cacheRoot.empty())
        p = m_cacheRoot / p;
    std::string s = p.string();
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// Worker-safe: resolve a texture path and load its cooked pixels.
// Returns TexGPU with bgfx::copy'd memory (thread-safe), or empty on miss.
static TexGPU resolveTextureGPU(const char* texPath,
                                 const std::filesystem::path& sourceDir,
                                 assetlib::AssetRegistry* assetLib,
                                 const std::filesystem::path& projectRoot,
                                 const std::filesystem::path& cacheRoot) {
    if (!texPath || texPath[0] == '\0') return {};

    // SIBLING .ctex FIRST. A cooked mesh's embedded textures are written by
    // MeshCooker as extra outputs BESIDE the cooked file, and the material
    // stores only their basename — "the loader resolves that against the
    // cooked file's own directory" (src/assets/info.md). They are not
    // registry assets, so the registry lookup below can never find them.
    //
    // Skipping this step is why shipped builds rendered UNTEXTURED while
    // reporting success: every material's texture resolved to nothing, and
    // "0.0 MB of textures" looked like a BC-compression win instead of a bug.
    {
        std::error_code sec;
        const auto direct = sourceDir / texPath;
        if (std::filesystem::exists(direct, sec)) {
            assetlib::TextureAsset sibling;
            if (assetlib::loadTexture(sibling, direct))
                return stageTexGPU(sibling);
        }
    }

    // Otherwise it names a SOURCE asset (assets/foo.png) that the registry
    // maps to its own cooked output.
    if (!assetLib || projectRoot.empty()) return {};

    std::filesystem::path absTexPath = sourceDir / texPath;
    std::error_code ec;
    auto relPath = std::filesystem::relative(absTexPath, projectRoot, ec);
    if (ec) return {};

    auto rec = assetLib->findBySourcePath(relPath.generic_string());
    if (!rec || rec->state != assetlib::AssetState::Ready
             || rec->cookedPath.empty())
        return {};

    auto cookedAbs = cacheRoot / rec->cookedPath;
    if (!std::filesystem::exists(cookedAbs)) return {};

    assetlib::TextureAsset texAsset;
    if (!assetlib::loadTexture(texAsset, cookedAbs)) return {};
    return stageTexGPU(texAsset);
}

// ═══════════════════════════════════════════════════════════════════════════
// Sync loadMesh
// ═══════════════════════════════════════════════════════════════════════════

MeshHandle AssetService::loadMesh(const char* cookedPath, MeshSkin* outSkin,
                                  MeshLods* outLods) {
    if (!cookedPath || cookedPath[0] == '\0') return {};

    // ── Dedup by cooked path, and it is not an optimisation ─────────────────
    // Two entities referencing the same mesh must SHARE one pair of GPU buffers.
    // Without this, sync loadMesh created a new vertex+index buffer per CALL, and
    // bgfx's pool is BGFX_CONFIG_MAX_INDEX_BUFFERS = 4096: a scene of 50 000
    // entities drawing 176 distinct cooked meshes loaded 4 089 of them and then
    // failed 45 911 times with "bgfx buffer creation failed", rendering 8% of the
    // scene. The count is the giveaway — it is the handle pool, not the files.
    //
    // Textures already worked this way ("every load of the same cooked texture
    // uploaded ANOTHER copy to the GPU", renderer audit R1 -> m_texCache). Meshes
    // never got the same treatment, and the primitive path hid it: every stress
    // scene to date used engine://primitive/cube, which is one shared mesh and
    // therefore one buffer pair however many entities reference it.
    //
    // Reuses the ASYNC path's map rather than introducing a second one. One cache
    // per resource kind is the point: a sync load and an async load of the same
    // path must not each create buffers. When async is absent (tools, tests) this
    // degrades to the old no-dedup behaviour rather than silently doing nothing.
    const std::string key = cookedPath;
    {
        std::lock_guard<std::mutex> lk(m_loadedMtx);
        auto it = m_loadedMeshes.find(key);
        if (it != m_loadedMeshes.end() && it->second.h.valid()) {
            it->second.lastUse = ++m_useClock;   // keep LRU honest
            // The chain travels with the cache entry. A cache HIT that returned
            // without filling this gave exactly the first entity per mesh an LOD
            // chain and every subsequent one none — 20 000 objects sharing 176
            // meshes would have produced 176 LOD'd entities and looked inert.
            if (outLods) outLods->levels = it->second.lods;
            if (outSkin) {
                // A cached skinned mesh cannot hand back its skeleton/clip
                // handles from here, so fall through to a full load rather than
                // return a mesh whose skin the caller silently never receives.
                if (m_meshes.getMesh(it->second.h) == nullptr
                    || !m_skeletons) { /* fall through */ }
                else { return it->second.h; }
            } else {
                return it->second.h;
            }
        }
    }

    std::filesystem::path absPath(cookedPath);
    if (absPath.is_relative() && !m_cacheRoot.empty())
        absPath = m_cacheRoot / absPath;

    assetlib::MeshAsset asset;
    if (!assetlib::loadMesh(asset, absPath)) {
        LOG_ERROR("AssetService", "Failed to load cooked mesh: %s", cookedPath);
        return {};
    }

    const auto& hdr = asset.header;
    // Static (48B Vertex) or v3 skinned (68B SkinnedVertex + bones + ozz
    // archives). Anything else is a stale cook.
    const bool skinned = hdr.version >= 3 && hdr.boneCount > 0
                      && hdr.vertexStride == sizeof(SkinnedVertex);
    if (hdr.vertexStride != sizeof(Vertex) && !skinned) {
        LOG_ERROR("AssetService",
            "Stride mismatch: cooked=%u runtime=%zu — re-cook needed: %s",
            hdr.vertexStride, sizeof(Vertex), cookedPath);
        return {};
    }
    if (skinned && (!m_skeletons || !m_clips)) {
        LOG_ERROR("AssetService", "skinned cooked mesh but no skeleton/clip "
                  "registries wired: %s", cookedPath);
        return {};
    }

    auto* vertexMem = bgfx::copy(asset.vertexData.data(),
                                  static_cast<uint32_t>(asset.vertexData.size()));
    auto* indexMem  = bgfx::copy(asset.indexData.data(),
                                  static_cast<uint32_t>(asset.indexData.size()));

    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(vertexMem,
        skinned ? SkinnedVertex::layout() : Vertex::layout());
    bgfx::IndexBufferHandle  ibh = (hdr.indexStride == 4)
        ? bgfx::createIndexBuffer(indexMem, BGFX_BUFFER_INDEX32)
        : bgfx::createIndexBuffer(indexMem);

    if (!bgfx::isValid(vbh) || !bgfx::isValid(ibh)) {
        if (bgfx::isValid(vbh)) bgfx::destroy(vbh);
        if (bgfx::isValid(ibh)) bgfx::destroy(ibh);
        LOG_ERROR("AssetService", "bgfx buffer creation failed: %s", cookedPath);
        return {};
    }

    Mesh mesh(vbh, ibh, hdr.indexCount);
    mesh.boundsMin  = { hdr.boundsMin[0], hdr.boundsMin[1], hdr.boundsMin[2] };
    mesh.boundsMax  = { hdr.boundsMax[0], hdr.boundsMax[1], hdr.boundsMax[2] };
    mesh.sourcePath = absPath.string();

    const auto sourceDir = absPath.parent_path();
    std::vector<MaterialHandle> matHandles;
    matHandles.reserve(asset.materials.size());

    for (uint32_t mi = 0; mi < static_cast<uint32_t>(asset.materials.size()); ++mi) {
        const auto& cm = asset.materials[mi];
        // Phase 5 step 4: a mesh-embedded material becomes the standard
        // shader's DECLARED form right here, so nothing downstream has to know
        // it came from geometry rather than a .cmat.
        TextureHandle base, norm;
        std::string   baseName, normName;
        if (cm.flags & assetlib::kMatFlag_HasBaseColor) {
            base     = resolveTexture(cm.baseColorPath, sourceDir);
            baseName = cm.baseColorPath;
        }
        if (cm.flags & assetlib::kMatFlag_HasNormalMap) {
            norm     = resolveTexture(cm.normalMapPath, sourceDir);
            normName = cm.normalMapPath;
        }
        Material mat = Material::standard(cm.baseColorFactor, cm.roughness,
                                          cm.metallic, base, norm);
        mat.baseColorName = std::move(baseName);
        mat.normalMapName = std::move(normName);
        matHandles.push_back(m_materials.addMaterial(std::move(mat)));
    }

    if (!matHandles.empty())
        mesh.material = matHandles[0];

    if (asset.submeshes.size() > 1) {
        for (const auto& sub : asset.submeshes) {
            SubmeshRange range;
            range.indexOffset = sub.indexOffset;
            range.indexCount  = sub.indexCount;
            range.material    = (sub.materialIndex < matHandles.size())
                ? matHandles[sub.materialIndex] : mesh.material;
            mesh.submeshes.push_back(range);
        }
    }

    MeshHandle result = m_meshes.addMesh(std::move(mesh));

    // Publish into the shared cache so the next reference to this path reuses
    // these buffers. Residency bytes match what the async drain records, so
    // evictOverBudget accounts for sync-loaded meshes too instead of treating
    // them as free.
    // ── The cooked LOD chain ────────────────────────────────────────────────
    // Each level is a real Mesh with its own buffers: the whole point is that a
    // coarser level costs FEWER triangles, so it cannot share level 0's index
    // buffer.
    //
    // Levels carry their PARENT'S MATERIAL GROUPS (cooked v5). They used to be
    // one range drawn with material[0], which meant a prop with two material
    // groups changed colour the moment it crossed a threshold — half the
    // MegaKit. Decimation rebuilds the index buffer group by group so the ranges
    // survive; a v4 file (or a genuinely single-material mesh) has no table,
    // which correctly reads as one range over the whole buffer.
    std::vector<MeshHandle> lodHandles;
    uint64_t lodBytes = 0;
    if (result.valid() && !asset.lods.empty() && !skinned) {
        for (const auto& lvl : asset.lods) {
            // indexData, not indexCount: the count is what gets handed to bgfx
            // as a draw range, and the loader now guarantees the two agree — but
            // the buffers are what bgfx::copy reads, so test those.
            if (lvl.indexData.empty() || lvl.vertexData.empty()) continue;
            auto* lv = bgfx::copy(lvl.vertexData.data(),
                                  (uint32_t)lvl.vertexData.size());
            auto* li = bgfx::copy(lvl.indexData.data(),
                                  (uint32_t)lvl.indexData.size());
            bgfx::VertexBufferHandle lvb =
                bgfx::createVertexBuffer(lv, Vertex::layout());
            bgfx::IndexBufferHandle lib = (hdr.indexStride == 4)
                ? bgfx::createIndexBuffer(li, BGFX_BUFFER_INDEX32)
                : bgfx::createIndexBuffer(li);
            if (!bgfx::isValid(lvb) || !bgfx::isValid(lib)) {
                if (bgfx::isValid(lvb)) bgfx::destroy(lvb);
                if (bgfx::isValid(lib)) bgfx::destroy(lib);
                // A level that fails to allocate is not fatal: extraction walks
                // back toward finer levels and counts the fallback. Losing
                // detail budget beats losing the object.
                LOG_WARN("AssetService", "LOD level buffer creation failed: %s",
                         cookedPath);
                break;
            }
            Mesh lm(lvb, lib, lvl.indexCount);
            // Level 0's bounds, deliberately — extraction culls and picks the
            // level from ONE sphere, so a level must not carry a different one.
            lm.boundsMin  = { hdr.boundsMin[0], hdr.boundsMin[1], hdr.boundsMin[2] };
            lm.boundsMax  = { hdr.boundsMax[0], hdr.boundsMax[1], hdr.boundsMax[2] };
            lm.material   = matHandles.empty() ? MaterialHandle{} : matHandles[0];
            lm.sourcePath = absPath.string();
            // The level's own material groups, resolved against the SAME
            // material handles as level 0 — the cooker stores an index into the
            // mesh's embedded material list, which is what matHandles is.
            for (const auto& sub : lvl.submeshes) {
                SubmeshRange range;
                range.indexOffset = sub.indexOffset;
                range.indexCount  = sub.indexCount;
                range.material    = (sub.materialIndex < matHandles.size())
                    ? matHandles[sub.materialIndex] : lm.material;
                lm.submeshes.push_back(range);
            }
            const MeshHandle lh = m_meshes.addMesh(std::move(lm));
            if (!lh.valid()) break;
            lodHandles.push_back(lh);
            lodBytes += (uint64_t)lvl.vertexData.size() + (uint64_t)lvl.indexData.size();
        }
        if (outLods) outLods->levels = lodHandles;
    }

    if (result.valid()) {
        const uint64_t bytes = (uint64_t)asset.vertexData.size()
                             + (uint64_t)asset.indexData.size() + lodBytes;
        std::lock_guard<std::mutex> lk(m_loadedMtx);
        m_loadedMeshes[key] = { result, bytes, ++m_useClock, lodHandles };
    }

    // Skinned payload: decode + register the skeleton and embedded clips
    // (shared decode with the editor's AsyncLoader — animation/cooked_skin.h).
    if (skinned) {
        Skeleton skel = anim::decodeCookedSkeleton(asset);
        if (!skel.ozz) {
            LOG_WARN("AssetService", "cooked skeleton blob unreadable — "
                     "bind pose only: %s", cookedPath);
        } else {
            SkeletonHandle sh = m_skeletons->add(std::move(skel));
            MeshSkin skin;
            skin.skeleton = sh;
            for (auto& clip : anim::decodeCookedClips(asset))
                skin.clips.push_back(m_clips->add(std::move(clip)));
            LOG_SUCCESS("AssetService", "COOKED skinned: %s — %u bones, "
                        "%zu clip(s), no source parse", cookedPath,
                        hdr.boneCount, skin.clips.size());
            if (outSkin) *outSkin = std::move(skin);
        }
    }

    LOG_SUCCESS("AssetService", "Loaded mesh: %s (handle=%u verts=%u idx=%u mats=%zu)",
                cookedPath, result.id,
                hdr.vertexCount, hdr.indexCount, asset.materials.size());
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Sync loadTexture
// ═══════════════════════════════════════════════════════════════════════════

TextureHandle AssetService::loadTexture(const char* cookedPath) {
    if (!cookedPath || cookedPath[0] == '\0') return {};
    std::filesystem::path absPath(cookedPath);
    if (absPath.is_relative() && !m_cacheRoot.empty())
        absPath = m_cacheRoot / absPath;
    return loadTextureFromCooked(absPath);
}

void AssetService::buildMaterialIndex() {
    if (m_materialIndexBuilt) return;
    // NOT latched yet. m_cacheRoot is late-bound (setProjectRoot, at project
    // open), and the editor boots projectless into the project hub — so an
    // early caller (the material picker) would otherwise mark the index
    // built-and-empty for the whole process and no material would ever resolve
    // again after a project opened. Nothing to scan is not the same as scanned.
    if (m_cacheRoot.empty()) return;

    std::error_code ec;
    const auto dir = m_cacheRoot / "materials";
    if (!std::filesystem::exists(dir, ec)) return;
    m_materialIndexBuilt = true;

    // Sorted, so a duplicated name resolves the same way on every machine
    // rather than depending on directory order.
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".cooked")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());

    for (const auto& f : files) {
        assetlib::MaterialAsset ma;
        if (!assetlib::loadMaterial(ma, f) || ma.name.empty()) continue;
        if (!m_materialByName.emplace(ma.name, f).second)
            LOG_WARN("AssetService", "two cooked materials are both named "
                     "\"%s\" — the first wins (stale cook?)", ma.name.c_str());
    }
    LOG_INFO("AssetService", "indexed %zu cooked material(s)",
             m_materialByName.size());
}

std::string AssetService::materialNameOf(MaterialHandle h) const {
    const auto it = m_materialByHandle.find(h.id);
    return it == m_materialByHandle.end() ? std::string{} : it->second;
}

std::vector<std::string> AssetService::materialNames() {
    buildMaterialIndex();
    std::vector<std::string> out;
    out.reserve(m_materialByName.size());
    for (const auto& [n, _] : m_materialByName) out.push_back(n);
    std::sort(out.begin(), out.end());
    return out;
}

MaterialHandle AssetService::loadMaterialAsset(const char* name) {
    if (!name || name[0] == '\0') return {};
    const std::string key = name;

    // One Material per name. A spawner calling this per entity is the expected
    // usage, and allocating a registry slot per spawn would leak steadily.
    if (const auto it = m_materialLoaded.find(key); it != m_materialLoaded.end())
        return it->second;

    buildMaterialIndex();
    const auto it = m_materialByName.find(key);
    if (it == m_materialByName.end()) {
        // The miss is deliberately NOT memoized: the index is already a cache,
        // and a second one layered on top would be a second thing to invalidate
        // for no gain — the lookup it saves is one hash probe. What must not
        // repeat is the DIAGNOSTIC: building the known-names list allocates,
        // sorts and concatenates, and a spawner asking per entity for a
        // misspelled name would pay that plus a log line every frame.
        //
        // NOTE: m_materialIndexBuilt latches after the first successful scan,
        // so a material cooked LATER in this process is not picked up by either
        // path. Live re-cook needs index invalidation, which nothing requests
        // yet — see materialNames().
        if (m_materialMissWarned.insert(key).second) {
            std::string known;
            for (const auto& n : materialNames()) {
                if (!known.empty()) known += ", ";
                known += n;
            }
            LOG_ERROR("AssetService", "no cooked material named \"%s\" (have: %s)",
                      name, known.empty() ? "none" : known.c_str());
        }
        return {};
    }
    const std::filesystem::path absPath = it->second;

    assetlib::MaterialAsset ma;
    if (!assetlib::loadMaterial(ma, absPath)) {
        LOG_ERROR("AssetService", "cannot load cooked material: %s",
                  absPath.string().c_str());
        return {};
    }

    Material mat;
    // No `dataDriven` flag any more — every material is. What distinguishes a
    // .cmat is that it NAMES its shader, which is what routes it to a program
    // of its own rather than the built-in one.
    mat.shaderName  = ma.shaderName;
    mat.featureMask = ma.featureMask;
    mat.doubleSided = ma.doubleSided;

    // Blocks are copied verbatim. The cooker already resolved every name,
    // checked arity and filled defaults against the shader's declared
    // interface, so there is nothing left to validate here — validating again
    // would be a second, drifting source of truth.
    mat.blocks.reserve(ma.uniforms.size());
    for (const auto& u : ma.uniforms)
        mat.blocks.push_back({ u.name, u.values });

    // Textures go through the same dedup/residency path as everything else, so
    // a material sharing an image with a mesh does not upload it twice.
    // Resolved relative to the PROJECT root: a .material names source paths
    // ("textures/rust.png") the way an author typed them.
    mat.textureBinds.reserve(ma.textures.size());
    for (const auto& t : ma.textures) {
        Material::TextureBind bind;
        bind.uniform  = t.uniform;
        bind.stage    = t.stage;
        bind.fallback = t.fallback;
        // The COOKED reference first, and it is the only one that works in a
        // shipped dist: there is no registry there, so `path` — a project
        // source path — resolves to nothing. engine_build fills this in when
        // packaging. Empty in a dev project, where the registry lookup below
        // is both available and authoritative.
        if (!t.cooked.empty() && !m_cacheRoot.empty()) {
            const auto abs = m_cacheRoot / t.cooked;
            std::error_code ec;
            if (std::filesystem::exists(abs, ec))
                bind.texture = loadTextureFromCooked(abs);
            if (!bind.texture.valid())
                LOG_WARN("AssetService", "material %s: cooked texture %s is "
                         "missing or unreadable — the package is incomplete",
                         ma.name.c_str(), t.cooked.c_str());
        }
        if (!bind.texture.valid() && !t.path.empty()) {
            bind.texture = resolveTexture(t.path.c_str(), m_projectRoot);
            if (!bind.texture.valid())
                LOG_WARN("AssetService", "material %s: texture %s did not "
                         "load — binding the %s fallback", ma.name.c_str(),
                         t.path.c_str(),
                         t.fallback.empty() ? "default" : t.fallback.c_str());
        }
        mat.textureBinds.push_back(std::move(bind));
    }

    const MaterialHandle h = m_materials.addMaterial(std::move(mat));
    m_materialLoaded.emplace(key, h);
    // insert_or_assign, not emplace: addMaterial reuses freed slots, so this id
    // may still carry the name of a material unloaded some other way.
    m_materialByHandle.insert_or_assign(h.id, key);
    m_materialMissWarned.erase(key);   // it exists now; a future miss is news again
    LOG_INFO("AssetService", "Loaded material: %s -> shader \"%s\" "
             "(%zu block(s), %zu texture(s), features 0x%x)",
             ma.name.c_str(), ma.shaderName.c_str(), ma.uniforms.size(),
             ma.textures.size(), ma.featureMask);
    return h;
}

// ═══════════════════════════════════════════════════════════════════════════
// Unload / Query
// ═══════════════════════════════════════════════════════════════════════════

bool AssetService::unloadMesh(MeshHandle h) {
    // ── FORGET THE CACHE ENTRY, or the next load returns a recycled slot ─────
    // AssetRegistry::removeMesh pushes the slot onto its free list, and the next
    // addMesh pops it. The dedup map keys a cooked path to a HANDLE, and
    // Handle::valid() is just `id != 0` — it never consults the registry. So an
    // entry left behind here is not merely stale: after any other mesh loads
    // into that slot, `loadMesh(samePath)` hits the cache, believes the handle
    // is live, and hands back A DIFFERENT MESH. Wrong geometry, no crash, and
    // nothing in the log.
    //
    // Levels are matched too, so unloading one does not leave the entry offering
    // a destroyed level to the next caller. Destruction does NOT cascade: every
    // path that receives a chain (SceneService) unloads the levels it was given,
    // and cascading here would free level 0 out from under a second scene that
    // shares it.
    {
        std::lock_guard<std::mutex> lk(m_loadedMtx);
        for (auto it = m_loadedMeshes.begin(); it != m_loadedMeshes.end(); ) {
            const bool isLevel0 = it->second.h.id == h.id;
            const bool isLevel  = std::find_if(it->second.lods.begin(),
                                               it->second.lods.end(),
                                               [&](MeshHandle l) { return l.id == h.id; })
                                  != it->second.lods.end();
            it = (isLevel0 || isLevel) ? m_loadedMeshes.erase(it) : std::next(it);
        }
    }
    return m_meshes.removeMesh(h);
}
bool AssetService::unloadTexture(TextureHandle h)  { return m_textures.removeTexture(h); }
bool AssetService::unloadMaterial(MaterialHandle h) {
    if (!m_materials.removeMaterial(h)) return false;
    // The name cache must let go in the same breath. MaterialHandle is a bare
    // slot index over a free list with no generation counter, so the next
    // addMaterial hands this exact id back out — and a stale entry here would
    // make loadMaterialAsset("rust") return whatever material now owns the slot.
    // Renders something plausible, hides the problem: the one outcome
    // loadMaterialAsset is documented to refuse.
    if (const auto it = m_materialByHandle.find(h.id); it != m_materialByHandle.end()) {
        m_materialLoaded.erase(it->second);
        m_materialByHandle.erase(it);
    }
    return true;
}

size_t AssetService::meshCount()     const { return m_meshes.meshCount(); }
size_t AssetService::textureCount()  const { return m_textures.textureCount(); }
size_t AssetService::materialCount() const { return m_materials.materialCount(); }

// ═══════════════════════════════════════════════════════════════════════════
// Sync internal helpers
// ═══════════════════════════════════════════════════════════════════════════

TextureHandle AssetService::loadTextureFromCooked(
        const std::filesystem::path& absPath) {
    // Keyed by the cooked path, normalised so "a/./b.ctex" and "a/b.ctex" are
    // one resource. The ENTIRE load is the factory, so a cache hit skips the
    // file read as well as the upload — the previous code re-read and
    // re-uploaded the same texture for every material that referenced it.
    //
    // Path, not content hash: the hash the DDC computes at cook time is not
    // carried in the .ctex header (which is full at 32 bytes), so two
    // byte-identical textures at different paths still upload twice. That is a
    // format change, tracked as a follow-up; path identity already removes the
    // duplicates that actually occur.
    const std::string key = absPath.lexically_normal().generic_string();

    TextureHandle out{};
    const bool ok = m_texCache.acquire(
        key, absPath.filename().string(),
        [&](TextureHandle& created, size_t& bytes) {
            assetlib::TextureAsset texAsset;
            if (!assetlib::loadTexture(texAsset, absPath)) {
                LOG_ERROR("AssetService", "Failed to load cooked texture: %s",
                          absPath.string().c_str());
                return false;
            }

            // Format-aware: BC7/BC5 blocks + mips upload as-is
            // (render/cooked_texture.h).
            bgfx::TextureHandle th = createCookedTexture(texAsset);
            if (!bgfx::isValid(th)) {
                LOG_ERROR("AssetService", "bgfx texture creation failed: %s",
                          absPath.string().c_str());
                return false;
            }

            Texture tex(th,
                        static_cast<uint16_t>(texAsset.header.width),
                        static_cast<uint16_t>(texAsset.header.height));

            created = m_textures.addTexture(std::move(tex));
            // GPU cost is the uploaded payload: BC blocks + the whole mip
            // chain, which is what the .ctex already holds.
            bytes = texAsset.pixels.size();
            LOG_INFO("AssetService", "Loaded texture: %s (%ux%u, handle=%u, %.1f MB)",
                     absPath.filename().string().c_str(),
                     texAsset.header.width, texAsset.header.height, created.id,
                     (double)bytes / (1024.0 * 1024.0));
            return true;
        },
        out);

    return ok ? out : TextureHandle{};
}

TextureHandle AssetService::resolveTexture(
        const char* texPath, const std::filesystem::path& sourceDir) {
    if (!texPath || texPath[0] == '\0') return {};

    // Sibling .ctex beside the cooked mesh — see resolveTextureGPU for why the
    // registry cannot resolve these. loadTextureFromCooked is cache-backed, so
    // two materials naming the same sibling share one upload.
    {
        std::error_code sec;
        const auto direct = sourceDir / texPath;
        if (std::filesystem::exists(direct, sec)) {
            // Existing is not the same as PARSING. This step used to return
            // whatever the load produced, failure included, and that made every
            // .material texture unresolvable: a material names a SOURCE asset
            // ("assets/tex/brick.jpeg") relative to the project root, so the
            // file at that path exists — it is the source image, not a cooked
            // .ctex. Parsing it as one fails, and returning that failure
            // short-circuited the registry lookup below that would have found
            // the real cooked output. Every textured material silently bound
            // its white fallback.
            //
            // The async twin (resolveTextureGPU) always had this right; only
            // the sync path was wrong, which is why nothing caught it — and
            // why the one sample .material in the tree has no textures.
            if (TextureHandle h = loadTextureFromCooked(direct); h.valid())
                return h;
        }
    }

    if (m_assetLib && !m_projectRoot.empty()) {
        std::filesystem::path absTexPath = sourceDir / texPath;
        std::error_code ec;
        auto relPath = std::filesystem::relative(absTexPath, m_projectRoot, ec);
        if (!ec) {
            auto rec = m_assetLib->findBySourcePath(relPath.generic_string());
            if (rec && rec->state == assetlib::AssetState::Ready
                    && !rec->cookedPath.empty()) {
                auto cookedAbs = m_cacheRoot / rec->cookedPath;
                if (std::filesystem::exists(cookedAbs))
                    return loadTextureFromCooked(cookedAbs);
            }
        }
    }

    LOG_WARN("AssetService", "Could not resolve texture: %s (from %s)",
             texPath, sourceDir.string().c_str());
    return {};
}

// ═══════════════════════════════════════════════════════════════════════════
// Async: worker lifecycle
// ═══════════════════════════════════════════════════════════════════════════

void AssetService::ensureWorker() {
    if (m_async) return;
    m_async = std::make_unique<AsyncState>();
    m_async->worker = std::thread([this] { workerLoop(); });
}

void AssetService::workerLoop() {
    while (m_async->running) {
        AsyncState::Request req;
        {
            std::unique_lock<std::mutex> lk(m_async->pendingMtx);
            m_async->pendingCV.wait(lk, [this] {
                return !m_async->pending.empty() || !m_async->running;
            });
            if (!m_async->running && m_async->pending.empty()) break;
            req = std::move(m_async->pending.front());
            m_async->pending.pop();
        }

        ReadyAsset result;
        result.type = req.type;
        result.key  = req.key;

        if (req.type == ReadyAsset::kMesh) {
            // ── Parse cooked mesh on worker thread ──────────────────
            assetlib::MeshAsset asset;
            if (!assetlib::loadMesh(asset, req.absPath)) {
                result.error = "Failed to load cooked mesh";
            } else if (asset.header.vertexStride != sizeof(Vertex)) {
                result.error = "Stride mismatch — re-cook needed";
            } else {
                const auto& hdr = asset.header;
                result.vertexMem  = bgfx::copy(asset.vertexData.data(),
                                    static_cast<uint32_t>(asset.vertexData.size()));
                result.indexMem   = bgfx::copy(asset.indexData.data(),
                                    static_cast<uint32_t>(asset.indexData.size()));
                result.indexCount  = hdr.indexCount;
                result.indexStride = hdr.indexStride;
                std::memcpy(result.boundsMin, hdr.boundsMin, sizeof(result.boundsMin));
                std::memcpy(result.boundsMax, hdr.boundsMax, sizeof(result.boundsMax));
                result.sourcePath = req.absPath;

                // Submeshes
                if (asset.submeshes.size() > 1) {
                    for (const auto& sub : asset.submeshes)
                        result.submeshes.push_back({sub.indexOffset, sub.indexCount,
                                                     sub.materialIndex});
                }

                // Materials + texture resolution (all I/O on worker)
                //
                // STAGE EACH .ctex ONCE PER MESH. Materials routinely share an
                // image — the fps_shooter pistol has 4 material references to
                // 2 files after the cooker's content dedup — and staging each
                // reference separately read the file and bgfx::copy'd it again,
                // uploading the same texture twice.
                //
                // The skip has to happen HERE, on the worker, not at the drain:
                // bgfx memory from copy() is only freed when a create call
                // consumes it, so dropping an already-allocated duplicate at
                // the drain would leak it. Not allocating is the only safe
                // form of "don't upload this twice".
                //
                // Scope is ONE MESH. Cross-mesh sharing still uploads twice;
                // that needs the worker to consult the (main-thread) texture
                // cache safely and is the next step.
                const auto sourceDir = std::filesystem::path(req.absPath).parent_path();
                std::unordered_map<std::string, uint32_t> stagedTex; // name -> first user
                for (uint32_t mi = 0; mi < static_cast<uint32_t>(asset.materials.size()); ++mi) {
                    const auto& cm = asset.materials[mi];
                    MatGPU mg;
                    std::memcpy(mg.baseColorFactor, cm.baseColorFactor, 16);
                    mg.roughness = cm.roughness;
                    mg.metallic  = cm.metallic;

                    // The NAME is always recorded, staged or not: it is what
                    // the drain uses to resolve a skipped material to the
                    // handle the first one created.
                    if (cm.flags & assetlib::kMatFlag_HasBaseColor) {
                        mg.baseColorName = cm.baseColorPath;
                        if (stagedTex.emplace(cm.baseColorPath, mi).second)
                            mg.baseColor = resolveTextureGPU(cm.baseColorPath,
                                               sourceDir, m_assetLib,
                                               m_projectRoot, m_cacheRoot);
                    }
                    if (cm.flags & assetlib::kMatFlag_HasNormalMap) {
                        mg.normalMapName = cm.normalMapPath;
                        if (stagedTex.emplace(cm.normalMapPath, mi).second)
                            mg.normalMap = resolveTextureGPU(cm.normalMapPath,
                                               sourceDir, m_assetLib,
                                               m_projectRoot, m_cacheRoot);
                    }
                    result.materials.push_back(std::move(mg));
                }
                result.success = true;
                LOG_INFO("AssetService", "Worker parsed mesh: %s (verts=%u idx=%u)",
                         req.key.c_str(), hdr.vertexCount, hdr.indexCount);
            }
        } else {
            // ── Parse cooked texture on worker thread ───────────────
            assetlib::TextureAsset texAsset;
            if (!assetlib::loadTexture(texAsset, req.absPath)) {
                result.error = "Failed to load cooked texture";
            } else {
                result.texData = stageTexGPU(texAsset);
                result.success = true;
                LOG_INFO("AssetService", "Worker parsed texture: %s (%ux%u)",
                         req.key.c_str(), texAsset.header.width, texAsset.header.height);
            }
        }

        if (!result.success)
            LOG_ERROR("AssetService", "Worker failed: %s — %s",
                      req.key.c_str(), result.error.c_str());

        {
            std::lock_guard<std::mutex> lk(m_async->readyMtx);
            m_async->ready.push(std::move(result));
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Async: loadMeshAsync / loadTextureAsync
// ═══════════════════════════════════════════════════════════════════════════

void AssetService::loadMeshAsync(const char* cookedPath) {
    if (!cookedPath || cookedPath[0] == '\0') return;
    ensureWorker();

    std::string key = resolvePath(cookedPath);

    // Already loaded? (A fresh request also clears any recorded failure —
    // explicit retry semantics — and stamps use for the LRU.)
    {
        std::lock_guard<std::mutex> lk(m_loadedMtx);
        if (auto it = m_loadedMeshes.find(key);
            it != m_loadedMeshes.end()) {
            it->second.lastUse = ++m_useClock;
            return;
        }
        m_async->failedKeys.erase(key);
    }
    // Already in-flight?
    {
        std::lock_guard<std::mutex> lk(m_async->pendingMtx);
        if (m_async->inFlight.count(key)) return;
        m_async->inFlight.insert(key);
        m_async->pending.push({ReadyAsset::kMesh, key, key});
    }
    m_async->pendingCV.notify_one();
}

void AssetService::loadTextureAsync(const char* cookedPath) {
    if (!cookedPath || cookedPath[0] == '\0') return;
    ensureWorker();

    std::string key = resolvePath(cookedPath);

    {
        std::lock_guard<std::mutex> lk(m_loadedMtx);
        if (m_async->loadedTextures.count(key)) return;
        m_async->failedKeys.erase(key);              // retry semantics
    }
    {
        std::lock_guard<std::mutex> lk(m_async->pendingMtx);
        if (m_async->inFlight.count(key)) return;
        m_async->inFlight.insert(key);
        m_async->pending.push({ReadyAsset::kTexture, key, key});
    }
    m_async->pendingCV.notify_one();
}

// ═══════════════════════════════════════════════════════════════════════════
// Async: queryMesh / queryTexture / isLoading / pendingCount
// ═══════════════════════════════════════════════════════════════════════════

uint32_t AssetService::queryMesh(const char* cookedPath) const {
    if (!m_async || !cookedPath) return 0;
    std::string key = resolvePath(cookedPath);
    std::lock_guard<std::mutex> lk(m_loadedMtx);
    auto it = m_loadedMeshes.find(key);
    if (it == m_loadedMeshes.end()) return 0;
    it->second.lastUse = ++m_useClock;   // a query IS a use (LRU)
    return it->second.h.id;
}

uint32_t AssetService::queryTexture(const char* cookedPath) const {
    if (!m_async || !cookedPath) return 0;
    std::string key = resolvePath(cookedPath);
    std::lock_guard<std::mutex> lk(m_loadedMtx);
    auto it = m_async->loadedTextures.find(key);
    return (it != m_async->loadedTextures.end()) ? it->second.id : 0;
}

void AssetService::setResidencyBudget(uint64_t bytes) {
    m_residencyBudget = bytes;
}

uint64_t AssetService::residentBytes() const {
    if (!m_async) return 0;
    std::lock_guard<std::mutex> lk(m_loadedMtx);
    uint64_t sum = 0;
    for (const auto& [k, e] : m_loadedMeshes) sum += e.bytes;
    return sum;
}

size_t AssetService::evictOverBudget(const std::unordered_set<uint32_t>& inUse) {
    if (!m_async || m_residencyBudget == 0) return 0;

    // Collect eviction candidates under the lock; destroy registry entries
    // OUTSIDE it (Mesh dtors issue bgfx::destroy — keep lock scopes tight).
    // The LOD levels ride along, and they have to: `bytes` INCLUDES them, so an
    // eviction that freed only level 0 credited itself with reclaiming memory it
    // left resident — the budget total never came down and the level buffers
    // leaked for the life of the process.
    struct Victim { std::string key; MeshHandle h; uint64_t bytes;
                    std::vector<MeshHandle> lods; };
    std::vector<Victim> victims;
    {
        std::lock_guard<std::mutex> lk(m_loadedMtx);
        uint64_t total = 0;
        for (const auto& [k, e] : m_loadedMeshes) total += e.bytes;
        if (total <= m_residencyBudget) return 0;

        // LRU order: oldest stamp first.
        std::vector<const std::pair<const std::string,
                                    AssetService::MeshResidency>*> byAge;
        byAge.reserve(m_loadedMeshes.size());
        for (const auto& kv : m_loadedMeshes) byAge.push_back(&kv);
        std::sort(byAge.begin(), byAge.end(),
                  [](auto* a, auto* b) {
                      return a->second.lastUse < b->second.lastUse;
                  });

        for (auto* kv : byAge) {
            if (total <= m_residencyBudget) break;
            // A live MeshRenderer still points at this handle — evicting
            // would yank the mesh out from under the renderer. Skip; it
            // ages out naturally once nothing references it.
            // A live MeshRenderer OR a live LodMesh level still points at this
            // entry. The caller collects both (EngineRuntime::frame) — and it
            // must, because after LOD selection the RenderItem the pipeline
            // dereferences is a LEVEL, not level 0. Evicting a level whose
            // parent is idle-but-referenced would hand bgfx a destroyed buffer.
            if (inUse.count(kv->second.h.id)) continue;
            bool levelInUse = false;
            for (MeshHandle lh : kv->second.lods)
                if (inUse.count(lh.id)) { levelInUse = true; break; }
            if (levelInUse) continue;
            victims.push_back({kv->first, kv->second.h, kv->second.bytes,
                               kv->second.lods});
            total -= kv->second.bytes;
        }
        for (const auto& v : victims) m_loadedMeshes.erase(v.key);
    }

    for (const auto& v : victims) {
        m_meshes.removeMesh(v.h);           // frees GPU buffers (Mesh dtor)
        for (MeshHandle lh : v.lods) m_meshes.removeMesh(lh);
        LOG_INFO("AssetService", "Evicted LRU mesh: %s (%.2f MB, %zu LOD level(s))"
                 " — reloads on next request", v.key.c_str(),
                 v.bytes / (1024.0 * 1024.0), v.lods.size());
    }
    return victims.size();
}

bool AssetService::isLoading(const char* cookedPath) const {
    if (!m_async || !cookedPath) return false;
    std::string key = resolvePath(cookedPath);
    std::lock_guard<std::mutex> lk(m_async->pendingMtx);
    return m_async->inFlight.count(key) > 0;
}

bool AssetService::loadFailed(const char* cookedPath) const {
    if (!m_async || !cookedPath) return false;
    std::string key = resolvePath(cookedPath);
    std::lock_guard<std::mutex> lk(m_loadedMtx);
    return m_async->failedKeys.count(key) > 0;
}

int AssetService::pendingCount() const {
    if (!m_async) return 0;
    std::lock_guard<std::mutex> lk(m_async->pendingMtx);
    // inFlight tracks every key from enqueue to drain — pending is a subset.
    return static_cast<int>(m_async->inFlight.size());
}

// ═══════════════════════════════════════════════════════════════════════════
// Async: drainUploads — main thread only
// ═══════════════════════════════════════════════════════════════════════════
//
// Pops ONE completed result from the ready queue, creates bgfx handles
// (O(microseconds) — no memcpy, data already in bgfx's pool), registers
// in the runtime registries, and stores in the loaded cache for queryMesh/
// queryTexture. Returns true if an asset was processed.

bool AssetService::drainUploads() {
    if (!m_async) return false;

    ReadyAsset item;
    {
        std::lock_guard<std::mutex> lk(m_async->readyMtx);
        if (m_async->ready.empty()) return false;
        item = std::move(m_async->ready.front());
        m_async->ready.pop();
    }

    // Remove from in-flight regardless of success/failure
    {
        std::lock_guard<std::mutex> lk(m_async->pendingMtx);
        m_async->inFlight.erase(item.key);
    }

    if (!item.success) {
        LOG_ERROR("AssetService", "Async load failed: %s — %s",
                  item.key.c_str(), item.error.c_str());
        { std::lock_guard<std::mutex> lk(m_loadedMtx);
          m_async->failedKeys.insert(item.key); }   // pollers must see this
        return true; // consumed an item even on failure
    }

    if (item.type == ReadyAsset::kMesh) {
        // ── Finalize mesh ───────────────────────────────────────────
        bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(
            item.vertexMem, Vertex::layout());
        bgfx::IndexBufferHandle  ibh = (item.indexStride == 4)
            ? bgfx::createIndexBuffer(item.indexMem, BGFX_BUFFER_INDEX32)
            : bgfx::createIndexBuffer(item.indexMem);

        if (!bgfx::isValid(vbh) || !bgfx::isValid(ibh)) {
            if (bgfx::isValid(vbh)) bgfx::destroy(vbh);
            if (bgfx::isValid(ibh)) bgfx::destroy(ibh);
            LOG_ERROR("AssetService", "Async bgfx buffer creation failed: %s",
                      item.key.c_str());
            { std::lock_guard<std::mutex> lk(m_loadedMtx);
              m_async->failedKeys.insert(item.key); }
            return true;
        }

        Mesh mesh(vbh, ibh, item.indexCount);
        mesh.boundsMin  = {item.boundsMin[0], item.boundsMin[1], item.boundsMin[2]};
        mesh.boundsMax  = {item.boundsMax[0], item.boundsMax[1], item.boundsMax[2]};
        mesh.sourcePath = item.sourcePath;

        // Finalize materials
        std::vector<MaterialHandle> matHandles;
        // Resolves materials the worker deliberately did NOT stage (see the
        // staging comment): a null `mem` with a non-empty name means "another
        // material in this mesh already uploaded this file" — share its handle
        // instead of uploading a second copy. Populated in the same order the
        // worker staged, so the first user is always seen first.
        std::unordered_map<std::string, TextureHandle> texByName;

        for (auto& mg : item.materials) {
            // Texture resolution is unchanged — including the dedup that lets a
            // material share an image another material in the same mesh already
            // uploaded. Only the SHAPE of the result changed.
            TextureHandle base, norm;

            if (mg.baseColor.mem) {
                auto th = createTexFromGPU(mg.baseColor);   // format-aware
                if (bgfx::isValid(th)) {
                    base = m_textures.addTexture(
                        Texture(th, mg.baseColor.w, mg.baseColor.h));
                    if (!mg.baseColorName.empty())
                        texByName[mg.baseColorName] = base;
                }
            } else if (!mg.baseColorName.empty()) {
                auto it = texByName.find(mg.baseColorName);
                if (it != texByName.end()) base = it->second;
                // A miss means the FIRST user failed to load; leaving the
                // handle invalid falls back to the white texture, which is
                // the same behaviour as before.
            }

            if (mg.normalMap.mem) {
                auto th = createTexFromGPU(mg.normalMap);
                if (bgfx::isValid(th)) {
                    norm = m_textures.addTexture(
                        Texture(th, mg.normalMap.w, mg.normalMap.h));
                    if (!mg.normalMapName.empty())
                        texByName[mg.normalMapName] = norm;
                }
            } else if (!mg.normalMapName.empty()) {
                auto it = texByName.find(mg.normalMapName);
                if (it != texByName.end()) norm = it->second;
            }

            Material mat = Material::standard(mg.baseColorFactor, mg.roughness,
                                              mg.metallic, base, norm);
            mat.baseColorName = mg.baseColorName;
            mat.normalMapName = mg.normalMapName;
            matHandles.push_back(m_materials.addMaterial(std::move(mat)));
        }

        if (!matHandles.empty())
            mesh.material = matHandles[0];

        for (auto& sub : item.submeshes) {
            SubmeshRange range;
            range.indexOffset = sub.indexOffset;
            range.indexCount  = sub.indexCount;
            range.material    = (sub.materialIndex < matHandles.size())
                ? matHandles[sub.materialIndex] : mesh.material;
            mesh.submeshes.push_back(range);
        }

        MeshHandle h = m_meshes.addMesh(std::move(mesh));
        {
            // Residency bookkeeping: GPU-side byte cost + fresh LRU stamp.
            // (bgfx releases the staging Memory at frame end — reading the
            // sizes here, microseconds after handle creation, is safe.)
            const uint64_t bytes =
                (item.vertexMem ? item.vertexMem->size : 0u) +
                (item.indexMem  ? item.indexMem->size  : 0u);
            std::lock_guard<std::mutex> lk(m_loadedMtx);
            m_loadedMeshes[item.key] =
                {h, bytes, ++m_useClock};
        }
        LOG_SUCCESS("AssetService", "Async mesh ready: %s (handle=%u)",
                    item.key.c_str(), h.id);

    } else {
        // ── Finalize texture ────────────────────────────────────────
        bgfx::TextureHandle th = createTexFromGPU(item.texData);

        if (!bgfx::isValid(th)) {
            LOG_ERROR("AssetService", "Async bgfx texture creation failed: %s",
                      item.key.c_str());
            return true;
        }

        Texture tex(th, item.texData.w, item.texData.h);
        TextureHandle handle = m_textures.addTexture(std::move(tex));
        {
            std::lock_guard<std::mutex> lk(m_loadedMtx);
            m_async->loadedTextures[item.key] = handle;
        }
        LOG_SUCCESS("AssetService", "Async texture ready: %s (%ux%u, handle=%u)",
                    item.key.c_str(), item.texData.w, item.texData.h, handle.id);
    }

    return true;
}
