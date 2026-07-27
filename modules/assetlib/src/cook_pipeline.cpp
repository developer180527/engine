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
#include <cstring>
#include <fstream>

#if defined(__APPLE__)
    #include <pthread/qos.h>
    #include <sys/sysctl.h>
#elif defined(__linux__)
    #include <unistd.h>
    #include <pthread.h>
#endif
#if !defined(_WIN32)
    #include <spawn.h>
    #include <sys/wait.h>
    #include <signal.h>
    extern char** environ;
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

#if !defined(_WIN32)
// ── Out-of-process cook ───────────────────────────────────────────────────────
// One asset per child process. The worker writes its outcome to a sidecar
// RESULT FILE (never parsed from stdout — cookers print freely) with a tiny
// line protocol:
//     RESULT ok|skip|fail
//     ERROR  <single-line message>       (optional)
//     OUTPUT <extra output path>         (0..n, mesh's sibling .ctex)
//     DEP    <uuid>                      (0..n)
// Exit-by-signal, a missing/garbled result file, or a deadline overrun all
// become a per-asset failure — the host process (editor!) never dies with it.
CookResult runWorkerProcess(const std::filesystem::path& exe,
                            ICooker* cooker, const CookContext& ctx) {
    namespace fs = std::filesystem;
    const fs::path resultPath = ctx.outputPath.string() + ".result";
    std::error_code ec;
    fs::remove(resultPath, ec);

    // Hard per-task memory cap, applied by the CHILD via setrlimit: a runaway
    // import hits ENOMEM/bad_alloc inside its own process instead of OOM-ing
    // the machine. 2× the estimate with a 1 GB floor (estimates are ceilings,
    // not promises); COOK_TASK_MEM_CAP_MB pins it explicitly.
    long capMb = envLong("COOK_TASK_MEM_CAP_MB", 0);
    if (capMb <= 0)
        capMb = std::max((long)((cooker->estimatePeakBytes(ctx) * 2) >> 20),
                         1024L);

    std::string exeArg = exe.string();
    std::string srcArg = ctx.sourcePath.string();
    std::string outArg = ctx.outputPath.string();
    std::string resArg = resultPath.string();
    std::string capArg = std::to_string(capMb);
    char* argv[] = { exeArg.data(), srcArg.data(), outArg.data(),
                     resArg.data(), capArg.data(), nullptr };

    pid_t pid = -1;
    const int rc = posix_spawn(&pid, exeArg.c_str(), nullptr, nullptr,
                               argv, environ);
    if (rc != 0)
        return { .success=false,
                 .error=std::string("cannot spawn cook worker: ") + std::strerror(rc) };

    // Reap with a deadline. Default is generous — an HQ BC7 8K encode is
    // legitimately minutes — but bounded, so one wedged parse can't hold a
    // worker slot forever (COOK_TASK_TIMEOUT_SEC overrides).
    const long timeoutSec = envLong("COOK_TASK_TIMEOUT_SEC", 3600);
    const auto deadline   = std::chrono::steady_clock::now()
                          + std::chrono::seconds(timeoutSec);
    int  status   = 0;
    bool timedOut = false;
    for (;;) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0)   { status = -1; break; }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timedOut = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto cleanupResult = [&] { std::error_code e; fs::remove(resultPath, e); };

    if (timedOut) {
        cleanupResult();
        return { .success=false,
                 .error="cook timed out after " + std::to_string(timeoutSec)
                      + "s (worker killed)" };
    }
    if (WIFSIGNALED(status)) {
        cleanupResult();
        const int sig = WTERMSIG(status);
        return { .success=false,
                 .error=std::string("cook worker crashed: signal ")
                      + std::to_string(sig) + " (" + strsignal(sig) + ")" };
    }

    std::ifstream f(resultPath);
    if (!f) {
        return { .success=false,
                 .error="cook worker exited (code "
                      + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1)
                      + ") without writing a result" };
    }
    std::string verdict, error, line;
    while (std::getline(f, line)) {
        if      (line.rfind("RESULT ", 0) == 0) verdict = line.substr(7);
        else if (line.rfind("ERROR ", 0)  == 0) error   = line.substr(6);
        else if (line.rfind("OUTPUT ", 0) == 0) {
            if (ctx.addOutput) ctx.addOutput(fs::path(line.substr(7)));
        }
        else if (line.rfind("DEP ", 0) == 0) {
            if (ctx.addDependency)
                ctx.addDependency(UUID::fromString(line.substr(4)));
        }
    }
    f.close();
    cleanupResult();
    if (verdict == "ok")   return { .success=true };
    if (verdict == "skip") return { .success=false, .skipped=true, .error=error };
    if (verdict == "fail") return { .success=false, .error=error };
    return { .success=false, .error="worker result file had no RESULT line" };
}
#endif // !_WIN32

} // namespace

CookResult CookPipeline::dispatchCook(ICooker* cooker,
                                      const CookContext& ctx) const {
#if !defined(_WIN32)
    if (!m_workerExe.empty())
        return runWorkerProcess(m_workerExe, cooker, ctx);
#endif
    // In-process fallback. Exception net: cook() runs third-party parsers
    // (Assimp, stb, json) on corrupt files — a throw escaping a worker
    // std::thread is std::terminate for the whole host process (cooker
    // audit: "Unwrapped Worker Thread Exception Paths"). Convert to a
    // per-asset failure. (Signals still kill us in-process — that is
    // precisely what the out-of-process mode is for.)
    try {
        return cooker->cook(ctx);
    } catch (const std::exception& e) {
        return { .success=false,
                 .error=std::string("cooker threw: ") + e.what() };
    } catch (...) {
        return { .success=false, .error="cooker threw a non-std exception" };
    }
}

CookPipeline::CookPipeline(AssetRegistry& registry,
                           std::filesystem::path projectRoot,
                           std::filesystem::path cacheRoot)
    : m_registry(registry)
    , m_projectRoot(std::move(projectRoot))
    , m_cacheRoot(std::move(cacheRoot))
    , m_ddc() {}   // roots from ENGINE_DDC / ENGINE_DDC_SHARED (or defaults)

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

std::string CookPipeline::sourceHashFor(const AssetRecord& rec) const {
    // scan() keeps sourceHash current (and upgrades legacy FNV to BLAKE3);
    // hash directly only when a record predates the scan or slipped through.
    if (rec.sourceHash.size() == 64) return rec.sourceHash;
    return blake3File(m_projectRoot / rec.sourcePath);
}

std::string CookPipeline::currentKey(const AssetRecord& rec,
                                     ICooker* cooker) const {
    if (!cooker) return {};
    const std::string srcHash = sourceHashFor(rec);
    if (srcHash.empty()) return {};        // unreadable source — no identity

    CookContext ctx;                        // fingerprint may inspect the path
    ctx.uuid       = rec.uuid;
    ctx.sourcePath = m_projectRoot / rec.sourcePath;

    DdcKeyInputs in;
    in.cookerId      = cooker->id();
    in.cookerVersion = cooker->version();
    in.settings      = cooker->settingsFingerprint(ctx);
    if (!rec.importSettings.empty()) {
        in.settings += '\n';
        in.settings += rec.importSettings.json;
    }
    in.sourceHash = srcHash;
    return computeDdcKey(in);
}

bool CookPipeline::isStale(const AssetRecord& rec) const {
    auto ext = std::filesystem::path(rec.sourcePath).extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(c));
    ICooker* cooker = findCooker(ext);
    if (!cooker) return false;              // nothing can cook it

    const std::string key = currentKey(rec, cooker);
    if (key.empty()) return false;          // unreadable source — cooking
                                            // would fail identically
    // Inputs changed since the last attempt (or never attempted).
    if (rec.ddcKey != key) return true;

    // Same inputs as last time. Failed stays failed (retry only when the
    // source/cooker/settings change — or via forceRecook); a Ready record
    // with no cookedPath was deliberately skipped.
    if (rec.state == AssetState::Failed) return false;
    if (rec.cookedPath.empty())          return false;

    // Materialized output vanished (user wiped .cache/) — a DDC hit restores
    // it without recooking.
    std::error_code ec;
    return !std::filesystem::exists(m_cacheRoot / rec.cookedPath, ec);
}

// ── DDC record = manifest of member blobs ────────────────────────────────────
// A cook can produce several files (cooked mesh + sibling .ctex embedded
// textures). Each member is stored under ITS OWN content hash; a small
// manifest under the cook key names them. Fetching materializes every member
// or reports a miss — a hit can never yield a mesh missing its textures.
// Format: "ddc-manifest-v1\n" then "<blobKey>\t<name>\n" per member, where
// name "@" is the primary output and anything else is a sibling filename.

bool CookPipeline::ddcStoreOutputs(const std::string& key,
                                   const std::filesystem::path& primary,
                                   const std::vector<std::filesystem::path>& extras) {
    std::string manifest = "ddc-manifest-v1\n";
    auto addMember = [&](const std::filesystem::path& file,
                         const std::string& name) -> bool {
        const std::string mk = blake3File(file);
        if (mk.empty() || !m_ddc.store(mk, file)) return false;
        manifest += mk; manifest += '\t'; manifest += name; manifest += '\n';
        return true;
    };
    if (!addMember(primary, "@")) return false;
    for (const auto& e : extras)
        if (!addMember(e, e.filename().string())) return false;
    return m_ddc.storeBytes(key, manifest);
}

bool CookPipeline::ddcFetchOutputs(const std::string& key,
                                   const std::filesystem::path& outPath) {
    std::string manifest;
    if (!m_ddc.fetchBytes(key, manifest)) return false;

    size_t pos = manifest.find('\n');
    if (pos == std::string::npos ||
        manifest.compare(0, pos, "ddc-manifest-v1") != 0) return false;
    ++pos;
    while (pos < manifest.size()) {
        size_t eol = manifest.find('\n', pos);
        if (eol == std::string::npos) eol = manifest.size();
        const std::string line = manifest.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) return false;
        const std::string mk   = line.substr(0, tab);
        const std::string name = line.substr(tab + 1);
        // Names are plain sibling filenames — a manifest from a SHARED store
        // is remote input; never let one path-traverse out of the cache dir.
        if (name.empty() || name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos || name.find("..") == 0)
            return false;
        const std::filesystem::path dst =
            (name == "@") ? outPath : outPath.parent_path() / name;
        if (!m_ddc.fetch(mk, dst)) return false;
    }
    return true;
}

// Single writer for the registry outcome of a cook attempt — success, skip,
// and failure previously carried three hand-rolled copies of this logic.
void CookPipeline::commitResult(const UUID& uuid, const CookResult& res,
                                const std::string& key, uint32_t cookerVersion,
                                const std::filesystem::path& outPath,
                                const std::vector<UUID>& deps) {
    auto rec = m_registry.findByUUID(uuid);
    if (!rec) return;

    if (res.success) {
        for (auto& dep : deps) m_registry.addDependency(uuid, dep);
        rec->cookedPath  = std::filesystem::relative(outPath, m_cacheRoot).string();
        rec->state       = AssetState::Ready;
        rec->errorMessage.clear();
    } else {
        // Skip or failure: delete any stale .cooked binary so no code path
        // can accidentally serve it.
        if (!rec->cookedPath.empty()) {
            std::error_code ec;
            std::filesystem::remove(m_cacheRoot / rec->cookedPath, ec);
        }
        rec->cookedPath.clear();
        if (res.skipped) {
            // Cooker can't handle this asset type (e.g. skinned meshes) —
            // Ready with empty cookedPath; the runtime import path serves it.
            rec->state        = AssetState::Ready;
            rec->errorMessage = res.error;   // "why it was skipped"
        } else {
            rec->state        = AssetState::Failed;
            rec->errorMessage = res.error.empty()
                ? "cook failed (no error reported)" : res.error;
        }
    }
    rec->ddcKey      = key;                  // the attempt is now addressed
    rec->cookVersion = cookerVersion;
    rec->cookedAt    = static_cast<int64_t>(std::time(nullptr));
    m_registry.update(*rec);
}

CookResult CookPipeline::cookOne(const UUID& uuid) {
    return cookInternal(uuid, /*useFetch=*/true);
}

CookResult CookPipeline::cookInternal(const UUID& uuid, bool useFetch) {
    auto rec = m_registry.findByUUID(uuid);
    if (!rec) return { .success=false, .error="UUID not found in registry" };
    if (!isStale(*rec)) return { .success=true }; // already fresh

    auto ext = std::filesystem::path(rec->sourcePath).extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(c));
    ICooker* cooker = findCooker(ext);
    if (!cooker) return { .success=false, .error="No cooker registered for extension: " + ext };

    const std::string key = currentKey(*rec, cooker);
    if (key.empty())
        return { .success=false, .error="source unreadable (no content hash)" };

    auto outDir = m_cacheRoot / (assetTypeName(rec->type) + "s");
    std::filesystem::create_directories(outDir);
    auto outPath = outDir / (uuid.toString() + ".cooked");

    // ── Cache hit: someone (this machine, a teammate via the shared tier)
    // already cooked these exact inputs — materialize, done.
    if (useFetch && ddcFetchOutputs(key, outPath)) {
        commitResult(uuid, { .success=true }, key, cooker->version(), outPath, {});
        std::printf("[AssetLib] DDC hit: %s\n", rec->sourcePath.c_str());
        return { .success=true, .cookedPath=outPath.string() };
    }

    // ── Miss: cook to a TEMP file, ingest the result into the DDC, then
    // materialize from the store. Never let a cooker write the final path
    // directly — materialization hardlinks blobs, and an ofstream opened on
    // a hardlinked output would truncate the blob for every project.
    auto tmpPath = outDir / (uuid.toString() + ".cooking");

    CookContext ctx;
    ctx.uuid       = uuid;
    ctx.sourcePath = m_projectRoot / rec->sourcePath;
    ctx.outputPath = tmpPath;
    std::vector<UUID> deps;
    std::vector<std::filesystem::path> extras;
    ctx.addDependency = [&deps](const UUID& dep) { deps.push_back(dep); };
    ctx.addOutput     = [&extras](const std::filesystem::path& p) { extras.push_back(p); };

    auto result = dispatchCook(cooker, ctx);
    if (result.success) {
        if (ddcStoreOutputs(key, tmpPath, extras) && ddcFetchOutputs(key, outPath)) {
            std::error_code ec;
            std::filesystem::remove(tmpPath, ec);
        } else {
            // Store unusable (disk full?) — keep the correct output anyway.
            std::error_code ec;
            std::filesystem::rename(tmpPath, outPath, ec);
            if (ec) result = { .success=false, .error="cannot place cooked output: "
                                                      + ec.message() };
        }
    } else {
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
    }

    commitResult(uuid, result, key, cooker->version(), outPath, deps);
    if (result.success) {
        result.cookedPath = outPath.string();
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
        std::filesystem::path sourcePath, outputPath, tmpPath;
        std::string           sourceRel;
        std::string           key;
        std::vector<UUID>     deps;
        std::vector<std::filesystem::path> extras;
        CookResult            result;
    };

    // ── Phase 1 (caller thread): resolve records into self-contained work,
    // serving DDC hits inline — a hit is a hardlink + a registry row, there
    // is nothing to parallelize.
    std::vector<Work> work;
    work.reserve(uuids.size());
    int hits = 0;
    for (const auto& uuid : uuids) {
        auto rec = m_registry.findByUUID(uuid);
        if (!rec || !isStale(*rec)) continue;
        auto ext = std::filesystem::path(rec->sourcePath).extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(c));
        ICooker* cooker = findCooker(ext);
        if (!cooker) continue;
        const std::string key = currentKey(*rec, cooker);
        if (key.empty()) continue;          // unreadable source

        auto outDir = m_cacheRoot / (assetTypeName(rec->type) + "s");
        std::filesystem::create_directories(outDir);
        auto outPath = outDir / (uuid.toString() + ".cooked");

        if (ddcFetchOutputs(key, outPath)) {
            commitResult(uuid, { .success=true }, key, cooker->version(),
                         outPath, {});
            ++hits;
            if (onResult) onResult(rec->sourcePath, true);
            continue;
        }

        Work w;
        w.uuid       = uuid;
        w.cooker     = cooker;
        w.sourcePath = m_projectRoot / rec->sourcePath;
        w.outputPath = outPath;
        w.tmpPath    = outDir / (uuid.toString() + ".cooking");
        w.sourceRel  = rec->sourcePath;
        w.key        = key;
        work.push_back(std::move(w));
    }
    const int numWork = static_cast<int>(work.size());
    if (hits > 0)
        std::printf("[AssetLib] DDC: %d asset(s) restored from cache\n", hits);
    if (numWork == 0) return hits;

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
            ctx.outputPath    = w.tmpPath;   // NEVER the final path — see cookOne
            ctx.addDependency = [&w](const UUID& dep) { w.deps.push_back(dep); };
            ctx.addOutput     = [&w](const std::filesystem::path& p) {
                w.extras.push_back(p);
            };

            // Reserve this task's estimated peak against the shared budget
            // before touching it — heavy assets wait for headroom, cheap ones
            // stream through. Released the moment the cook returns (or throws).
            const size_t estBytes = w.cooker->estimatePeakBytes(ctx);
            gov.acquire(estBytes);

            // dispatchCook: spawns an isolated worker process when one is
            // configured (crash = one failed asset, hard child memory cap),
            // else cooks in-process behind the exception net.
            w.result = dispatchCook(w.cooker, ctx);
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

    // ── Phase 3 (caller thread): ingest into the DDC + commit registry ────
    int cooked = 0;
    for (auto& w : work) {
        std::error_code ec;
        if (w.result.success) {
            if (ddcStoreOutputs(w.key, w.tmpPath, w.extras)
                    && ddcFetchOutputs(w.key, w.outputPath)) {
                std::filesystem::remove(w.tmpPath, ec);
            } else {
                std::filesystem::rename(w.tmpPath, w.outputPath, ec);
                if (ec) w.result = { .success=false,
                                     .error="cannot place cooked output: " + ec.message() };
            }
        } else {
            std::filesystem::remove(w.tmpPath, ec);
            if (!w.result.skipped)
                std::printf("[AssetLib] Cook FAILED: %s — %s\n",
                            w.sourceRel.c_str(), w.result.error.c_str());
        }
        commitResult(w.uuid, w.result, w.key, w.cooker->version(),
                     w.outputPath, w.deps);
        if (w.result.success) ++cooked;
    }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[AssetLib] Cooked %d/%d asset(s) in %.1f ms on %d worker(s) "
                "(+%d from DDC)\n", cooked, numWork, ms, workers, hits);
    return cooked + hits;
}

CookResult CookPipeline::forceRecook(const UUID& uuid) {
    auto rec = m_registry.findByUUID(uuid);
    if (!rec) return { .success=false, .error="UUID not found" };
    // A force-recook exists because someone suspects the cached output —
    // evict the local blob and cook with the DDC read path bypassed, so we
    // genuinely re-cook instead of re-fetching the very bytes under
    // suspicion. (Shared tier untouched: other machines may be serving from
    // it; ingest there is first-writer-wins, so a poisoned shared blob needs
    // an admin wipe — same as every production DDC.)
    auto ext = std::filesystem::path(rec->sourcePath).extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(c));
    if (ICooker* cooker = findCooker(ext))
        m_ddc.evictLocal(currentKey(*rec, cooker));
    rec->ddcKey.clear();                    // force staleness
    m_registry.update(*rec);
    return cookInternal(uuid, /*useFetch=*/false);
}

} // namespace assetlib
