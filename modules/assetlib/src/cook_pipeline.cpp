#include "assetlib/cook_pipeline.h"
#include <ctime>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>

namespace assetlib {

CookPipeline::CookPipeline(AssetRegistry& registry,
                           std::filesystem::path projectRoot,
                           std::filesystem::path cacheRoot)
    : m_registry(registry)
    , m_projectRoot(std::move(projectRoot))
    , m_cacheRoot(std::move(cacheRoot)) {}

void CookPipeline::registerCooker(std::unique_ptr<ICooker> cooker) {
    m_cookers.push_back(std::move(cooker));
}

ICooker* CookPipeline::findCooker(const std::string& ext) const {
    for (auto& c : m_cookers)
        for (auto& e : c->extensions())
            if (e == ext) return c.get();
    return nullptr;
}

bool CookPipeline::hasCookerFor(const std::string& ext) const {
    std::string lower = ext;
    for (auto& c : lower) c = static_cast<char>(std::tolower(c));
    return findCooker(lower) != nullptr;
}
bool CookPipeline::isStale(const AssetRecord& rec) const {
    if (rec.cookedPath.empty())                  return true;
    if (rec.cookVersion != kCurrentCookVersion)  return true;
    auto cooked = m_cacheRoot / rec.cookedPath;
    if (!std::filesystem::exists(cooked))        return true;
    if (rec.state == AssetState::Stale)          return true;
    // Source file newer than cooked file — catches edits on disk
    auto source = m_projectRoot / rec.sourcePath;
    if (std::filesystem::exists(source)) {
        auto st = std::filesystem::last_write_time(source);
        auto ct = std::filesystem::last_write_time(cooked);
        if (st > ct) return true;
    }
    return false;
}

CookResult CookPipeline::cookOne(const UUID& uuid) {
    auto rec = m_registry.findByUUID(uuid);
    if (!rec) return { .success=false, .error="UUID not found in registry" };
    if (!isStale(*rec)) return { .success=true }; // already fresh

    auto ext = std::filesystem::path(rec->sourcePath).extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(c));
    ICooker* cooker = findCooker(ext);
    if (!cooker) return { .success=false, .error="No cooker registered for extension: " + ext };

    auto outDir = m_cacheRoot / (assetTypeName(rec->type) + "s");
    std::filesystem::create_directories(outDir);
    auto outPath = outDir / (uuid.toString() + ".cooked");

    CookContext ctx;
    ctx.uuid       = uuid;
    ctx.sourcePath = m_projectRoot / rec->sourcePath;
    ctx.outputPath = outPath;
    ctx.addDependency = [&](const UUID& dep) {
        m_registry.addDependency(uuid, dep);
    };

    auto result = cooker->cook(ctx);
    if (result.success) {
        rec->cookedPath   = std::filesystem::relative(outPath, m_cacheRoot).string();
        rec->cookVersion  = kCurrentCookVersion;
        rec->cookedAt     = static_cast<int64_t>(std::time(nullptr));
        rec->state        = AssetState::Ready;
        m_registry.update(*rec);
        std::printf("[AssetLib] Cooked: %s\n", rec->sourcePath.c_str());
    }
    return result;
}

int CookPipeline::cookAll(std::function<void(int,int)> progress) {
    auto all   = m_registry.all();
    int  total = static_cast<int>(all.size());

    std::vector<UUID> stale;
    for (auto& rec : all) {
        if (isStale(rec)) { stale.push_back(rec.uuid); continue; }
        if (rec.state != AssetState::Ready) {            // fresh but unmarked
            auto r = m_registry.findByUUID(rec.uuid);
            if (r) { r->state = AssetState::Ready; m_registry.update(*r); }
        }
    }

    std::atomic<int> done{ total - static_cast<int>(stale.size()) };
    int cooked = cookMany(stale, [&](const std::string&, bool) {
        if (progress) progress(done.fetch_add(1) + 1, total);
    });
    if (progress) progress(total, total);
    return cooked;
}

int CookPipeline::cookMany(const std::vector<UUID>& uuids,
                           std::function<void(const std::string&, bool)> onResult,
                           std::function<bool()> shouldContinue) {
    struct Work {
        UUID                  uuid;
        ICooker*              cooker = nullptr;
        std::filesystem::path sourcePath, outputPath;
        std::string           sourceRel;
        std::vector<UUID>     deps;
        CookResult            result;
    };

    // ── Phase 1 (caller thread): resolve records into self-contained work ──
    std::vector<Work> work;
    work.reserve(uuids.size());
    for (const auto& uuid : uuids) {
        auto rec = m_registry.findByUUID(uuid);
        if (!rec || !isStale(*rec)) continue;
        auto ext = std::filesystem::path(rec->sourcePath).extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(c));
        ICooker* cooker = findCooker(ext);
        if (!cooker) continue;
        auto outDir = m_cacheRoot / (assetTypeName(rec->type) + "s");
        std::filesystem::create_directories(outDir);
        Work w;
        w.uuid       = uuid;
        w.cooker     = cooker;
        w.sourcePath = m_projectRoot / rec->sourcePath;
        w.outputPath = outDir / (uuid.toString() + ".cooked");
        w.sourceRel  = rec->sourcePath;
        work.push_back(std::move(w));
    }
    const int numWork = static_cast<int>(work.size());
    if (numWork == 0) return 0;

    // ── Phase 2 (parallel): pure cook() across the cores ──────────────────
    const unsigned hw      = std::max(1u, std::thread::hardware_concurrency());
    const int      workers = static_cast<int>(std::min<unsigned>(hw, (unsigned)numWork));
    std::atomic<int> next{0};
    std::mutex       cbMtx;
    const auto       t0 = std::chrono::steady_clock::now();
    std::printf("[AssetLib] Cooking %d asset(s) on %d worker(s)...\n", numWork, workers);

    auto run = [&]() {
        for (;;) {
            if (shouldContinue && !shouldContinue()) break;
            int i = next.fetch_add(1);
            if (i >= numWork) break;
            Work& w = work[i];
            CookContext ctx;
            ctx.uuid          = w.uuid;
            ctx.sourcePath    = w.sourcePath;
            ctx.outputPath    = w.outputPath;
            ctx.addDependency = [&w](const UUID& dep) { w.deps.push_back(dep); };
            w.result = w.cooker->cook(ctx);
            if (onResult) {
                std::lock_guard<std::mutex> lk(cbMtx);
                onResult(w.sourceRel, w.result.success);
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (int t = 0; t < workers; ++t) pool.emplace_back(run);
    for (auto& th : pool) th.join();

    // ── Phase 3 (caller thread): commit registry mutations ────────────────
    int cooked = 0;
    for (auto& w : work) {
        if (!w.result.success) {
            std::printf("[AssetLib] Cook FAILED: %s — %s\n",
                        w.sourceRel.c_str(), w.result.error.c_str());
            continue;
        }
        for (auto& dep : w.deps) m_registry.addDependency(w.uuid, dep);
        if (auto rec = m_registry.findByUUID(w.uuid)) {
            rec->cookedPath  = std::filesystem::relative(w.outputPath, m_cacheRoot).string();
            rec->cookVersion = kCurrentCookVersion;
            rec->cookedAt    = static_cast<int64_t>(std::time(nullptr));
            rec->state       = AssetState::Ready;
            m_registry.update(*rec);
        }
        ++cooked;
    }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[AssetLib] Cooked %d/%d asset(s) in %.1f ms on %d worker(s)\n",
                cooked, numWork, ms, workers);
    return cooked;
}

CookResult CookPipeline::forceRecook(const UUID& uuid) {
    auto rec = m_registry.findByUUID(uuid);
    if (!rec) return { .success=false, .error="UUID not found" };
    // Clear cooked path to force staleness
    rec->cookedPath = "";
    m_registry.update(*rec);
    return cookOne(uuid);
}

} // namespace assetlib
