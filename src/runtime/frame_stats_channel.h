#pragma once
// ── FrameStatsChannel — frame-time DISTRIBUTION (profiler channel) ───────────
// The existing timer channel answers "where did THIS frame go?". That is the
// wrong shape for a stutter question: stutter is a property of the tail, not of
// any one frame, and a single frame's breakdown cannot distinguish "we are
// slow" from "we hitch every two seconds". This channel keeps a ring of every
// frame's timings and reports the distribution.
//
// Three numbers per frame, and the SPLIT between them is the whole point:
//   cadence   beginFrame(N-1) -> beginFrame(N).  Wall-clock frame interval —
//             what the player actually feels. Includes everything: our work,
//             the present/vsync wait, and the pre-frame event poll.
//   present   duration of the "bgfx.frame" scope — submit + swap + the block
//             on the next drawable. Time we are WAITING for the display.
//   work      cadence's CPU span minus present — our actual per-frame cost.
//
// If `present` dominates and `work` is flat, the app is display-bound and has
// headroom; optimising CPU work buys nothing. If `work`'s p99 is far above its
// p50, there is a real hitch and the timer channel will say where. Reading
// either conclusion off a single frame is how you end up optimising the wrong
// thing.
//
// Cost: two clock reads per frame plus one scan of the frame's timer samples.
// The ring is sized once at construction and never grows, so the channel cannot
// perturb the thing it is measuring.
//
// `present` requires the ENGINE_PROFILE scope macros to be live. A Release
// build defines NDEBUG, which compiles them out, so configure a profiling build
// with -DENGINE_PROFILE=1 to keep the split — otherwise present reads 0 and the
// report says so rather than quietly attributing the wait to `work`.
#include <cstdint>
#include <string>
#include <vector>

#include "core/profiler.h"

class FrameStatsChannel final : public prof::IProfilerChannel {
public:
    // ~2.3 minutes at 60 Hz. Older frames fall out; a steady-state measurement
    // is what this is for, and an unbounded history would make the channel a
    // memory leak in a long editor session.
    static constexpr uint32_t kCapacity = 8192;

    // Frames excluded from the report by default. Boot, first-use shader
    // compiles and the async scene upload all land in the first second and
    // would dominate every tail percentile of a steady-state measurement.
    static constexpr uint32_t kDefaultWarmup = 60;

    struct Sample {
        uint32_t intervalNs;   // cadence
        uint32_t cpuNs;        // beginFrame -> endFrame (present included)
        uint32_t presentNs;    // the "bgfx.frame" scope
    };

    FrameStatsChannel() { m_ring.resize(kCapacity); }

    const char* channelName() const override { return "FrameStats"; }

    void beginFrame() override;
    void endFrame()   override;

    // Frames recorded since construction (or the last reset), including any
    // that have since rolled out of the ring.
    uint64_t totalFrames() const { return m_total; }

    // Drop the history. Call after a load or a mode switch so the measurement
    // window contains one regime instead of two averaged together.
    void reset() { m_total = 0; m_havePrev = false; }

    // The ring in chronological order, oldest first. `firstIndex` receives the
    // absolute frame number of element 0, so a spike can be matched against
    // whatever else the log says happened at that frame.
    std::vector<Sample> ordered(uint64_t* firstIndex = nullptr) const;

    // Multi-line distribution report on stdout: percentiles for all three
    // numbers, a dropped-frame table in units of the display cadence, an
    // interval histogram, and the worst frames by index.
    void logDistribution(const char* tag,
                         uint32_t warmup = kDefaultWarmup) const;

    // One line, for a periodic dump that should not spam a log.
    void logSummary(const char* tag, uint32_t warmup = kDefaultWarmup) const;

    // frame,interval_ms,work_ms,present_ms — for plotting a real histogram or
    // a frame-time graph offline. Returns false if the file could not be
    // opened. Writes the whole ring, warmup included; trimming is the
    // analysis step's business, not the exporter's.
    bool writeCsv(const std::string& path) const;

private:
    // Duration of a named root scope in the frame the timer channel just
    // snapshotted, or 0 if it is not there (scopes compiled out, or the frame
    // ended early). Matched by string, not by literal address: the scope lives
    // in another TU and identical literals are not required to share storage.
    static uint32_t scopeNs(const char* name);

    std::vector<Sample> m_ring;
    uint64_t m_total      = 0;    // frames ever recorded (ring index = % cap)
    uint64_t m_prevBegin  = 0;
    uint64_t m_beginNs    = 0;
    uint32_t m_intervalNs = 0;
    bool     m_havePrev   = false;
};
