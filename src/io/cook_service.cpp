#include "io/cook_service.h"
#include "cookers/mesh_cooker.h"
#include "cookers/texture_cooker.h"
#include "engine/logger.h"

CookService::CookService(const std::filesystem::path& dbPath,
                         const std::filesystem::path& projectRoot,
                         const std::filesystem::path& assetsRoot,
                         const std::filesystem::path& cacheRoot)
    : m_dbPath(dbPath), m_projectRoot(projectRoot),
      m_assetsRoot(assetsRoot), m_cacheRoot(cacheRoot) {}

CookService::~CookService() {
    m_running = false;
    m_requestCV.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void CookService::start() {
    m_thread = std::thread([this] { cookLoop(); });
}

void CookService::requestRefresh() {
    {
        std::lock_guard<std::mutex> lk(m_requestMtx);
        ++m_pendingRequests;
    }
    m_requestCV.notify_one();
}

CookService::Stats CookService::stats() const {
    return m_stats.load();
}

void CookService::cookLoop() {
    while (m_running) {
        // Wait for a cook request
        {
            std::unique_lock<std::mutex> lk(m_requestMtx);
            m_requestCV.wait(lk, [this] {
                return m_pendingRequests > 0 || !m_running;
            });
            if (!m_running) break;
            --m_pendingRequests;
        }
        runOneCookPass();
    }
}

void CookService::runOneCookPass() {
    // Open a dedicated registry connection — WAL mode allows concurrent
    // reads from the main thread's registry at the same time.
    assetlib::AssetRegistry registry;
    if (!registry.open(m_dbPath)) {
        LOG_ERROR("CookService", "Failed to open registry at %s",
                  m_dbPath.string().c_str());
        return;
    }

    // Re-scan so newly dropped assets get UUIDs before cooking
    if (std::filesystem::exists(m_assetsRoot))
        registry.scan(m_assetsRoot, m_projectRoot);

    assetlib::CookPipeline pipeline(registry, m_projectRoot, m_cacheRoot);
    pipeline.registerCooker(std::make_unique<MeshCooker>());
    pipeline.registerCooker(std::make_unique<TextureCooker>());

    auto all = registry.all();
    int total  = 0;
    int cooked = 0;
    int failed = 0;

    // Count cookable stale assets (skip unsupported extensions + missing files)
    auto isCookable = [&](const assetlib::AssetRecord& rec) {
        auto ext = std::filesystem::path(rec.sourcePath).extension().string();
        if (!pipeline.hasCookerFor(ext)) return false;
        auto src = m_projectRoot / rec.sourcePath;
        if (!std::filesystem::exists(src)) return false;
        return pipeline.isStale(rec);
    };
    for (auto& rec : all)
        if (isCookable(rec)) ++total;

    m_stats.total  = total;
    m_stats.cooked = 0;
    m_stats.failed = 0;
    m_stats.active = total > 0;

    if (total == 0) {
        LOG_INFO("CookService", "All assets up to date");
        return;
    }

    LOG_INFO("CookService", "Cooking %d asset(s) in background...", total);

    for (auto& rec : all) {
        if (!m_running) break;
        if (!isCookable(rec)) continue;

        {
            std::lock_guard<std::mutex> lk(m_stats.nameMtx);
            m_stats.currentAsset = std::filesystem::path(rec.sourcePath)
                                       .filename().string();
        }

        auto result = pipeline.cookOne(rec.uuid);
        if (result.success) {
            ++cooked;
            m_stats.cooked = cooked;
            LOG_INFO("CookService", "[%d/%d] Cooked: %s",
                     cooked, total, rec.sourcePath.c_str());
        } else {
            ++failed;
            m_stats.failed = failed;
            LOG_WARN("CookService", "Failed: %s — %s",
                     rec.sourcePath.c_str(), result.error.c_str());
        }
    }

    m_stats.active = false;
    {
        std::lock_guard<std::mutex> lk(m_stats.nameMtx);
        m_stats.currentAsset.clear();
    }
    LOG_INFO("CookService", "Done — %d cooked, %d failed", cooked, failed);
}
