#include "assetlib/task_graph.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>

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
    nice(10);   // best-effort; ignored if unsupported
#endif
}

// Admits work against a byte budget instead of a fixed thread count, so a
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

int TaskGraph::add(std::string name, size_t estBytes, WorkFn work, DoneFn done) {
    Node n;
    n.name     = std::move(name);
    n.estBytes = estBytes;
    n.work     = std::move(work);
    n.done     = std::move(done);
    m_nodes.push_back(std::move(n));
    return (int)m_nodes.size() - 1;
}

void TaskGraph::addEdge(int before, int after) {
    if (before < 0 || after < 0 || before == after ||
        before >= (int)m_nodes.size() || after >= (int)m_nodes.size()) return;
    m_nodes[before].dependents.push_back(after);
    m_nodes[after].unmet++;
    ++m_edges;
}

size_t TaskGraph::run(const Options& opts) {
    const size_t total = m_nodes.size();
    if (total == 0) return 0;

    // ── Governance (same three levers as the old flat pool) ───────────────
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    int workers = opts.workers > 0 ? opts.workers
        : (int)envLong("COOK_THREADS", (long)std::max(1u, hw > 2 ? hw - 2 : 1u));
    workers = std::max(1, std::min(workers, (int)total));

    size_t budget = opts.memBudget;
    if (budget == 0) {
        const size_t ram = physicalRamBytes();
        const size_t autoBudget = ram ? ram * 3 / 5 : ((size_t)4 << 30); // 60% RAM
        budget = (size_t)envLong("COOK_MEM_BUDGET_MB",
                                 (long)(autoBudget >> 20)) << 20;
    }
    MemGovernor gov(budget);

    std::printf("[TaskGraph] %zu task(s), %zu edge(s), %d worker(s), "
                "mem budget %zu MB\n", total, m_edges, workers, budget >> 20);

    // ── Shared state (all under one mutex; work runs outside it) ──────────
    std::mutex              mtx;
    std::condition_variable cvWork;   // workers: ready-heap non-empty / stop
    std::condition_variable cvDrain;  // drain: completions / state change
    // Max-heap on estBytes → longest-processing-time-first dispatch.
    std::priority_queue<std::pair<size_t,int>> ready;
    std::vector<int> finishedQ;
    int    inFlight = 0;
    bool   stop     = false;

    for (int i = 0; i < (int)total; ++i)
        if (m_nodes[i].unmet == 0)
            ready.push({m_nodes[i].estBytes, i});

    auto workerLoop = [&] {
        demoteToBackground();
        for (;;) {
            int idx = -1;
            {
                std::unique_lock<std::mutex> lk(mtx);
                cvWork.wait(lk, [&] { return stop || !ready.empty(); });
                if (stop) return;
                idx = ready.top().second;
                ready.pop();
                ++inFlight;
            }
            Node& n = m_nodes[idx];
            gov.acquire(n.estBytes);
            // Work bodies carry their own error handling (dispatchCook nets
            // exceptions; scene tasks return bool) — this last-resort catch
            // only keeps the worker alive if one slips through.
            try { if (n.work) n.work(); } catch (...) {}
            gov.release(n.estBytes);
            {
                std::lock_guard<std::mutex> lk(mtx);
                --inFlight;
                finishedQ.push_back(idx);
            }
            cvDrain.notify_one();
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (int t = 0; t < workers; ++t) pool.emplace_back(workerLoop);

    // ── Drain (caller thread): done() callbacks + dependency release ──────
    size_t drained   = 0;
    bool   cancelled = false;
    for (;;) {
        int idx = -1;
        {
            std::unique_lock<std::mutex> lk(mtx);
            // Timed wait so cancellation is noticed even while every worker
            // is deep inside a long cook.
            cvDrain.wait_for(lk, std::chrono::milliseconds(100),
                             [&] { return !finishedQ.empty(); });

            if (!cancelled && opts.shouldContinue && !opts.shouldContinue()) {
                // Stop dispatching; in-flight tasks still land and drain.
                cancelled = true;
                stop      = true;
                ready     = {};
                cvWork.notify_all();
            }
            if (!finishedQ.empty()) {
                idx = finishedQ.back();
                finishedQ.pop_back();
            } else if (cancelled) {
                if (inFlight == 0) break;          // everything landed
                continue;
            } else if (ready.empty() && inFlight == 0) {
                if (drained == total) break;       // all done
                // Nothing running, nothing ready, tasks remain → cycle.
                std::fprintf(stderr, "[TaskGraph] dependency cycle — %zu "
                             "task(s) unreachable:\n", total - drained);
                for (const auto& n : m_nodes)
                    if (n.unmet > 0)
                        std::fprintf(stderr, "[TaskGraph]   %s\n", n.name.c_str());
                break;
            } else {
                continue;                          // spurious/timeout wakeup
            }
        }

        Node& n = m_nodes[idx];
        try { if (n.done) n.done(); } catch (...) {
            std::fprintf(stderr, "[TaskGraph] done() threw for %s\n",
                         n.name.c_str());
        }
        ++drained;

        {
            std::lock_guard<std::mutex> lk(mtx);
            for (int dep : n.dependents)
                if (--m_nodes[dep].unmet == 0 && !stop)
                    ready.push({m_nodes[dep].estBytes, dep});
        }
        cvWork.notify_all();
    }

    {
        std::lock_guard<std::mutex> lk(mtx);
        stop = true;
    }
    cvWork.notify_all();
    for (auto& th : pool) th.join();
    return drained;
}

} // namespace assetlib
