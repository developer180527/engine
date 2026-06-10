#include "assets/cookers/cook_service.h"
#include "assets/cookers/mesh_cooker.h"
#include "assets/cookers/texture_cooker.h"
#include "assets/cookers/scene_cooker.h"
#include "core/logger.h"

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
        // Still check for stale scenes even when no mesh/texture changed —
        // the user may have edited scene JSON directly.
        cookSceneFiles(registry, false);
        return;
    }

    LOG_INFO("CookService", "Cooking %d asset(s) in background (parallel)...", total);

    // Collect cookable stale UUIDs and cook them across all cores. cookMany
    // does registry I/O on this thread and runs only cook() on the pool, so
    // the single registry connection stays single-threaded. The callback
    // (serialized) drives progress; shouldContinue stops dispatch on shutdown.
    std::vector<assetlib::UUID> todo;
    todo.reserve(all.size());
    for (auto& rec : all)
        if (isCookable(rec)) todo.push_back(rec.uuid);

    pipeline.cookMany(todo,
        [&](const std::string& src, bool ok) {
            if (ok) {
                ++cooked; m_stats.cooked = cooked;
                LOG_INFO("CookService", "[%d/%d] Cooked: %s", cooked, total, src.c_str());
            } else {
                ++failed; m_stats.failed = failed;
                LOG_WARN("CookService", "Failed: %s", src.c_str());
            }
            std::lock_guard<std::mutex> lk(m_stats.nameMtx);
            m_stats.currentAsset = std::filesystem::path(src).filename().string();
        },
        [this] { return (bool)m_running; });

    m_stats.active = false;
    {
        std::lock_guard<std::mutex> lk(m_stats.nameMtx);
        m_stats.currentAsset.clear();
    }
    LOG_INFO("CookService", "Done — %d cooked, %d failed", cooked, failed);

    // ── Scene cooking ─────────────────────────────────────────────────
    // Scenes are NOT DB-tracked assets — they live in scenes/ as JSON.
    // After individual assets are cooked (so cooked paths are available in
    // the DB), convert any stale JSON scenes into binary .cooked files.
    cookSceneFiles(registry, cooked > 0);
}

void CookService::cookSceneFiles(assetlib::AssetRegistry& registry,
                                 bool assetsChanged) {
    namespace fs = std::filesystem;
    fs::path scenesDir    = m_projectRoot / "scenes";
    fs::path sceneCacheDir = m_cacheRoot  / "scenes";

    if (!fs::exists(scenesDir) || !fs::is_directory(scenesDir)) return;

    std::error_code ec;
    int scenesCooked = 0;

    for (const auto& entry : fs::directory_iterator(scenesDir, ec)) {
        if (!m_running) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".scene") continue;

        fs::path outPath = sceneCacheDir
            / (entry.path().stem().string() + ".cooked");

        // Stale if binary doesn't exist or is older than the JSON source
        bool stale = !fs::exists(outPath);
        if (!stale) {
            std::error_code ec2;
            auto srcTime = fs::last_write_time(entry.path(), ec2);
            auto outTime = fs::last_write_time(outPath, ec2);
            if (!ec2) stale = (srcTime > outTime);
        }
        // If any mesh/texture was cooked this pass, their cooked paths may
        // have changed — re-cook all scenes so they pick up the new paths.
        if (!stale && assetsChanged) stale = true;

        if (!stale) continue;

        if (cookSceneFile(entry.path(), outPath, &registry, m_projectRoot))
            ++scenesCooked;
        else
            LOG_WARN("CookService", "Scene cook failed: %s",
                     entry.path().filename().string().c_str());
    }

    if (scenesCooked > 0)
        LOG_INFO("CookService", "Cooked %d scene(s) to binary", scenesCooked);
}
