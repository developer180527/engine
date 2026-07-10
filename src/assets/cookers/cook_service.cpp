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

// A source file counts as "settled" once its mtime is at least this old.
// External DCC tools (Blender, Substance, Photoshop) write big exports over
// hundreds of ms; cooking a file mid-write reads a truncated asset — false
// cook failures or corrupt output (cooker audit: "Race Condition on
// Unfinished File Writes"). Deferred files trigger a follow-up pass.
static bool fileSettled(const std::filesystem::path& p) {
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(p, ec);
    if (ec) return false;                        // vanished mid-scan — skip
    const auto age = std::filesystem::file_time_type::clock::now() - mtime;
    return age >= std::chrono::milliseconds(750);
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
        // Exception net (cooker audit: "Unwrapped Worker Thread Exception
        // Paths"): a throw escaping this std::thread is std::terminate for
        // the whole editor. Third-party code inside the pass (Assimp, JSON,
        // sqlite, allocations) can throw on corrupt input — log, keep the
        // service alive, wait for the next request.
        try {
            runOneCookPass();
        } catch (const std::exception& e) {
            LOG_ERROR("CookService", "cook pass crashed: %s — service "
                      "still alive, will retry on next refresh", e.what());
            m_stats.active = false;
        } catch (...) {
            LOG_ERROR("CookService", "cook pass crashed (non-std exception) "
                      "— service still alive");
            m_stats.active = false;
        }
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

    // Collect cookable stale assets ONCE (skip unsupported extensions +
    // missing files). Files still being written by an external tool are
    // DEFERRED — cooked on a follow-up pass once their mtime settles.
    int deferred = 0;
    std::vector<assetlib::UUID> todo;
    todo.reserve(all.size());
    for (auto& rec : all) {
        auto ext = std::filesystem::path(rec.sourcePath).extension().string();
        if (!pipeline.hasCookerFor(ext)) continue;
        auto src = m_projectRoot / rec.sourcePath;
        if (!std::filesystem::exists(src)) continue;
        if (!pipeline.isStale(rec)) continue;
        if (!fileSettled(src)) { ++deferred; continue; }
        todo.push_back(rec.uuid);
    }
    total = (int)todo.size();

    // Assets that failed at the current cook version are NOT retried (that
    // would loop forever) — but they must not be silently reported as "up to
    // date" either. Surface them every pass until the source is fixed.
    int standingFailures = 0;
    for (auto& rec : all)
        if (rec.state == assetlib::AssetState::Failed) {
            ++standingFailures;
            LOG_WARN("CookService", "Still failing: %s — %s (resave/fix the file to retry)",
                     rec.sourcePath.c_str(),
                     rec.errorMessage.empty() ? "no error recorded" : rec.errorMessage.c_str());
        }

    m_stats.total  = total;
    m_stats.cooked = 0;
    m_stats.failed = standingFailures;
    m_stats.active = total > 0;

    if (total == 0) {
        if (standingFailures > 0)
            LOG_WARN("CookService", "Up to date, but %d asset(s) in FAILED state", standingFailures);
        else if (deferred == 0)
            LOG_INFO("CookService", "All assets up to date");
        // Still check for stale scenes even when no mesh/texture changed —
        // the user may have edited scene JSON directly.
        cookSceneFiles(registry);
        requeueIfDeferred(deferred);
        return;
    }

    LOG_INFO("CookService", "Cooking %d asset(s) in background (parallel)...", total);

    // Cook the collected UUIDs across the cores. cookMany does registry I/O
    // on this thread and runs only cook() on the pool, so the single
    // registry connection stays single-threaded. The callback (serialized)
    // drives progress; shouldContinue stops dispatch on shutdown.
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
    cookSceneFiles(registry);
    requeueIfDeferred(deferred);
}

void CookService::requeueIfDeferred(int deferred) {
    if (deferred <= 0 || !m_running) return;
    LOG_INFO("CookService", "%d file(s) still being written — retrying "
             "shortly", deferred);
    // Give the external writer time to finish, then run another pass. The
    // settle check converges: once mtimes stop moving, the follow-up pass
    // cooks them and defers nothing.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    requestRefresh();
}

void CookService::cookSceneFiles(assetlib::AssetRegistry& registry) {
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

        // A scene mid-save by the editor/user gets the same settle
        // treatment as assets — never cook a half-written JSON.
        if (!fileSettled(entry.path())) continue;   // next pass picks it up

        // Stale if binary doesn't exist or is older than the JSON source
        bool stale = !fs::exists(outPath);
        std::filesystem::file_time_type outTime{};
        if (!stale) {
            std::error_code ec2;
            auto srcTime = fs::last_write_time(entry.path(), ec2);
            outTime      = fs::last_write_time(outPath, ec2);
            if (!ec2) stale = (srcTime > outTime);
        }
        // Per-scene dependency check (replaces the global assetsChanged
        // flag, which re-cooked EVERY scene in the workspace whenever ANY
        // single asset changed — cooker audit): only re-cook this scene if
        // one of ITS OWN referenced assets has a newer cooked output.
        if (!stale && sceneDependsOnNewerAssets(entry.path(), outTime,
                                                &registry, m_projectRoot,
                                                m_cacheRoot))
            stale = true;

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
