#include "render/diag/frame_gpu_stats.h"

#include <bgfx/bgfx.h>

namespace rdiag {

int HandleCounts::total() const {
    return textures + vertexBuffers + indexBuffers + frameBuffers
         + programs + shaders + uniforms + dynVertexBuffers + dynIndexBuffers;
}

bool HandleCounts::operator==(const HandleCounts& o) const {
    return textures == o.textures && vertexBuffers == o.vertexBuffers
        && indexBuffers == o.indexBuffers && frameBuffers == o.frameBuffers
        && programs == o.programs && shaders == o.shaders
        && uniforms == o.uniforms
        && dynVertexBuffers == o.dynVertexBuffers
        && dynIndexBuffers == o.dynIndexBuffers;
}

const char* toString(ChurnVerdict v) {
    switch (v) {
        case ChurnVerdict::NoData:        return "no frames sampled";
        case ChurnVerdict::Steady:        return "STEADY (no per-frame create/destroy)";
        case ChurnVerdict::Occasional:    return "occasional (streaming/resize — expected)";
        case ChurnVerdict::PerFrameChurn: return "PER-FRAME CHURN (create/destroy in the frame loop)";
        case ChurnVerdict::LeakSuspected: return "LEAK SUSPECTED (handles trend upward)";
    }
    return "?";
}

void FrameGpuStats::sample() {
    // Reflects the LAST SUBMITTED frame, so it only means anything once at
    // least one frame has gone through.
    const bgfx::Stats* s = bgfx::getStats();
    if (!s) return;

    HandleCounts c;
    c.textures         = s->numTextures;
    c.vertexBuffers    = s->numVertexBuffers;
    c.indexBuffers     = s->numIndexBuffers;
    c.frameBuffers     = s->numFrameBuffers;
    c.programs         = s->numPrograms;
    c.shaders          = s->numShaders;
    c.uniforms         = s->numUniforms;
    c.dynVertexBuffers = s->numDynamicVertexBuffers;
    c.dynIndexBuffers  = s->numDynamicIndexBuffers;

    ingest(c, s->numDraw, s->textureMemoryUsed, s->rtMemoryUsed,
           s->transientVbUsed, s->transientIbUsed);
}

void FrameGpuStats::sampleExplicit(const HandleCounts& counts, uint32_t draws,
                                   int64_t texBytes, int64_t rtBytes,
                                   int32_t transientVb, int32_t transientIb) {
    ingest(counts, draws, texBytes, rtBytes, transientVb, transientIb);
}

void FrameGpuStats::ingest(const HandleCounts& c, uint32_t draws, int64_t tex,
                           int64_t rt, int32_t tvb, int32_t tib) {
    // The first sample establishes the baseline — it cannot be churn, because
    // there is nothing to compare against.
    if (m_frames > 0 && c != m_last) {
        ++m_churnFrames;
        m_handleDrift += c.total() - m_last.total();
    }
    m_last = c;

    m_draws += draws;
    if (draws > m_maxDraws) m_maxDraws = draws;

    m_transientVb += (uint64_t)(tvb > 0 ? tvb : 0);
    m_transientIb += (uint64_t)(tib > 0 ? tib : 0);

    m_texBytes = tex;
    m_rtBytes  = rt;
    if (tex + rt > m_peakVram) m_peakVram = tex + rt;

    ++m_frames;
}

ChurnVerdict FrameGpuStats::churn() const {
    if (m_frames == 0)                 return ChurnVerdict::NoData;
    if (m_churnFrames == 0)            return ChurnVerdict::Steady;
    // Upward drift is a leak regardless of how often it happens; a handful of
    // handles is normal jitter, a monotone climb is not.
    if (m_handleDrift > 8)             return ChurnVerdict::LeakSuspected;
    if (m_churnFrames * 4 > m_frames)  return ChurnVerdict::PerFrameChurn;
    return ChurnVerdict::Occasional;
}

double FrameGpuStats::avgDraws() const {
    return m_frames ? (double)m_draws / (double)m_frames : 0.0;
}
double FrameGpuStats::avgTransientVbKb() const {
    return m_frames ? (double)m_transientVb / (double)m_frames / 1024.0 : 0.0;
}
double FrameGpuStats::avgTransientIbKb() const {
    return m_frames ? (double)m_transientIb / (double)m_frames / 1024.0 : 0.0;
}

} // namespace rdiag
