// ── AsyncLoader — QUEUE + LIFECYCLE (main + worker threads) ──────────────────
// One of AsyncLoader's three TUs (parse.cpp CPU import, upload.cpp GPU
// upload). Owns the request queue, in-flight/waiter/cache maps (normalized
// keys — audit C.4), the self-chaining worker dispatch (shutdown-safe —
// audit: m_jobBusy is the destructor's contract), and the public poll API.
#include "runtime/services/async_loader.h"
#include "runtime/services/async_loader/loader_internal.h"
#include "core/logger.h"
#include "core/memory/mem.h"
#include "runtime/jobs/jobs.h"

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
        LOG_INFO("Loader", "Worker started: %s", req.name.c_str());
        LoadedAsset asset = processFile(req.path, req.name);
        if (asset.success)
            LOG_SUCCESS("Loader", "Worker done: %s — ready to upload",
                        req.name.c_str());
        else
            LOG_ERROR("Loader", "Worker failed: %s", asset.error.c_str());
        {
            std::lock_guard<std::mutex> lk(m_readyMtx);
            m_ready.push({std::move(asset), std::move(req.cb)});
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
