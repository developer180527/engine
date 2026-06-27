#pragma once
// ── Profiler framework ──────────────────────────────────────────────────────
// An EXTENSIBLE profiling system, not just a timer. The Profiler hub owns the
// frame lifecycle and a registry of channels; each *kind* of profiler (timing,
// memory, GPU, allocations, ...) is an IProfilerChannel. The timer is the
// first channel; add others by implementing the interface and registering:
//
//     class MemoryChannel : public prof::IProfilerChannel { ... };
//     prof::Profiler::get().addChannel(&myMemoryChannel);
//
// Design rules that keep it cheap and safe to grow:
//   • The hot path (ENGINE_PROFILE_SCOPE) NEVER touches the hub or a channel
//     vtable — it checks one relaxed atomic and writes to a thread-local
//     buffer. Adding channels cannot tax the per-scope cost.
//   • Lives in core/ — pure std, zero bgfx/flecs/GLFW. So it times the boot
//     sequence before the renderer exists and works in engine_core CLI tools.
//   • Channels own their own data shape and presentation; the hub only
//     orchestrates frame begin/end and enumerates channels for consumers.
//
// This is an INSTRUMENTING profiler (named scopes you place), complementary to
// a sampling profiler (Instruments/perf/VTune). Scope at PHASE granularity —
// never inside a hot per-iteration loop, or the instrumentation perturbs what
// it measures.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

// Compile switch: macros vanish entirely in shipping release builds.
#ifndef ENGINE_PROFILE
  #ifdef NDEBUG
    #define ENGINE_PROFILE 0
  #else
    #define ENGINE_PROFILE 1
  #endif
#endif

// Cache-line size — recorders are aligned to it to avoid false sharing.
#ifndef ENGINE_CACHE_LINE
  #define ENGINE_CACHE_LINE 64
#endif

namespace prof {

// ── Monotonic clock (the single time source) ────────────────────────────────
// The one place time comes from, isolated so it can be swapped (rdtsc, a fake
// clock for deterministic replay) without touching any caller. std::steady_
// clock is portable and already backed by the best monotonic source on each
// platform (mach_absolute_time on Apple, QueryPerformanceCounter on Windows,
// clock_gettime(MONOTONIC) on Linux).
struct Clock {
    static uint64_t nowNs() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};
inline uint64_t nowNs() { return Clock::nowNs(); }

// Master enable — the ONLY thing the scope hot path checks (one relaxed load).
inline std::atomic<bool> g_enabled{false};

inline void warnOverflowOnce() {
    static std::atomic<bool> warned{false};
    bool expected = false;
    if (warned.compare_exchange_strong(expected, true))
        std::fprintf(stderr, "[Profiler] timer sample buffer overflow — "
            "dropping samples this frame (raise "
            "TimerChannel::kMaxSamplesPerThread or use fewer scopes)\n");
}

// ── Channel interface — the extension point ─────────────────────────────────
class IProfilerChannel {
public:
    virtual ~IProfilerChannel() = default;
    virtual const char* channelName() const = 0;
    virtual void beginFrame() = 0;
    virtual void endFrame()   = 0;
};

// ── Sample ──────────────────────────────────────────────────────────────────
// name = address of the string literal (stable id; no hashing/interning/alloc).
// parent = index of the enclosing scope WITHIN the merged frame snapshot
// (rebased at collect time), or kNoParent for a root — enables exact tree /
// flamegraph reconstruction. depth is kept for quick indentation.
static constexpr uint32_t kNoParent = 0xFFFFFFFFu;
struct TimerSample {
    const char* name;
    uint64_t    start;
    uint64_t    end;
    uint32_t    parent;
    uint16_t    depth;
    uint16_t    threadIndex;
};

// ── Timer channel ───────────────────────────────────────────────────────────
class TimerChannel final : public IProfilerChannel {
public:
    // Fixed per-thread budget — bounds memory and means no realloc after warmup.
    static constexpr uint32_t kMaxSamplesPerThread = 16384;
    static constexpr uint32_t kMaxDepth            = 256;

    const char* channelName() const override { return "Timer"; }

    // Per-thread recorder, cache-line aligned so its hot fields never share a
    // line with a neighbour's. Fixed-capacity buffers (reserved once); overflow
    // drops+warns rather than growing.
    struct alignas(ENGINE_CACHE_LINE) Recorder {
        std::vector<TimerSample> samples;   // capacity kMaxSamplesPerThread
        std::vector<uint32_t>    openStack; // indices of currently-open scopes
        uint64_t                 dropped     = 0;
        uint16_t                 threadIndex = 0;
        Recorder() {
            samples.reserve(kMaxSamplesPerThread);
            openStack.reserve(kMaxDepth);
        }
    };

    Recorder* createRecorder() {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto r = std::make_unique<Recorder>();
        r->threadIndex = (uint16_t)m_recorders.size();
        Recorder* raw = r.get();
        m_recorders.push_back(std::move(r));
        return raw;
    }

    void beginFrame() override {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto& r : m_recorders) { r->samples.clear(); r->openStack.clear(); }
        m_frameStart = nowNs();
    }

    // Merge every thread's samples into the readable snapshot. The frame
    // boundary is a sync point (worker jobs joined), so this is race-free.
    // Parent indices are per-thread-local; rebase them into the merged buffer.
    void endFrame() override {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_last.clear();
        m_lastDropped = 0;
        for (auto& r : m_recorders) {
            const uint32_t base = (uint32_t)m_last.size();
            for (const TimerSample& s : r->samples) {
                TimerSample t = s;
                if (t.parent != kNoParent) t.parent += base;
                m_last.push_back(t);
            }
            m_lastDropped += r->dropped;
            r->dropped = 0;
        }
        m_lastStart = m_frameStart;
        m_lastEnd   = nowNs();
    }

    // ── Consumer read API ───────────────────────────────────────────────────
    const std::vector<TimerSample>& lastFrame() const { return m_last; }
    uint64_t lastFrameStart()   const { return m_lastStart; }
    uint64_t lastFrameEnd()     const { return m_lastEnd; }
    double   lastFrameMs()      const { return (m_lastEnd - m_lastStart) / 1e6; }
    uint64_t lastFrameDropped() const { return m_lastDropped; }

    void logLastFrame(const char* tag) const {
        std::printf("[Profiler] %s — %.3f ms (%zu samples%s)\n",
                    tag, lastFrameMs(), m_last.size(),
                    m_lastDropped ? ", OVERFLOWED" : "");
        for (const auto& x : m_last) {           // pre-order: parent before child
            if (x.threadIndex != 0) continue;
            std::printf("           ");
            for (int i = 0; i < x.depth; ++i) std::printf("  ");
            std::printf("%-22s %.3f ms\n", x.name, (x.end - x.start) / 1e6);
        }
        if (m_lastDropped)
            std::printf("           (%llu dropped — raise kMaxSamplesPerThread)\n",
                        (unsigned long long)m_lastDropped);
        std::fflush(stdout);
    }

private:
    std::mutex                             m_mtx;
    std::vector<std::unique_ptr<Recorder>> m_recorders;
    std::vector<TimerSample>               m_last;
    uint64_t m_frameStart = 0, m_lastStart = 0, m_lastEnd = 0, m_lastDropped = 0;
};

// ── Hub ─────────────────────────────────────────────────────────────────────
// Intentionally leaked singleton (never destroyed): thread-local recorders may
// outlive normal static teardown, so a never-destroyed hub keeps their
// registration valid at any point in the program's life.
class Profiler {
public:
    static Profiler& get() {
        static Profiler* p = new Profiler();
        return *p;
    }

    void setEnabled(bool e) { g_enabled.store(e, std::memory_order_relaxed); }
    bool enabled() const    { return g_enabled.load(std::memory_order_relaxed); }

    TimerChannel& timer() { return m_timer; }

    void addChannel(IProfilerChannel* ch)    { m_extra.push_back(ch); }
    void removeChannel(IProfilerChannel* ch) {
        for (auto it = m_extra.begin(); it != m_extra.end(); ++it)
            if (*it == ch) { m_extra.erase(it); return; }
    }
    std::vector<IProfilerChannel*> channels() {
        std::vector<IProfilerChannel*> all{ &m_timer };
        all.insert(all.end(), m_extra.begin(), m_extra.end());
        return all;
    }

    void beginFrame() {
        if (!enabled()) return;
        ++m_frame;
        m_timer.beginFrame();
        for (auto* c : m_extra) c->beginFrame();
    }
    void endFrame() {
        if (!enabled()) return;
        m_timer.endFrame();
        for (auto* c : m_extra) c->endFrame();
    }
    uint64_t frame() const { return m_frame; }

private:
    Profiler() = default;
    TimerChannel                   m_timer;
    std::vector<IProfilerChannel*> m_extra;
    uint64_t                       m_frame = 0;
};

// Thread-local recorder pointer — cached so the hot path is a thread-local
// load + a buffer write, never a hub lookup.
inline thread_local TimerChannel::Recorder* t_recorder = nullptr;
inline TimerChannel::Recorder& timerRecorder() {
    if (!t_recorder) t_recorder = Profiler::get().timer().createRecorder();
    return *t_recorder;
}

// ── ScopedTimer — what the macro instantiates ───────────────────────────────
// Pre-order emission: the sample's slot is reserved on enter (so its parent is
// the enclosing open scope's slot), end is filled on exit. Overflow: a dropped
// scope is simply omitted — its children re-attach to the nearest emitted
// ancestor, and the open stack stays balanced.
class ScopedTimer {
public:
    explicit ScopedTimer(const char* name) {
        if (!g_enabled.load(std::memory_order_relaxed)) return;
        TimerChannel::Recorder& r = timerRecorder();
        m_rec = &r;
        if (r.samples.size() >= TimerChannel::kMaxSamplesPerThread ||
            r.openStack.size() >= TimerChannel::kMaxDepth) {
            ++r.dropped;
            warnOverflowOnce();
            return; // not emitted; m_emitted stays false
        }
        const uint32_t parent = r.openStack.empty() ? kNoParent : r.openStack.back();
        const uint16_t depth  = (uint16_t)r.openStack.size();
        m_idx = (uint32_t)r.samples.size();
        r.samples.push_back({ name, nowNs(), 0, parent, depth, r.threadIndex });
        r.openStack.push_back(m_idx);
        m_emitted = true;
    }
    ~ScopedTimer() {
        if (!m_rec || !m_emitted) return;
        m_rec->samples[m_idx].end = nowNs();
        m_rec->openStack.pop_back();
    }
    ScopedTimer(const ScopedTimer&)            = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    TimerChannel::Recorder* m_rec     = nullptr;
    uint32_t                m_idx     = 0;
    bool                    m_emitted = false;
};

} // namespace prof

// ── Macros ──────────────────────────────────────────────────────────────────
#if ENGINE_PROFILE
  #define ENGINE_PROF_CONCAT2(a, b) a##b
  #define ENGINE_PROF_CONCAT(a, b)  ENGINE_PROF_CONCAT2(a, b)
  #define ENGINE_PROFILE_SCOPE(name) \
      ::prof::ScopedTimer ENGINE_PROF_CONCAT(_engine_prof_, __LINE__){ name }
  #define ENGINE_PROFILE_FUNC() ENGINE_PROFILE_SCOPE(__func__)
#else
  #define ENGINE_PROFILE_SCOPE(name) ((void)0)
  #define ENGINE_PROFILE_FUNC()      ((void)0)
#endif
