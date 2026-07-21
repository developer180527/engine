#include "assetlib/cook_pipeline.h"
#include <ctime>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <cstdlib>
#include <cstdio>

#if defined(__APPLE__)
    #include <pthread/qos.h>
    #include <sys/sysctl.h>
#elif defined(__linux__)
    #include <unistd.h>
    #include <pthread.h>
#endif

namespace assetlib {

namespace {

// Total physical RAM in bytes (0 if unknown).
size_t physicalRamBytes() {
#if defined(__APPLE__)
    int64_t mem = 0; size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0 && mem > 0)
        return (size_t)mem;
#elif defined(__linux__)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long psize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && psize > 0) return (size_t)pages * (size_t)psize;
#endif
    return 0;
}

// Drop the calling (cook worker) thread to a background/utility QoS so the OS
// scheduler keeps it BELOW foreground work and clocks the cores down before
// the fans spin up — the cook should be invisible, not a space heater.
void demoteToBackground() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
    // Best-effort niceness; ignored if unsupported.
    nice(10);
#endif
}

// Admits cook work against a byte budget instead of a fixed thread count, so a
// burst of 8K textures / high-poly meshes serializes rather than OOM-ing. A
// task larger than the whole budget is allowed to run ALONE (used==0) so it
// can never deadlock waiting for space that will never exist.
struct MemGovernor {
    std::mutex              m;
    std::condition_variable cv;
    size_t                  budget;
    size_t                  used = 0;
    explicit MemGovernor(size_t b) : budget(b ? b : (size_t)1 << 30) {}

    void acquire(size_t need) {
        need = std::min(need, budget);
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return used == 0 || used + need <= budget; });
        used += need;
    }
    void release(size_t need) {
        need = std::min(need, budget);
        { std::lock_guard<std::mutex> lk(m); used -= need; }
        cv.notify_all();
    }
};

// Reads an integer environment override; returns fallback when unset/invalid.
long envLong(const char* name, long fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    return (end && *end == '\0' && n > 0) ? n : fallback;
}

} // namespace

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
    // Already processed at current cook version — don't retry:
    //  • Failed: cooker tried and reported an error
    //  • Ready with empty cookedPath: deliberately skipped (e.g. skinned
    //    meshes handled by the runtime Assimp path)
    if (rec.cookVersion == kCurrentCookVersion) {
        if (rec.state == AssetState::Failed)
            return false;
        if (rec.state == AssetState::Ready && rec.cookedPath.empty())
            return false;
    }
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
        rec->errorMessage.clear();
        m_registry.update(*rec);
        std::printf("[AssetLib] Cooked: %s\n", rec->sourcePath.c_str());
    } else if (result.skipped) {
        // Cooker can't handle this asset type (e.g. skinned meshes, animation-
        // only files). Mark Ready with empty cookedPath — the runtime Assimp
        // path loads it on demand. Not an error; keep the reason inspectable.
        if (!rec->cookedPath.empty()) {
            auto stale = m_cacheRoot / rec->cookedPath;
            std::error_code ec;
            std::filesystem::remove(stale, ec);
        }
        rec->cookedPath.clear();
        rec->cookVersion  = kCurrentCookVersion;
        rec->state        = AssetState::Ready;
        rec->errorMessage = result.error;    // "why it was skipped"
        m_registry.update(*rec);
    } else {
        // Genuine cook failure. Delete any stale .cooked binary so no code
        // path can accidentally serve it. Persist WHY it failed — an empty
        // error_message made failures undiagnosable from the DB/editor.
        if (!rec->cookedPath.empty()) {
            auto stale = m_cacheRoot / rec->cookedPath;
            std::error_code ec;
            std::filesystem::remove(stale, ec);
        }
        rec->cookedPath.clear();
        rec->cookVersion  = kCurrentCookVersion;
        rec->state        = AssetState::Failed;
        rec->errorMessage = result.error.empty() ? "cook failed (no error reported)"
                                                 : result.error;
        m_registry.update(*rec);
    }
    return result;
}

int CookPipeline::cookAll(std::function<void(int,int)> progress) {
    auto all   = m_registry.all();
    int  total = static_cast<int>(all.size());

    std::vector<UUID> stale;
    for (auto& rec : all) {
        if (isStale(rec)) { stale.push_back(rec.uuid); continue; }
        if (rec.state != AssetState::Ready
                && rec.state != AssetState::Failed) {   // fresh but unmarked
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

    // ── Phase 2 (parallel): pure cook() under thermal + memory governance ──
    // Thermal citizenship: the cook is an OFFLINE background chore, not the
    // foreground app. It must not pin every core (the "melting laptop") nor
    // OOM the box on a pile of 8K assets. Three levers:
    //   • worker cap — leave 2 cores for the OS/editor (COOK_THREADS overrides)
    //   • QoS demotion — workers run at background/utility priority
    //   • memory budget — admit work by estimated peak bytes (COOK_MEM_BUDGET_MB)
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    int workers = (int)envLong("COOK_THREADS", (long)std::max(1u, hw > 2 ? hw - 2 : 1u));
    workers = std::max(1, std::min(workers, numWork));

    const size_t ram        = physicalRamBytes();
    const size_t autoBudget = ram ? ram * 3 / 5 : ((size_t)4 << 30);   // 60% RAM
    const size_t budget     = (size_t)envLong("COOK_MEM_BUDGET_MB",
                                  (long)(autoBudget >> 20)) << 20;
    MemGovernor gov(budget);

    std::atomic<int> next{0};
    std::mutex       cbMtx;
    const auto       t0 = std::chrono::steady_clock::now();
    std::printf("[AssetLib] Cooking %d asset(s) on %d worker(s), mem budget %zu MB...\n",
                numWork, workers, budget >> 20);

    auto run = [&]() {
        demoteToBackground();   // this worker yields to foreground work
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

            // Reserve this task's estimated peak against the shared budget
            // before touching it — heavy assets wait for headroom, cheap ones
            // stream through. Released the moment the cook returns (or throws).
            const size_t estBytes = w.cooker->estimatePeakBytes(ctx);
            gov.acquire(estBytes);

            // Exception net: cook() runs third-party parsers (Assimp, stb,
            // json) on a corrupt file away from any try/catch — a throw
            // here (bad_alloc, parse_error, out_of_range) would terminate
            // this std::thread and take the whole host process with it
            // (cooker audit: "Unwrapped Worker Thread Exception Paths").
            // Convert to a per-asset failure and keep the worker alive.
            try {
                w.result = w.cooker->cook(ctx);
            } catch (const std::exception& e) {
                w.result = {.success = false,
                            .error = std::string("cooker threw: ") + e.what()};
            } catch (...) {
                w.result = {.success = false,
                            .error = "cooker threw a non-std exception"};
            }
            gov.release(estBytes);

            if (onResult) {
                std::lock_guard<std::mutex> lk(cbMtx);
                onResult(w.sourceRel, w.result.success || w.result.skipped);
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
            if (w.result.skipped) {
                // Cooker can't handle this type — mark Ready, not Failed
                if (auto rec = m_registry.findByUUID(w.uuid)) {
                    if (!rec->cookedPath.empty()) {
                        auto stale = m_cacheRoot / rec->cookedPath;
                        std::error_code ec;
                        std::filesystem::remove(stale, ec);
                    }
                    rec->cookedPath.clear();
                    rec->cookVersion  = kCurrentCookVersion;
                    rec->state        = AssetState::Ready;
                    rec->errorMessage = w.result.error;   // "why it was skipped"
                    m_registry.update(*rec);
                }
            } else {
                std::printf("[AssetLib] Cook FAILED: %s — %s\n",
                            w.sourceRel.c_str(), w.result.error.c_str());
                // Mark failed so we don't retry every cook pass.
                // Delete any stale .cooked binary so no code path serves it.
                if (auto rec = m_registry.findByUUID(w.uuid)) {
                    if (!rec->cookedPath.empty()) {
                        auto stale = m_cacheRoot / rec->cookedPath;
                        std::error_code ec;
                        std::filesystem::remove(stale, ec);
                    }
                    rec->cookedPath.clear();
                    rec->cookVersion  = kCurrentCookVersion;
                    rec->state        = AssetState::Failed;
                    rec->errorMessage = w.result.error.empty()
                        ? "cook failed (no error reported)" : w.result.error;
                    m_registry.update(*rec);
                }
            }
            continue;
        }
        for (auto& dep : w.deps) m_registry.addDependency(w.uuid, dep);
        if (auto rec = m_registry.findByUUID(w.uuid)) {
            rec->cookedPath  = std::filesystem::relative(w.outputPath, m_cacheRoot).string();
            rec->cookVersion = kCurrentCookVersion;
            rec->cookedAt    = static_cast<int64_t>(std::time(nullptr));
            rec->state       = AssetState::Ready;
            rec->errorMessage.clear();
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
