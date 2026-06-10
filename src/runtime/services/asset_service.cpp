#include "runtime/services/asset_service.h"

#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "render/mesh.h"
#include "render/texture.h"
#include "render/material.h"
#include "render/vertex.h"
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

// Pre-copied texture pixels (bgfx::copy on worker thread, handle on main)
struct TexGPU {
    const bgfx::Memory* mem = nullptr;
    uint16_t w = 0, h = 0;
};

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

    // Loaded cache — written by main thread in drain, read by query*()
    mutable std::mutex                              loadedMtx;
    std::unordered_map<std::string, MeshHandle>     loadedMeshes;
    std::unordered_map<std::string, TextureHandle>  loadedTextures;
};

// ═══════════════════════════════════════════════════════════════════════════
// Construction / Destruction
// ═══════════════════════════════════════════════════════════════════════════

AssetService::AssetService(Config cfg)
    : m_meshes(cfg.meshes)
    , m_textures(cfg.textures)
    , m_materials(cfg.materials)
    , m_assetLib(cfg.assetLib)
    , m_projectRoot(cfg.projectRoot)
    , m_cacheRoot(cfg.projectRoot.empty() ? std::filesystem::path{}
                                          : cfg.projectRoot / ".cache")
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

    TexGPU out;
    out.mem = bgfx::copy(texAsset.pixels.data(),
                          static_cast<uint32_t>(texAsset.pixels.size()));
    out.w   = static_cast<uint16_t>(texAsset.header.width);
    out.h   = static_cast<uint16_t>(texAsset.header.height);
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// Sync loadMesh
// ═══════════════════════════════════════════════════════════════════════════

MeshHandle AssetService::loadMesh(const char* cookedPath) {
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
    if (hdr.vertexStride != sizeof(Vertex)) {
        LOG_ERROR("AssetService",
            "Stride mismatch: cooked=%u runtime=%zu — re-cook needed: %s",
            hdr.vertexStride, sizeof(Vertex), cookedPath);
        return {};
    }

    auto* vertexMem = bgfx::copy(asset.vertexData.data(),
                                  static_cast<uint32_t>(asset.vertexData.size()));
    auto* indexMem  = bgfx::copy(asset.indexData.data(),
                                  static_cast<uint32_t>(asset.indexData.size()));

    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(vertexMem,
                                                              Vertex::layout());
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
    assetlib::TextureAsset texAsset;
    if (!assetlib::loadTexture(texAsset, absPath)) {
        LOG_ERROR("AssetService", "Failed to load cooked texture: %s",
                  absPath.string().c_str());
        return {};
    }

    auto* mem = bgfx::copy(texAsset.pixels.data(),
                            static_cast<uint32_t>(texAsset.pixels.size()));

    bgfx::TextureHandle th = bgfx::createTexture2D(
        static_cast<uint16_t>(texAsset.header.width),
        static_cast<uint16_t>(texAsset.header.height),
        false, 1, bgfx::TextureFormat::RGBA8, 0, mem);

    if (!bgfx::isValid(th)) {
        LOG_ERROR("AssetService", "bgfx texture creation failed: %s",
                  absPath.string().c_str());
        return {};
    }

    Texture tex(th,
                static_cast<uint16_t>(texAsset.header.width),
                static_cast<uint16_t>(texAsset.header.height));

    TextureHandle handle = m_textures.addTexture(std::move(tex));
    LOG_INFO("AssetService", "Loaded texture: %s (%ux%u, handle=%u)",
             absPath.filename().string().c_str(),
             texAsset.header.width, texAsset.header.height, handle.id);
    return handle;
}

TextureHandle AssetService::resolveTexture(
        const char* texPath, const std::filesystem::path& sourceDir) {
    if (!texPath || texPath[0] == '\0') return {};

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
                const auto sourceDir = std::filesystem::path(req.absPath).parent_path();
                for (uint32_t mi = 0; mi < static_cast<uint32_t>(asset.materials.size()); ++mi) {
                    const auto& cm = asset.materials[mi];
                    MatGPU mg;
                    std::memcpy(mg.baseColorFactor, cm.baseColorFactor, 16);
                    mg.roughness = cm.roughness;
                    mg.metallic  = cm.metallic;

                    if (cm.flags & assetlib::kMatFlag_HasBaseColor) {
                        mg.baseColor     = resolveTextureGPU(cm.baseColorPath,
                                               sourceDir, m_assetLib,
                                               m_projectRoot, m_cacheRoot);
                        mg.baseColorName = cm.baseColorPath;
                    }
                    if (cm.flags & assetlib::kMatFlag_HasNormalMap) {
                        mg.normalMap     = resolveTextureGPU(cm.normalMapPath,
                                               sourceDir, m_assetLib,
                                               m_projectRoot, m_cacheRoot);
                        mg.normalMapName = cm.normalMapPath;
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
                result.texData.mem = bgfx::copy(texAsset.pixels.data(),
                                     static_cast<uint32_t>(texAsset.pixels.size()));
                result.texData.w   = static_cast<uint16_t>(texAsset.header.width);
                result.texData.h   = static_cast<uint16_t>(texAsset.header.height);
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

    // Already loaded?
    {
        std::lock_guard<std::mutex> lk(m_async->loadedMtx);
        if (m_async->loadedMeshes.count(key)) return;
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
    return (it != m_async->loadedMeshes.end()) ? it->second.id : 0;
}

uint32_t AssetService::queryTexture(const char* cookedPath) const {
    if (!m_async || !cookedPath) return 0;
    std::string key = resolvePath(cookedPath);
    std::lock_guard<std::mutex> lk(m_async->loadedMtx);
    auto it = m_async->loadedTextures.find(key);
    return (it != m_async->loadedTextures.end()) ? it->second.id : 0;
}

bool AssetService::isLoading(const char* cookedPath) const {
    if (!m_async || !cookedPath) return false;
    std::string key = resolvePath(cookedPath);
    std::lock_guard<std::mutex> lk(m_async->pendingMtx);
    return m_async->inFlight.count(key) > 0;
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
            return true;
        }

        Mesh mesh(vbh, ibh, item.indexCount);
        mesh.boundsMin  = {item.boundsMin[0], item.boundsMin[1], item.boundsMin[2]};
        mesh.boundsMax  = {item.boundsMax[0], item.boundsMax[1], item.boundsMax[2]};
        mesh.sourcePath = item.sourcePath;

        // Finalize materials
        std::vector<MaterialHandle> matHandles;
        for (auto& mg : item.materials) {
            Material mat;
            std::memcpy(mat.baseColorFactor, mg.baseColorFactor,
                        sizeof(mat.baseColorFactor));
            mat.roughness     = mg.roughness;
            mat.metallic      = mg.metallic;
            mat.baseColorName = mg.baseColorName;
            mat.normalMapName = mg.normalMapName;

            if (mg.baseColor.mem) {
                auto th = bgfx::createTexture2D(
                    mg.baseColor.w, mg.baseColor.h,
                    false, 1, bgfx::TextureFormat::RGBA8, 0, mg.baseColor.mem);
                if (bgfx::isValid(th))
                    mat.baseColorTexture = m_textures.addTexture(
                        Texture(th, mg.baseColor.w, mg.baseColor.h));
            }
            if (mg.normalMap.mem) {
                auto th = bgfx::createTexture2D(
                    mg.normalMap.w, mg.normalMap.h,
                    false, 1, bgfx::TextureFormat::RGBA8, 0, mg.normalMap.mem);
                if (bgfx::isValid(th))
                    mat.normalMapTexture = m_textures.addTexture(
                        Texture(th, mg.normalMap.w, mg.normalMap.h));
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
            std::lock_guard<std::mutex> lk(m_async->loadedMtx);
            m_async->loadedMeshes[item.key] = h;
        }
        LOG_SUCCESS("AssetService", "Async mesh ready: %s (handle=%u)",
                    item.key.c_str(), h.id);

    } else {
        // ── Finalize texture ────────────────────────────────────────
        bgfx::TextureHandle th = bgfx::createTexture2D(
            item.texData.w, item.texData.h,
            false, 1, bgfx::TextureFormat::RGBA8, 0, item.texData.mem);

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
