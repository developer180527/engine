// ── AsyncLoader — QUEUE + LIFECYCLE (main + worker threads) ──────────────────
// One of AsyncLoader's three TUs (parse.cpp cooked read, upload.cpp GPU
// upload). Owns the request queue, in-flight/waiter/cache maps (normalized
// keys — audit C.4), the self-chaining worker dispatch (shutdown-safe —
// audit: m_jobBusy is the destructor's contract), and the public poll API.
#include "runtime/services/async_loader.h"
#include "runtime/services/async_loader/loader_internal.h"
#include "core/logger.h"
#include "core/memory/mem.h"
#include "core/jobs/jobs.h"

#include <thread>

using asyncldr::normalizeKey;

// -----------------------------------------------------------------------
// AsyncLoader — ONE parse at a time (mirrors the old dedicated
// single worker — registry access stays single-consumer) but scheduled on
// the shared pool. Hosts without jobs::init (bare tools) degrade to
// inline synchronous loads via the jobs facade fallback.
// -----------------------------------------------------------------------
AsyncLoader::AsyncLoader() = default;

AsyncLoader::~AsyncLoader() {
    // Wait out an in-flight parse job (it holds `this`). The pool survives
    // us in normal host order; if jobs already shut down, chained runs were
    // inline and m_jobBusy is already false.
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(m_pendingMtx);
            if (!m_jobBusy) break;
        }
        std::this_thread::yield();
    }
    // Drain pending uploads — staging blobs are released by the backend at shutdown
    std::lock_guard<std::mutex> lk(m_readyMtx);
    while (!m_ready.empty()) m_ready.pop();
}

void AsyncLoader::load(const std::string& path, const std::string& name, OnLoaded cb) {
    const std::string key = normalizeKey(path);
    // Fast path: already fully loaded — return cached result immediately
    {
        std::lock_guard<std::mutex> lk(m_loadedMtx);
        auto it = m_loadedResults.find(key);
        if (it != m_loadedResults.end()) {
            if (cb) cb(it->second, name);
            return;
        }
    }
    // In-flight: queue callback for when current load completes
    {
        std::lock_guard<std::mutex> lk(m_pendingMtx);
        if (m_inFlight.count(key)) {
            m_waiters[key].push_back(std::move(cb));
            return;
        }
        m_inFlight.insert(key);
        m_pending.push({path, name, std::move(cb)});   // raw path: fs access
    }
    m_failed.erase(key);   // a new request is a retry
    armWorker();
}

AssetLoadState AsyncLoader::state(const std::string& path) const {
    const std::string key = normalizeKey(path);
    {
        std::lock_guard<std::mutex> lk(m_loadedMtx);
        if (m_loadedResults.count(key)) return AssetLoadState::Ready;
    }
    {
        std::lock_guard<std::mutex> lk(m_pendingMtx);
        if (m_inFlight.count(key)) return AssetLoadState::Pending;
    }
    return m_failed.count(key) ? AssetLoadState::Failed : AssetLoadState::None;
}

// ── The job: a NotCooked result becomes a cook request, or a failure ─────────
void AsyncLoader::park(UploadRequest& req) {
    if (!m_cook || !m_cook->canCook()) {
        // The null provider: no cooker in this build. Final, and said once.
        req.asset.error = "not cooked: " + req.asset.path
                        + " (this build has no cooker; run engine_cook)";
        finishFailed(req);
        return;
    }
    for (const std::string& src : req.asset.needsCook)
        if (m_requested.insert(normalizeKey(src)).second) {
            LOG_INFO("Loader", "not cooked, cook requested: %s (for %s)",
                     src.c_str(), req.asset.name.c_str());
            m_cook->requestCook(src);
        }
    m_parked.push_back({LoadRequest{req.asset.path, req.asset.name, std::move(req.cb),
                                    req.waitForTextures}});
}

void AsyncLoader::retryParked() {
    m_drainsSinceRetry = 0;
    if (m_parked.empty()) return;
    // A cook that never lands (a source no cooker handles, a cooker that is
    // never scheduled) must not leave a placeholder Pending forever: after
    // kMaxRetries (~5 minutes of retries) it is Failed and says why.
    constexpr int kMaxRetries = 600;
    std::vector<Parked> parked;
    parked.swap(m_parked);
    for (Parked& p : parked) {
        if (++p.retries > kMaxRetries) {
            UploadRequest r;
            r.asset.path  = p.req.path;
            r.asset.name  = p.req.name;
            r.asset.error = "cook requested but never finished: " + p.req.path;
            r.cb = std::move(p.req.cb);
            finishFailed(r);
            continue;
        }
        std::lock_guard<std::mutex> lk(m_pendingMtx);
        m_pending.push(std::move(p.req));   // key stays in m_inFlight
    }
    armWorker();
}

void AsyncLoader::unload(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_loadedMtx);
    m_loadedResults.erase(normalizeKey(path));
}
bool AsyncLoader::isLoading(const std::string& path) const {
    std::lock_guard<std::mutex> lk(m_pendingMtx);
    return m_inFlight.count(normalizeKey(path)) > 0;
}

bool AsyncLoader::isLoaded(const std::string& path) const {
    std::lock_guard<std::mutex> lk(m_loadedMtx);
    return m_loadedResults.count(normalizeKey(path)) > 0;
}

int AsyncLoader::pendingCount() const {
    std::lock_guard<std::mutex> lk(m_pendingMtx);
    return (int)(m_pending.size() + m_inFlight.size());
}

void AsyncLoader::armWorker() {
    LoadRequest req;
    {
        std::lock_guard<std::mutex> lk(m_pendingMtx);
        if (m_jobBusy || m_pending.empty()) return;
        m_jobBusy = true;
        req = std::move(m_pending.front());
        m_pending.pop();
    }
    dispatch(std::move(req));
}

void AsyncLoader::dispatch(LoadRequest req) {
    jobs::run("io.assetLoad", [this, req = std::move(req)]() mutable {
        // Asset work allocates under the Assets tag (Assimp scenes, vertex
        // staging, ozz scratch) regardless of which pool thread runs it.
        MEM_SCOPE(mem::Tag::Assets);
        LoadedAsset asset = processFile(req.path, req.name, req.waitForTextures);
        {
            std::lock_guard<std::mutex> lk(m_readyMtx);
            m_ready.push({std::move(asset), std::move(req.cb), req.waitForTextures});
        }
        // Intentionally do NOT touch m_loadedHandles or m_inFlight here.
        // drainOne() (main thread) sets the real handle then erases inFlight
        // atomically, closing the window where load() could find an invalid
        // placeholder handle and call a callback prematurely.
        //
        // Chain the next request WITHOUT dropping m_jobBusy first. The old code
        // set m_jobBusy=false then called armWorker() (re-locking m_pendingMtx +
        // maybe spawning a job) — but ~AsyncLoader frees us the instant it sees
        // !m_jobBusy, so armWorker() could run on a destroyed `this` (shutdown
        // UAF). Decide under ONE lock: keep busy=true while chaining; clearing
        // it when idle is our LAST access to `this`.
        LoadRequest next;
        bool chain = false;
        {
            std::lock_guard<std::mutex> lk(m_pendingMtx);
            if (m_pending.empty()) {
                m_jobBusy = false;                    // idle → dtor may free us now
            } else {
                next  = std::move(m_pending.front()); // stay busy: next job holds `this`
                m_pending.pop();
                chain = true;
            }
        }
        if (chain) dispatch(std::move(next));         // no `this` access after !busy
    });
}

// -----------------------------------------------------------------------
// drainOne — main thread only.
// ALL data is pre-copied. This function only creates GPU handles and
// spawns the entity. Typical cost: <1ms even for complex assets.
// -----------------------------------------------------------------------
