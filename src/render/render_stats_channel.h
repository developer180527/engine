#pragma once
// ── RenderStatsChannel — what the GPU is actually being asked to do ─────────
//
// Instruments' graphics track showed the editor making an unreasonable number
// of GPU resource allocations. Chasing that from a platform profiler is
// guesswork: it reports Metal objects, not which engine system created them.
// This channel reports the same truth from inside bgfx, per frame, on every
// backend — so the question stops being "Metal looks busy" and becomes "these
// handle counts moved on this frame".
//
// THE DIAGNOSTIC THAT MATTERS is handle CHURN. A steady-state renderer holds a
// constant number of textures, buffers and framebuffers: the scene is loaded,
// nothing should be created or destroyed while the camera merely moves.
//   • counts rising every frame        -> a leak
//   • counts oscillating frame to frame -> per-frame create/destroy (the
//     "unreasonable allocations" smell)
//   • counts flat                      -> allocations are transient-buffer
//     traffic (ImGui, debug lines), which is by design — read transientVb/Ib
//     instead
// Reporting the delta is the whole point; the absolute number never told
// anyone anything.
//
// It is also the VRAM budget instrument. The target machine is an Intel UHD
// 630 sharing 128 MB with a 4 GB system, so texture + render-target memory is
// a hard constraint, not a curiosity. Numbers here are what make "tuned down"
// a measurable setting rather than a hope.
#include "core/profiler.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <cstdio>

// Live GPU object counts. Compared frame to frame to detect churn; a snapshot
// of "how many things exist right now".
struct RenderHandleCounts {
    uint16_t textures = 0, vertexBuffers = 0, indexBuffers = 0;
    uint16_t frameBuffers = 0, programs = 0, shaders = 0, uniforms = 0;
    uint16_t dynVertexBuffers = 0, dynIndexBuffers = 0;

    bool operator!=(const RenderHandleCounts& o) const {
        return textures != o.textures || vertexBuffers != o.vertexBuffers
            || indexBuffers != o.indexBuffers || frameBuffers != o.frameBuffers
            || programs != o.programs || shaders != o.shaders
            || uniforms != o.uniforms
            || dynVertexBuffers != o.dynVertexBuffers
            || dynIndexBuffers != o.dynIndexBuffers;
    }
    int total() const {
        return textures + vertexBuffers + indexBuffers + frameBuffers
             + programs + shaders + uniforms + dynVertexBuffers
             + dynIndexBuffers;
    }
};

class RenderStatsChannel final : public prof::IProfilerChannel {
public:
    const char* channelName() const override { return "RenderStats"; }

    void beginFrame() override {}

    void endFrame() override {
        // getStats() is cheap (a struct read of counters bgfx already keeps),
        // but it reflects the LAST SUBMITTED frame, so it is only meaningful
        // once at least one frame has been submitted.
        const bgfx::Stats* s = bgfx::getStats();
        if (!s) return;

        RenderHandleCounts now;
        now.textures         = s->numTextures;
        now.vertexBuffers    = s->numVertexBuffers;
        now.indexBuffers     = s->numIndexBuffers;
        now.frameBuffers     = s->numFrameBuffers;
        now.programs         = s->numPrograms;
        now.shaders          = s->numShaders;
        now.uniforms         = s->numUniforms;
        now.dynVertexBuffers = s->numDynamicVertexBuffers;
        now.dynIndexBuffers  = s->numDynamicIndexBuffers;

        if (m_frames > 0 && now != m_last) {
            ++m_churnFrames;
            const int delta = now.total() - m_last.total();
            if (delta > m_maxGrowth) m_maxGrowth = delta;
            m_netHandleDrift += delta;
        }
        m_last = now;

        m_draws += s->numDraw;
        if (s->numDraw > m_maxDraws) m_maxDraws = s->numDraw;

        m_transientVb += (uint64_t)(s->transientVbUsed > 0 ? s->transientVbUsed : 0);
        m_transientIb += (uint64_t)(s->transientIbUsed > 0 ? s->transientIbUsed : 0);

        m_texMem = s->textureMemoryUsed;
        m_rtMem  = s->rtMemoryUsed;
        m_gpuMem = s->gpuMemoryUsed;
        m_gpuMax = s->gpuMemoryMax;
        if (m_texMem + m_rtMem > m_peakVram) m_peakVram = m_texMem + m_rtMem;

        ++m_frames;
    }

    // Snapshot of the most recent frame plus the churn verdict. Called from a
    // dev runner's periodic dump, or at shutdown.
    void report(const char* tag) const {
        if (m_frames == 0) {
            std::printf("[RenderStats] %s — no frames submitted\n", tag);
            return;
        }
        const double mb = 1.0 / (1024.0 * 1024.0);
        std::printf(
            "[RenderStats] %s — %llu frames\n"
            "      draws        avg %.1f   max %u\n"
            "      VRAM         tex %.1f MB   rt %.1f MB   peak %.1f MB%s\n"
            "      transient    vb %.1f KB/frame   ib %.1f KB/frame\n"
            "      handles      %d live (tex %u vb %u ib %u fb %u prog %u uni %u)\n"
            "      CHURN        %llu/%llu frames changed handle counts%s\n",
            tag, (unsigned long long)m_frames,
            (double)m_draws / (double)m_frames, m_maxDraws,
            (double)m_texMem * mb, (double)m_rtMem * mb, (double)m_peakVram * mb,
            m_gpuMax > 0 ? "" : "  (driver reports no VRAM budget)",
            (double)m_transientVb / (double)m_frames / 1024.0,
            (double)m_transientIb / (double)m_frames / 1024.0,
            m_last.total(), m_last.textures, m_last.vertexBuffers,
            m_last.indexBuffers, m_last.frameBuffers, m_last.programs,
            m_last.uniforms,
            (unsigned long long)m_churnFrames, (unsigned long long)m_frames,
            churnVerdict());
    }

    // The one-line answer to "is the renderer allocating when it shouldn't?"
    const char* churnVerdict() const {
        if (m_churnFrames == 0)            return "  -> STEADY (no per-frame create/destroy)";
        if (m_netHandleDrift > 8)          return "  -> LEAK SUSPECTED (handles trend upward)";
        if (m_churnFrames * 4 > m_frames)  return "  -> PER-FRAME CHURN (create/destroy in the frame loop)";
        return "  -> occasional (streaming/resize — expected)";
    }

    uint64_t frames()      const { return m_frames; }
    uint64_t churnFrames() const { return m_churnFrames; }
    int      netDrift()    const { return m_netHandleDrift; }
    int64_t  peakVram()    const { return m_peakVram; }

private:
    RenderHandleCounts m_last{};
    uint64_t m_frames = 0, m_churnFrames = 0;
    uint64_t m_draws = 0, m_transientVb = 0, m_transientIb = 0;
    uint32_t m_maxDraws = 0;
    int      m_maxGrowth = 0, m_netHandleDrift = 0;
    int64_t  m_texMem = 0, m_rtMem = 0, m_gpuMem = 0, m_gpuMax = 0, m_peakVram = 0;
};
