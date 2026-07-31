#pragma once
#include <assetlib/asset_registry.h>
#include <assetlib/cook_pipeline.h>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>
#include <unordered_set>
#include <vector>

// CookService — runs the cook pipeline on a background thread so the
// editor opens immediately. Opens its own DB connection (WAL mode allows
// concurrent reads from the main thread's registry + writes here).
class CookService {
public:
    struct Stats {
        int  total    = 0;
        int  cooked   = 0;
        int  failed   = 0;
        int  deferred = 0;   // files not-yet-settled this pass (mid-write)
        bool active   = false;
        std::string currentAsset;
    };

    CookService(const std::filesystem::path& dbPath,
                const std::filesystem::path& projectRoot,
                const std::filesystem::path& assetsRoot,
                const std::filesystem::path& cacheRoot);
    ~CookService();

    // Launch background cook thread — call once after editor opens
    void start();

    // Cumulative result of a whole cookOnce() run, summed across its passes.
    // NOT the same as stats(), which is a live per-pass snapshot for UI polling.
    struct RunSummary { int cooked = 0; int failed = 0; };

    // Run synchronous cook passes on the calling thread until they converge —
    // no background thread. For CLI tooling (engine_cook).
    //
    // Loops rather than running a single pass because fileSettled() defers any
    // file whose (size, mtime) it hasn't seen stable across two polls — the
    // burst-write guard. On a static disk a file settles on the very next pass;
    // a real in-flight export settles once its writer stops. We keep going
    // while a pass still cooks or defers work, with a short sleep so genuinely
    // active writes can advance, and a hard cap so a perpetually-written file
    // can't spin us forever.
    //
    // Returns the CUMULATIVE totals: reading stats() after this call reports
    // the LAST pass, and convergence requires that pass to have cooked nothing
    // — so a caller printing stats().cooked always said "0 cooked" no matter
    // how much work actually happened.
    RunSummary cookOnce() {
        RunSummary total;
        for (int pass = 0; pass < 240; ++pass) {   // ~cap; static trees exit in 2
            runOneCookPass();
            const Stats s = m_stats.load();
            total.cooked += s.cooked;
            total.failed  = s.failed;   // standing failures, not per-pass news
            if (s.deferred == 0 && s.cooked == 0) break;   // converged
            if (s.deferred > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
        return total;
    }

    // Re-scan assets/ and cook anything new or stale.
    // Safe to call from any thread (main thread, button handler, etc.)
    void requestRefresh();

    // ── .cache garbage collection ────────────────────────────────────────
    // Cooked output accumulates: a cooker version bump re-cooks under a new
    // key, a deleted source leaves its output behind, and a regenerated
    // registry.db mints fresh UUIDs so every previous file is instantly
    // unreferenced. Measured on fps_shooter: one version bump grew .cache from
    // 72 MB to 113 MB, all of the growth orphaned.
    //
    // This is the EASY layer to collect, and the reason the distributed-cache
    // question is not scary: `.cache` is pure derived data. Deleting a live
    // file costs a re-cook (or a ~100 ms DDC restore), never data — so the
    // worst GC bug is slow, not destructive. The DDC and any shared tier are
    // collected separately by their own size/age policy; nothing here needs to
    // coordinate with another machine.
    //
    // DRY RUN BY DEFAULT. `prune == false` reports and deletes nothing.
    struct GcStats {
        int      keptFiles = 0,  orphanFiles = 0;
        uint64_t keptBytes = 0,  orphanBytes = 0;
        int      deleted   = 0;
        uint64_t freedBytes = 0;
    };
    GcStats collectGarbage(bool prune);

    Stats       stats()     const;
    bool        isCooking() const { return m_stats.load().active; }

    // What a cook pass considers cookable.
    //   WholeProject — every registered asset (editor default: any scene you
    //                  open next is ready).
    //   SceneClosure — only assets the project's .scene files reference, i.e.
    //                  the scene meshes + their textures. On-demand cooking:
    //                  touch 3 assets, not a 637-asset kit dropped in assets/.
    enum class Scope { WholeProject, SceneClosure };
    void setScope(Scope s) { m_scope = s; }

private:
    void cookLoop();
    void runOneCookPass();
    // Scene cooks as graph tasks. Scene staleness is PER-SCENE: source-JSON
    // mtime vs cooked binary, header version peek, plus
    // sceneDependsOnNewerAssets() — AND any scene whose referenced assets
    // are in `cooking` gets a task with dependency edges on exactly those
    // assets, so it cooks the moment they land (the old flow cooked every
    // scene sequentially after ALL assets finished). scenesCooked/scenesFailed
    // are bumped from the graph's drain lane (the cook thread).
    // `scenesDeferred` counts scenes skipped by the write-settle check. It MUST
    // reach the caller: cookOnce() converges when nothing cooked and nothing
    // deferred, and fileSettled()'s stability map is per-process, so a fresh
    // engine_cook always defers on its only pass. Not reporting it made the
    // CLI exit before scenes ever cooked — silently, since "0 cooked" looks
    // like "up to date".
    std::vector<assetlib::CookPipeline::ExtraTask> buildSceneTasks(
        assetlib::AssetRegistry& registry,
        const std::unordered_set<std::string>& cooking,
        int* scenesCooked, int* scenesFailed, int* scenesDeferred);
    // Files mid-write by external tools defer to a follow-up pass.
    void requeueIfDeferred(int deferred);
    // Scene source dirs: <project>/scenes and <assets>/scenes (whichever exist).
    std::vector<std::filesystem::path> sceneDirs() const;

    Scope m_scope = Scope::WholeProject;

    std::filesystem::path m_dbPath;
    std::filesystem::path m_projectRoot;
    std::filesystem::path m_assetsRoot;
    std::filesystem::path m_cacheRoot;

    std::thread             m_thread;
    std::atomic<bool>       m_running{true};

    // Lightweight atomic snapshot for UI polling (no lock needed for read)
    struct AtomicStats {
        std::atomic<int>  total{0};
        std::atomic<int>  cooked{0};
        std::atomic<int>  failed{0};
        std::atomic<int>  deferred{0};
        std::atomic<bool> active{false};
        // currentAsset needs a mutex — strings aren't atomically copyable
        mutable std::mutex        nameMtx;
        std::string               currentAsset;

        Stats load() const {
            Stats s;
            s.total    = total.load();
            s.cooked   = cooked.load();
            s.failed   = failed.load();
            s.deferred = deferred.load();
            s.active   = active.load();
            std::lock_guard<std::mutex> lk(nameMtx);
            s.currentAsset = currentAsset;
            return s;
        }
    } m_stats;

    std::mutex              m_requestMtx;
    std::condition_variable m_requestCV;
    int                     m_pendingRequests{1}; // start with 1 so first pass runs
};
