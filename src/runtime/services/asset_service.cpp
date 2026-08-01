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
    struct MeshResidency {
        MeshHandle h;
        uint64_t   bytes   = 0;
        uint64_t   lastUse = 0;
    };
    mutable std::mutex                              loadedMtx;
    std::unordered_map<std::string, MeshResidency>  loadedMeshes;
    std::unordered_map<std::string, TextureHandle>  loadedTextures;
    uint64_t                                        useClock = 0;   // LRU ticks
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

MeshHandle AssetService::loadMesh(const char* cookedPath, MeshSkin* outSkin) {
    if (!cookedPath || cookedPath[0] == '\0') return {};

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
        Material mat;
        std::memcpy(mat.baseColorFactor, cm.baseColorFactor,
                    sizeof(mat.baseColorFactor));
        mat.roughness   = cm.roughness;
        mat.metallic    = cm.metallic;
        mat.doubleSided = false;

        if (cm.flags & assetlib::kMatFlag_HasBaseColor) {
            mat.baseColorTexture = resolveTexture(cm.baseColorPath, sourceDir);
            mat.baseColorName    = cm.baseColorPath;
        }
        if (cm.flags & assetlib::kMatFlag_HasNormalMap) {
            mat.normalMapTexture = resolveTexture(cm.normalMapPath, sourceDir);
            mat.normalMapName    = cm.normalMapPath;
        }
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

MaterialHandle AssetService::loadMaterialAsset(const char* cookedPath) {
    if (!cookedPath || cookedPath[0] == '\0') return {};
    std::filesystem::path absPath(cookedPath);
    if (absPath.is_relative() && !m_cacheRoot.empty())
        absPath = m_cacheRoot / absPath;

    assetlib::MaterialAsset ma;
    if (!assetlib::loadMaterial(ma, absPath)) {
        LOG_ERROR("AssetService", "cannot load cooked material: %s",
                  absPath.string().c_str());
        return {};
    }

    Material mat;
    mat.dataDriven  = true;
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
        if (!t.path.empty()) {
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
    LOG_INFO("AssetService", "Loaded material: %s -> shader \"%s\" "
             "(%zu block(s), %zu texture(s), features 0x%x)",
             ma.name.c_str(), ma.shaderName.c_str(), ma.uniforms.size(),
             ma.textures.size(), ma.featureMask);
    return h;
}

// ═══════════════════════════════════════════════════════════════════════════
// Unload / Query
// ═══════════════════════════════════════════════════════════════════════════

bool AssetService::unloadMesh(MeshHandle h)       { return m_meshes.removeMesh(h); }
bool AssetService::unloadTexture(TextureHandle h)  { return m_textures.removeTexture(h); }
bool AssetService::unloadMaterial(MaterialHandle h) { return m_materials.removeMaterial(h); }

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
        if (std::filesystem::exists(direct, sec))
            return loadTextureFromCooked(direct);
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
        std::lock_guard<std::mutex> lk(m_async->loadedMtx);
        if (auto it = m_async->loadedMeshes.find(key);
            it != m_async->loadedMeshes.end()) {
            it->second.lastUse = ++m_async->useClock;
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
        std::lock_guard<std::mutex> lk(m_async->loadedMtx);
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
    std::lock_guard<std::mutex> lk(m_async->loadedMtx);
    auto it = m_async->loadedMeshes.find(key);
    if (it == m_async->loadedMeshes.end()) return 0;
    it->second.lastUse = ++m_async->useClock;   // a query IS a use (LRU)
    return it->second.h.id;
}

uint32_t AssetService::queryTexture(const char* cookedPath) const {
    if (!m_async || !cookedPath) return 0;
    std::string key = resolvePath(cookedPath);
    std::lock_guard<std::mutex> lk(m_async->loadedMtx);
    auto it = m_async->loadedTextures.find(key);
    return (it != m_async->loadedTextures.end()) ? it->second.id : 0;
}

void AssetService::setResidencyBudget(uint64_t bytes) {
    m_residencyBudget = bytes;
}

uint64_t AssetService::residentBytes() const {
    if (!m_async) return 0;
    std::lock_guard<std::mutex> lk(m_async->loadedMtx);
    uint64_t sum = 0;
    for (const auto& [k, e] : m_async->loadedMeshes) sum += e.bytes;
    return sum;
}

size_t AssetService::evictOverBudget(const std::unordered_set<uint32_t>& inUse) {
    if (!m_async || m_residencyBudget == 0) return 0;

    // Collect eviction candidates under the lock; destroy registry entries
    // OUTSIDE it (Mesh dtors issue bgfx::destroy — keep lock scopes tight).
    struct Victim { std::string key; MeshHandle h; uint64_t bytes; };
    std::vector<Victim> victims;
    {
        std::lock_guard<std::mutex> lk(m_async->loadedMtx);
        uint64_t total = 0;
        for (const auto& [k, e] : m_async->loadedMeshes) total += e.bytes;
        if (total <= m_residencyBudget) return 0;

        // LRU order: oldest stamp first.
        std::vector<const std::pair<const std::string,
                                    AsyncState::MeshResidency>*> byAge;
        byAge.reserve(m_async->loadedMeshes.size());
        for (const auto& kv : m_async->loadedMeshes) byAge.push_back(&kv);
        std::sort(byAge.begin(), byAge.end(),
                  [](auto* a, auto* b) {
                      return a->second.lastUse < b->second.lastUse;
                  });

        for (auto* kv : byAge) {
            if (total <= m_residencyBudget) break;
            // A live MeshRenderer still points at this handle — evicting
            // would yank the mesh out from under the renderer. Skip; it
            // ages out naturally once nothing references it.
            if (inUse.count(kv->second.h.id)) continue;
            victims.push_back({kv->first, kv->second.h, kv->second.bytes});
            total -= kv->second.bytes;
        }
        for (const auto& v : victims) m_async->loadedMeshes.erase(v.key);
    }

    for (const auto& v : victims) {
        m_meshes.removeMesh(v.h);           // frees GPU buffers (Mesh dtor)
        LOG_INFO("AssetService", "Evicted LRU mesh: %s (%.2f MB) — reloads "
                 "on next request", v.key.c_str(),
                 v.bytes / (1024.0 * 1024.0));
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
    std::lock_guard<std::mutex> lk(m_async->loadedMtx);
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
        { std::lock_guard<std::mutex> lk(m_async->loadedMtx);
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
            { std::lock_guard<std::mutex> lk(m_async->loadedMtx);
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
            Material mat;
            std::memcpy(mat.baseColorFactor, mg.baseColorFactor,
                        sizeof(mat.baseColorFactor));
            mat.roughness     = mg.roughness;
            mat.metallic      = mg.metallic;
            mat.baseColorName = mg.baseColorName;
            mat.normalMapName = mg.normalMapName;

            if (mg.baseColor.mem) {
                auto th = createTexFromGPU(mg.baseColor);   // format-aware
                if (bgfx::isValid(th)) {
                    mat.baseColorTexture = m_textures.addTexture(
                        Texture(th, mg.baseColor.w, mg.baseColor.h));
                    if (!mg.baseColorName.empty())
                        texByName[mg.baseColorName] = mat.baseColorTexture;
                }
            } else if (!mg.baseColorName.empty()) {
                auto it = texByName.find(mg.baseColorName);
                if (it != texByName.end()) mat.baseColorTexture = it->second;
                // A miss means the FIRST user failed to load; leaving the
                // handle invalid falls back to the white texture, which is
                // the same behaviour as before.
            }

            if (mg.normalMap.mem) {
                auto th = createTexFromGPU(mg.normalMap);
                if (bgfx::isValid(th)) {
                    mat.normalMapTexture = m_textures.addTexture(
                        Texture(th, mg.normalMap.w, mg.normalMap.h));
                    if (!mg.normalMapName.empty())
                        texByName[mg.normalMapName] = mat.normalMapTexture;
                }
            } else if (!mg.normalMapName.empty()) {
                auto it = texByName.find(mg.normalMapName);
                if (it != texByName.end()) mat.normalMapTexture = it->second;
            }

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
            std::lock_guard<std::mutex> lk(m_async->loadedMtx);
            m_async->loadedMeshes[item.key] =
                {h, bytes, ++m_async->useClock};
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
            std::lock_guard<std::mutex> lk(m_async->loadedMtx);
            m_async->loadedTextures[item.key] = handle;
        }
        LOG_SUCCESS("AssetService", "Async texture ready: %s (%ux%u, handle=%u)",
                    item.key.c_str(), item.texData.w, item.texData.h, handle.id);
    }

    return true;
}
