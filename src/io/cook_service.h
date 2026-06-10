#pragma once
#include <assetlib/asset_registry.h>
#include <assetlib/cook_pipeline.h>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>

// CookService — runs the cook pipeline on a background thread so the
// editor opens immediately. Opens its own DB connection (WAL mode allows
// concurrent reads from the main thread's registry + writes here).
class CookService {
public:
    struct Stats {
        int  total   = 0;
        int  cooked  = 0;
        int  failed  = 0;
        bool active  = false;
        std::string currentAsset;
    };

    CookService(const std::filesystem::path& dbPath,
                const std::filesystem::path& projectRoot,
                const std::filesystem::path& assetsRoot,
                const std::filesystem::path& cacheRoot);
    ~CookService();

    // Launch background cook thread — call once after editor opens
    void start();

    // Run a single synchronous cook pass on the calling thread — no
    // background thread. For CLI tooling (engine_cook). Returns the number
    // of assets that failed to cook (0 = success).
    int cookOnce() {
        runOneCookPass();
        return m_stats.load().failed;
    }

    // Re-scan assets/ and cook anything new or stale.
    // Safe to call from any thread (main thread, button handler, etc.)
    void requestRefresh();

    Stats       stats()     const;
    bool        isCooking() const { return m_stats.load().active; }

private:
    void cookLoop();
    void runOneCookPass();
    void cookSceneFiles(assetlib::AssetRegistry& registry, bool assetsChanged);

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
        std::atomic<bool> active{false};
        // currentAsset needs a mutex — strings aren't atomically copyable
        mutable std::mutex        nameMtx;
        std::string               currentAsset;

        Stats load() const {
            Stats s;
            s.total  = total.load();
            s.cooked = cooked.load();
            s.failed = failed.load();
            s.active = active.load();
            std::lock_guard<std::mutex> lk(nameMtx);
            s.currentAsset = currentAsset;
            return s;
        }
    } m_stats;

    std::mutex              m_requestMtx;
    std::condition_variable m_requestCV;
    int                     m_pendingRequests{1}; // start with 1 so first pass runs
};
