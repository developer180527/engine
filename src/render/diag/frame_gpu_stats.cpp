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

    // Timing. bgfx reports raw counter ticks plus the frequency to divide by;
    // a zero frequency (or a begin==end pair) means the backend has no GPU timer,
    // which must stay distinguishable from "the GPU took no time".
    FrameTiming t;
    if (s->gpuTimerFreq > 0 && s->gpuTimeEnd > s->gpuTimeBegin) {
        t.gpuMs = double(s->gpuTimeEnd - s->gpuTimeBegin) * 1000.0
                / double(s->gpuTimerFreq);
        t.gpuTimerValid = true;
    }
    if (s->cpuTimerFreq > 0) {
        const double toMs = 1000.0 / double(s->cpuTimerFreq);
        t.cpuMs        = double(s->cpuTimeFrame) * toMs;
        t.waitSubmitMs = double(s->waitSubmit)   * toMs;
        t.waitRenderMs = double(s->waitRender)   * toMs;
    }

    ingest(c, s->numDraw, s->textureMemoryUsed, s->rtMemoryUsed,
           s->transientVbUsed, s->transientIbUsed, t);
}

void FrameGpuStats::sampleExplicit(const HandleCounts& counts, uint32_t draws,
                                   int64_t texBytes, int64_t rtBytes,
                                   int32_t transientVb, int32_t transientIb,
                                   const FrameTiming& timing) {
    ingest(counts, draws, texBytes, rtBytes, transientVb, transientIb, timing);
}

void FrameGpuStats::ingest(const HandleCounts& c, uint32_t draws, int64_t tex,
                           int64_t rt, int32_t tvb, int32_t tib,
                           const FrameTiming& t) {
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

    // Only measured frames feed the GPU average; unsupported ones would drag it
    // toward zero and make an untimed backend look infinitely fast.
    if (t.gpuTimerValid) {
        ++m_gpuTimedFrames;
        m_gpuMsSum += t.gpuMs;
        if (t.gpuMs > m_maxGpuMs) m_maxGpuMs = t.gpuMs;
    }
    m_cpuMsSum      += t.cpuMs;
    m_waitSubmitSum += t.waitSubmitMs;
    m_waitRenderSum += t.waitRenderMs;

    ++m_frames;
}

double FrameGpuStats::avgGpuMs() const {
    return m_gpuTimedFrames ? m_gpuMsSum / double(m_gpuTimedFrames) : 0.0;
}
double FrameGpuStats::avgCpuMs() const {
    return m_frames ? m_cpuMsSum / double(m_frames) : 0.0;
}
double FrameGpuStats::avgWaitSubmitMs() const {
    return m_frames ? m_waitSubmitSum / double(m_frames) : 0.0;
}
double FrameGpuStats::avgWaitRenderMs() const {
    return m_frames ? m_waitRenderSum / double(m_frames) : 0.0;
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
