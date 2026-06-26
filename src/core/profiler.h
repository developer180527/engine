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
//     orchestrates frame begin/end and enumerates channels for consumers
//     (the editor overlay downcasts to the channel types it knows how to draw,
//     exactly like the plugins panel downcasts to IEditorPlugin).
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
#include <string>
#include <vector>
#include <algorithm>

// Compile switch: macros vanish entirely in shipping release builds.
#ifndef ENGINE_PROFILE
  #ifdef NDEBUG
    #define ENGINE_PROFILE 0
  #else
    #define ENGINE_PROFILE 1
  #endif
#endif

namespace prof {

using Clock = std::chrono::steady_clock;
inline uint64_t nowNs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

// Master enable — the ONLY thing the scope hot path checks (one relaxed load).
inline std::atomic<bool> g_enabled{false};

// ── Channel interface — the extension point ─────────────────────────────────
// A profiler kind implements this. The hub drives begin/endFrame (once per
// frame, cold path — virtual calls here are fine). Data model + presentation
// are the channel's own business.
class IProfilerChannel {
public:
    virtual ~IProfilerChannel() = default;
    virtual const char* channelName() const = 0;
    virtual void beginFrame() = 0;
    virtual void endFrame()   = 0;
};

// ── Timer channel ───────────────────────────────────────────────────────────
// One sample per completed scope: {name, start, end, depth}. The name is the
// ADDRESS of the string literal passed to the macro — string literals have
// static storage, so the const char* IS a stable id: no hashing, no interning,
// no per-sample allocation.
struct TimerSample {
    const char* name;
    uint64_t    start;
    uint64_t    end;
    uint16_t    depth;
    uint32_t    threadIndex;
};

class TimerChannel final : public IProfilerChannel {
public:
    const char* channelName() const override { return "Timer"; }

    // Per-thread recorder. The hot path writes only to its own thread's
    // recorder — no locks, no contention. Frame boundary (begin/endFrame) is
    // the sync point where the main thread reads them all.
    struct Recorder {
        std::vector<TimerSample> samples;
        uint16_t                 depth = 0;
        uint32_t                 threadIndex = 0;
        Recorder() { samples.reserve(4096); } // warm — clear() keeps capacity
    };

    Recorder* createRecorder() {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto r = std::make_unique<Recorder>();
        r->threadIndex = (uint32_t)m_recorders.size();
        Recorder* raw = r.get();
        m_recorders.push_back(std::move(r));
        return raw;
    }

    void beginFrame() override {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto& r : m_recorders) r->samples.clear();
        m_frameStart = nowNs();
    }

    // Collect every thread's samples into the readable snapshot. The frame
    // boundary is a sync point: all worker jobs are joined before this runs,
    // so reading the per-thread buffers is race-free (the threading contract).
    void endFrame() override {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_last.clear();
        for (auto& r : m_recorders)
            m_last.insert(m_last.end(), r->samples.begin(), r->samples.end());
        m_lastStart = m_frameStart;
        m_lastEnd   = nowNs();
    }

    // ── Consumer read API (overlay / export / log) ──────────────────────────
    const std::vector<TimerSample>& lastFrame() const { return m_last; }
    uint64_t lastFrameStart() const { return m_lastStart; }
    uint64_t lastFrameEnd()   const { return m_lastEnd; }
    double   lastFrameMs()    const { return (m_lastEnd - m_lastStart) / 1e6; }

    // Print the last frame's call tree (thread 0) to stdout. Boot dump + the
    // headless verification both use this; the editor overlay reads lastFrame()
    // directly and draws its own UI.
    void logLastFrame(const char* tag) const {
        std::vector<TimerSample> s;
        for (const auto& x : m_last) if (x.threadIndex == 0) s.push_back(x);
        std::sort(s.begin(), s.end(),
                  [](const TimerSample& a, const TimerSample& b) {
                      return a.start < b.start; // post-order -> pre-order
                  });
        std::printf("[Profiler] %s — %.3f ms (%zu samples)\n",
                    tag, lastFrameMs(), m_last.size());
        for (const auto& x : s) {
            std::printf("           ");
            for (int i = 0; i < x.depth; ++i) std::printf("  ");
            std::printf("%-22s %.3f ms\n", x.name, (x.end - x.start) / 1e6);
        }
        std::fflush(stdout); // debug dump — flush so it survives a kill/pipe
    }

private:
    std::mutex                             m_mtx;
    std::vector<std::unique_ptr<Recorder>> m_recorders;
    std::vector<TimerSample>               m_last;
    uint64_t m_frameStart = 0, m_lastStart = 0, m_lastEnd = 0;
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

    // Register an additional channel (memory, GPU, ...). Lifetime is the
    // caller's; remove before destroying it.
    void addChannel(IProfilerChannel* ch) {
        m_extra.push_back(ch);
    }
    void removeChannel(IProfilerChannel* ch) {
        m_extra.erase(std::remove(m_extra.begin(), m_extra.end(), ch),
                      m_extra.end());
    }
    // All channels (built-in timer first) — for the overlay/export to walk.
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
    TimerChannel                   m_timer;   // built-in channel
    std::vector<IProfilerChannel*> m_extra;   // memory/GPU/... added later
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
class ScopedTimer {
public:
    explicit ScopedTimer(const char* name) {
        if (!g_enabled.load(std::memory_order_relaxed)) return; // ~1 ns, dormant
        m_rec   = &timerRecorder();
        m_name  = name;
        m_start = nowNs();
        m_depth = m_rec->depth++;
    }
    ~ScopedTimer() {
        if (!m_rec) return;
        m_rec->depth--;
        m_rec->samples.push_back(
            { m_name, m_start, nowNs(), m_depth, m_rec->threadIndex });
    }
    ScopedTimer(const ScopedTimer&)            = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    TimerChannel::Recorder* m_rec   = nullptr; // null = dormant, no record
    const char*             m_name  = nullptr;
    uint64_t                m_start = 0;
    uint16_t                m_depth = 0;
};

} // namespace prof

// ── Macros ──────────────────────────────────────────────────────────────────
#if ENGINE_PROFILE
  #define ENGINE_PROF_CONCAT2(a, b) a##b
  #define ENGINE_PROF_CONCAT(a, b)  ENGINE_PROF_CONCAT2(a, b)
  // Time the enclosing block under `name` (a string literal).
  #define ENGINE_PROFILE_SCOPE(name) \
      ::prof::ScopedTimer ENGINE_PROF_CONCAT(_engine_prof_, __LINE__){ name }
  // Time the enclosing function (name = __func__).
  #define ENGINE_PROFILE_FUNC() ENGINE_PROFILE_SCOPE(__func__)
#else
  #define ENGINE_PROFILE_SCOPE(name) ((void)0)
  #define ENGINE_PROFILE_FUNC()      ((void)0)
#endif
