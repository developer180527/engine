#pragma once
// ── FrameGpuStats — what the GPU was asked to do, per frame ─────────────────
//
// ONE concern: accumulate bgfx's own per-frame counters and answer a single
// question — is the renderer allocating when it should be steady?
//
// Budget policy lives in gpu_budget.h; resident-resource accounting lives in
// resource_census.h; the profiler plumbing lives in render_stats_channel.h.
// This file only counts.
//
// THE DIAGNOSTIC THAT MATTERS is handle CHURN, not absolute counts. A steady
// scene holds a constant number of textures, buffers and framebuffers: the
// content is loaded, so moving the camera should create and destroy nothing.
//   • counts rising every frame         -> a leak
//   • counts oscillating frame to frame -> per-frame create/destroy
//   • counts flat                       -> allocation traffic is transient
//     buffers (ImGui, debug lines), which is by design — read transientVb/Ib
// The delta is the signal; the absolute number never told anyone anything.
#include <cstdint>

namespace bgfx { struct Stats; }

namespace rdiag {

// Live GPU object counts — a snapshot of "how many things exist right now",
// compared frame to frame to detect churn.
struct HandleCounts {
    uint16_t textures = 0, vertexBuffers = 0, indexBuffers = 0;
    uint16_t frameBuffers = 0, programs = 0, shaders = 0, uniforms = 0;
    uint16_t dynVertexBuffers = 0, dynIndexBuffers = 0;

    int  total() const;
    bool operator==(const HandleCounts& o) const;
    bool operator!=(const HandleCounts& o) const { return !(*this == o); }
};

enum class ChurnVerdict {
    NoData,
    Steady,        // nothing created or destroyed in the frame loop
    Occasional,    // streaming / a resize — expected
    PerFrameChurn, // create+destroy inside the frame loop
    LeakSuspected, // handle counts trend upward
};
const char* toString(ChurnVerdict v);

// Accumulated over a run. Deliberately free of bgfx types in its interface so
// callers (and tests) need no graphics headers.
class FrameGpuStats {
public:
    // Samples bgfx::getStats(). Safe to call every frame; cheap (a struct read
    // of counters bgfx already maintains). No-op before the first submitted
    // frame.
    void sample();

    // Test seam: feed counters directly, no GPU required.
    void sampleExplicit(const HandleCounts& counts, uint32_t draws,
                        int64_t texBytes, int64_t rtBytes,
                        int32_t transientVb, int32_t transientIb);

    ChurnVerdict churn() const;

    uint64_t frames()       const { return m_frames; }
    uint64_t churnFrames()  const { return m_churnFrames; }
    int      handleDrift()  const { return m_handleDrift; }
    double   avgDraws()     const;
    uint32_t maxDraws()     const { return m_maxDraws; }
    int64_t  textureBytes() const { return m_texBytes; }
    int64_t  rtBytes()      const { return m_rtBytes; }
    int64_t  peakVramBytes()const { return m_peakVram; }
    double   avgTransientVbKb() const;
    double   avgTransientIbKb() const;
    const HandleCounts& lastCounts() const { return m_last; }

private:
    void ingest(const HandleCounts& c, uint32_t draws, int64_t tex, int64_t rt,
                int32_t tvb, int32_t tib);

    HandleCounts m_last{};
    uint64_t m_frames = 0, m_churnFrames = 0;
    uint64_t m_draws = 0, m_transientVb = 0, m_transientIb = 0;
    uint32_t m_maxDraws = 0;
    int      m_handleDrift = 0;
    int64_t  m_texBytes = 0, m_rtBytes = 0, m_peakVram = 0;
};

} // namespace rdiag
