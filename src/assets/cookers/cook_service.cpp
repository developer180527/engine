#include "assets/cookers/cook_service.h"
#include "assets/cookers/mesh/mesh_cooker.h"
#include "assets/cookers/texture/texture_cooker.h"
#include "assets/cookers/scene/scene_cooker.h"
#include "assets/cookers/shader/shader_cooker.h"
#include "assets/cookers/material/material_cooker.h"
#include "assets/asset_path.h"
#include "core/logger.h"
#include <assetlib/scene_asset.h>   // header peek: version-aware staleness
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <utility>

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
    const auto size  = std::filesystem::file_size(p, ec);
    if (ec) return false;

    // mtime-age alone is not enough: a slow export over a network drive or a
    // saturated disk can stall >750ms *between* chunks, so the file looks old
    // while it is still growing, and we cook a truncated asset (cooker audit:
    // "750ms Burst-Write Trap"). Require the (size, mtime) pair to be UNCHANGED
    // since the previous poll as well — a file mid-write fails this on the pass
    // where its size moved, and only settles once writing has actually stopped.
    static std::mutex mtx;
    static std::unordered_map<std::string,
        std::pair<uintmax_t, std::filesystem::file_time_type>> seen;
    {
        std::lock_guard<std::mutex> lk(mtx);
        auto& prev = seen[p.string()];
        const bool stable = (prev.first == size && prev.second == mtime);
        prev = {size, mtime};
        if (!stable) return false;               // first sight or still moving
    }

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

std::vector<std::filesystem::path> CookService::sceneDirs() const {
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs;
    for (fs::path d : {m_projectRoot / "scenes", m_assetsRoot / "scenes"})
        if (fs::exists(d) && fs::is_directory(d))
            dirs.push_back(std::move(d));
    return dirs;
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
    pipeline.registerCooker(std::make_unique<ShaderCooker>());
    pipeline.registerCooker(std::make_unique<MaterialCooker>());

    // Out-of-process cooking: run each cook in an engine_cook_worker child
    // (crash isolation + hard per-task memory caps). The worker ships next
    // to whatever spawned us (editor, engine_cook — both at the build/bin
    // root). Missing binary or COOK_INPROC=1 falls back to in-process,
    // loudly — silently losing crash isolation is how a "stable" editor
    // starts dying on corrupt FBX imports again.
    const char* inproc = std::getenv("COOK_INPROC");
    if (!(inproc && *inproc && inproc[0] != '0')) {
        const auto worker = asset_path::executableDir() / "engine_cook_worker";
        std::error_code ec;
        if (std::filesystem::exists(worker, ec)) {
            pipeline.setWorkerExecutable(worker);
        } else {
            LOG_WARN("CookService", "engine_cook_worker not found at %s — "
                     "cooking IN-PROCESS (no crash isolation)",
                     worker.string().c_str());
        }
    }

    auto all = registry.all();
    int total  = 0;
    int cooked = 0;
    int failed = 0;

    // Collect cookable stale assets ONCE (skip unsupported extensions +
    // missing files). Files still being written by an external tool are
    // DEFERRED — cooked on a follow-up pass once their mtime settles.
    // On-demand scope: when restricted to the scene closure, cook only the
    // meshes (and thus their sibling textures) the project's .scene files
    // actually reference — not every asset that happens to live in assets/.
    // This is what keeps a 3-mesh scene from cooking a 637-asset kit.
    std::optional<std::unordered_set<std::string>> scope;
    if (m_scope == Scope::SceneClosure) {
        scope = collectSceneAssetClosure(sceneDirs(), &registry, m_projectRoot);
        LOG_INFO("CookService", "Scene-scoped cook: %zu asset(s) in closure",
                 scope->size());
    }

    int deferred = 0, backfilled = 0;
    std::vector<assetlib::UUID> todo;
    std::unordered_set<std::string> todoSet;   // scene→asset edge lookup
    todo.reserve(all.size());
    for (auto& rec : all) {
        if (scope && !scope->count(rec.uuid.toString())) continue;
        auto ext = std::filesystem::path(rec.sourcePath).extension().string();
        if (!pipeline.hasCookerFor(ext)) continue;
        auto src = m_projectRoot / rec.sourcePath;
        if (!std::filesystem::exists(src)) continue;
        if (!pipeline.isStale(rec)) {
            // Up to date — but this is also the ONLY place a fresh asset is
            // ever visited, so it is where a warm .cache beside a cold DDC
            // gets noticed. Ingest it now; otherwise the next .cache wipe
            // re-cooks output that was on disk all along, and this machine
            // never contributes to the shared tier. No-op (one stat) when the
            // DDC already has it, which is the normal case.
            if (pipeline.backfillDdc(rec)) ++backfilled;
            continue;
        }
        if (!fileSettled(src)) { ++deferred; continue; }
        todo.push_back(rec.uuid);
        todoSet.insert(rec.uuid.toString());
    }
    total = (int)todo.size();
    if (backfilled > 0)
        LOG_INFO("CookService", "DDC: back-filled %d up-to-date asset(s)",
                 backfilled);

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

    m_stats.total    = total;
    m_stats.cooked   = 0;
    m_stats.failed   = standingFailures;
    m_stats.deferred = deferred;   // lets the synchronous cookOnce() converge
    m_stats.active   = total > 0;

    // ── Scene tasks ────────────────────────────────────────────────────
    // Scenes are NOT DB-tracked assets — they live in scenes/ as JSON. They
    // join the SAME graph as the assets, each with dependency edges on
    // exactly the cooking assets it references: a scene cooks the moment
    // its own assets land (and immediately, in parallel, when none are
    // cooking) instead of every scene waiting for the whole batch.
    int scenesCooked = 0, scenesFailed = 0, scenesDeferred = 0;
    auto sceneTasks = buildSceneTasks(registry, todoSet,
                                      &scenesCooked, &scenesFailed,
                                      &scenesDeferred);
    // Fold scene deferrals into the pass's deferred total so the synchronous
    // cookOnce() loop keeps going until they settle. Assigned to m_stats HERE,
    // after the scene scan: the earlier assignment only knew about assets, so
    // a deferred scene was invisible to the convergence check.
    deferred += scenesDeferred;
    m_stats.deferred = deferred;

    if (total == 0 && sceneTasks.empty()) {
        if (standingFailures > 0)
            LOG_WARN("CookService", "Up to date, but %d asset(s) in FAILED state", standingFailures);
        else if (deferred == 0)
            LOG_INFO("CookService", "All assets up to date");
        requeueIfDeferred(deferred);
        return;
    }

    if (total > 0)
        LOG_INFO("CookService", "Cooking %d asset(s) + %zu scene(s) as a task "
                 "graph...", total, sceneTasks.size());

    // One graph run: cookGraph does registry I/O on this thread (the drain
    // lane) and runs cook()/DDC ingest on the pool, so the single registry
    // connection stays single-threaded. The callback (drain lane) drives
    // progress; shouldContinue stops dispatch on shutdown.
    pipeline.cookGraph(todo, std::move(sceneTasks),
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
    if (total > 0)
        LOG_INFO("CookService", "Done — %d cooked, %d failed", cooked, failed);
    if (scenesCooked > 0)
        LOG_INFO("CookService", "Cooked %d scene(s) to binary", scenesCooked);
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

CookService::GcStats CookService::collectGarbage(bool prune) {
    namespace fs = std::filesystem;
    GcStats st;

    // FAIL CLOSED, and do NOT trust open() to report this. AssetRegistry::open
    // CREATES the database when the path is missing, so a wiped or
    // never-created registry opens "successfully" with zero records — and a GC
    // that trusts that concludes nothing is referenced and deletes the entire
    // cache. Verified: with registry.db moved aside, an earlier version of this
    // function happily removed 8 of 9 files.
    std::error_code dbEc;
    if (!fs::exists(m_dbPath, dbEc)) {
        LOG_ERROR("CookService", "GC: no registry at %s — refusing to delete "
                  "anything, since nothing is known to be referenced",
                  m_dbPath.string().c_str());
        return st;
    }

    assetlib::AssetRegistry registry;
    if (!registry.open(m_dbPath)) {
        LOG_ERROR("CookService", "GC: cannot open registry at %s — refusing to "
                  "delete anything without knowing what is referenced",
                  m_dbPath.string().c_str());
        return st;
    }

    // ── 1. What is still referenced ─────────────────────────────────────────
    const auto records = registry.all();

    // Second guard: an EMPTY registry next to a populated cache means the DB
    // was rebuilt (or is mid-scan), not that every asset was deleted. Treating
    // "no records" as "no references" is how a GC eats a whole cache, so the
    // safe reading of an empty registry is "I don't know yet".
    if (records.empty()) {
        LOG_WARN("CookService", "GC: registry has no assets — refusing to "
                 "treat that as 'nothing referenced' (run a cook first)");
        return st;
    }

    std::unordered_set<std::string> keep;
    MeshCooker meshCooker;              // only meshes have sibling outputs

    for (const auto& rec : records) {
        if (rec.cookedPath.empty()) continue;
        const auto primary = m_cacheRoot / rec.cookedPath;
        keep.insert(primary.lexically_normal().generic_string());

        // A cooked mesh owns sibling .ctex files. Ask the cooker to re-derive
        // them from the mesh's own material table rather than guessing by
        // filename — the same exactness the DDC dedup needs, and the reason
        // ICooker::enumerateOutputs exists.
        std::error_code ec;
        if (!fs::exists(primary, ec)) continue;
        std::vector<fs::path> extras;
        meshCooker.enumerateOutputs(primary, extras);
        for (const auto& e : extras)
            keep.insert(e.lexically_normal().generic_string());
    }

    // Cooked scenes are NOT registry-tracked (they live in scenes/, not the
    // asset DB), so they are kept when their source .scene still exists.
    for (const auto& dir : sceneDirs()) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".scene") continue;
            const auto cooked = m_cacheRoot / "scenes"
                              / (e.path().stem().string() + ".cooked");
            keep.insert(cooked.lexically_normal().generic_string());
        }
    }

    // ── 2. Sweep — allowlist only ───────────────────────────────────────────
    // ONLY files this GC positively understands are candidates. Everything
    // else survives by default: registry.db (and its -wal/-shm, deleting which
    // would corrupt an open database), anim/*.ozzclip (the ClipLibrary's cache,
    // keyed by a different scheme this code cannot evaluate), and anything a
    // future cooker adds before this function learns about it. A GC that
    // deletes what it does not recognise is how caches eat real data.
    std::error_code ec;
    if (!fs::exists(m_cacheRoot, ec)) return st;

    std::vector<fs::path> victims;
    for (auto it = fs::recursive_directory_iterator(m_cacheRoot, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const auto& p   = it->path();
        const auto  ext = p.extension().string();
        if (ext != ".cooked" && ext != ".ctex") continue;   // allowlist

        const auto norm  = p.lexically_normal().generic_string();
        const auto bytes = (uint64_t)fs::file_size(p, ec);
        if (keep.count(norm)) { ++st.keptFiles;   st.keptBytes   += bytes; }
        else                  { ++st.orphanFiles; st.orphanBytes += bytes;
                                victims.push_back(p); }
    }

    // ── 3. Report, then (only if asked) delete ──────────────────────────────
    const double mb = 1.0 / (1024.0 * 1024.0);
    LOG_INFO("CookService",
             "GC: %d referenced (%.1f MB), %d orphaned (%.1f MB)%s",
             st.keptFiles, (double)st.keptBytes * mb,
             st.orphanFiles, (double)st.orphanBytes * mb,
             prune ? "" : "  [dry run — pass --gc-prune to delete]");

    for (const auto& v : victims) {
        if (!prune) {
            std::printf("  orphan  %s\n",
                        fs::relative(v, m_cacheRoot, ec).generic_string().c_str());
            continue;
        }
        std::error_code rmEc;
        const auto bytes = (uint64_t)fs::file_size(v, rmEc);
        if (fs::remove(v, rmEc)) { ++st.deleted; st.freedBytes += bytes; }
        else LOG_WARN("CookService", "GC: could not delete %s: %s",
                      v.string().c_str(), rmEc.message().c_str());
    }
    if (prune)
        // "Logical" is not pedantry. Materialized outputs are HARDLINKS to DDC
        // blobs, so removing a .cache entry drops one link, not the inode —
        // real disk is only reclaimed once the DDC's copy goes too (that store
        // has its own size/age policy). Measured on fps_shooter: 72.7 MB of
        // orphans removed, total disk unchanged at 55 MB. What this reclaims
        // for certain is a coherent working directory; claiming freed disk
        // would be a lie a user could check.
        LOG_INFO("CookService", "GC: deleted %d file(s), %.1f MB logical "
                 "(hardlinks to DDC blobs — real disk frees when the DDC "
                 "evicts them). Re-cook or a DDC hit restores anything wanted.",
                 st.deleted, (double)st.freedBytes * mb);
    return st;
}

std::vector<assetlib::CookPipeline::ExtraTask> CookService::buildSceneTasks(
        assetlib::AssetRegistry& registry,
        const std::unordered_set<std::string>& cooking,
        int* scenesCooked, int* scenesFailed, int* scenesDeferred) {
    namespace fs = std::filesystem;
    std::vector<assetlib::CookPipeline::ExtraTask> tasks;
    fs::path sceneCacheDir = m_cacheRoot / "scenes";

    // Scenes live in either <project>/scenes (canonical v2 layout) or
    // <assets>/scenes (editor-authored projects — fps_shooter's layout).
    // Scanning only the former silently skipped the latter: those scenes
    // were only ever cooked by editor saves, so format upgrades never
    // reached them.
    std::vector<fs::path> dirs = sceneDirs();
    if (dirs.empty()) return tasks;

    std::error_code ec;
    for (const auto& scenesDir : dirs)
    for (const auto& entry : fs::directory_iterator(scenesDir, ec)) {
        if (!m_running) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".scene") continue;

        fs::path outPath = sceneCacheDir
            / (entry.path().stem().string() + ".cooked");

        // A scene mid-save by the editor/user gets the same settle
        // treatment as assets — never cook a half-written JSON. Reported so
        // cookOnce() runs another pass instead of concluding "up to date":
        // fileSettled()'s stability map is per-process, so the FIRST sight of
        // any scene always defers, and a one-pass CLI would never cook it.
        if (!fileSettled(entry.path())) {
            if (scenesDeferred) ++*scenesDeferred;
            continue;
        }

        // This scene's asset refs, intersected with what's cooking NOW —
        // the graph edges that let it cook the moment its assets land.
        std::vector<assetlib::UUID> waitFor;
        if (!cooking.empty())
            for (const auto& u : collectSceneRefs(entry.path(), &registry,
                                                  m_projectRoot))
                if (cooking.count(u))
                    waitFor.push_back(assetlib::UUID::fromString(u));

        // Stale if binary doesn't exist, is older than the JSON source, or
        // was cooked by an older FORMAT version (header peek is cheap and
        // means a version bump re-cooks the workspace automatically).
        bool stale = !fs::exists(outPath);
        std::filesystem::file_time_type outTime{};
        if (!stale) {
            std::error_code ec2;
            auto srcTime = fs::last_write_time(entry.path(), ec2);
            outTime      = fs::last_write_time(outPath, ec2);
            if (!ec2) stale = (srcTime > outTime);
        }
        if (!stale) {
            std::ifstream f(outPath, std::ios::binary);
            assetlib::SceneHeader hdr{};
            f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
            if (!f || hdr.magic != assetlib::kSceneMagic
                   || hdr.version != assetlib::kSceneVersion)
                stale = true;
        }
        // Per-scene dependency check (replaces the global assetsChanged
        // flag, which re-cooked EVERY scene in the workspace whenever ANY
        // single asset changed — cooker audit): only re-cook this scene if
        // one of ITS OWN referenced assets has a newer cooked output —
        // or is about to (its refs are in the cook set).
        if (!stale && waitFor.empty()
                   && !sceneDependsOnNewerAssets(entry.path(), outTime,
                                                 &registry, m_projectRoot,
                                                 m_cacheRoot))
            continue;

        assetlib::CookPipeline::ExtraTask t;
        t.name    = entry.path().filename().string();
        t.waitFor = std::move(waitFor);
        // run() executes on the worker pool, possibly concurrent with the
        // drain lane's registry writes — open a PRIVATE read connection
        // (WAL: one writer + N readers). Everything captured by value; the
        // caller-thread registry is never touched from here.
        t.run = [jsonPath = entry.path(), outPath, dbPath = m_dbPath,
                 projectRoot = m_projectRoot]() -> bool {
            assetlib::AssetRegistry reg;
            if (!reg.open(dbPath))
                return cookSceneFile(jsonPath, outPath, nullptr, projectRoot);
            return cookSceneFile(jsonPath, outPath, &reg, projectRoot);
        };
        t.onDone = [name = t.name, scenesCooked, scenesFailed](bool ok) {
            if (ok) { ++*scenesCooked; }
            else    { ++*scenesFailed;
                      LOG_WARN("CookService", "Scene cook failed: %s",
                               name.c_str()); }
        };
        tasks.push_back(std::move(t));
    }
    return tasks;
}
