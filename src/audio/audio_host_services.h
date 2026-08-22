#pragma once
// ── audio_host_services — the engine's side of EngineAudioHostServices ───────
//
// The provider gets the engine's allocators, job pool and clock through here.
// Small file, but it is the whole reason a provider does not have to bring its
// own copies of those things:
//
//   * a second thread pool beside the engine's oversubscribes the cores —
//     threads do not run in parallel, they context-switch and evict each
//     other's cache lines, and each idle pool burns power spinning
//   * a provider calling malloc is INVISIBLE to mem::Tag::Audio, so nobody can
//     answer "what does audio cost", which is the entire content of a memory
//     budget review
//
// The old AudioPlugin already pointed miniaudio's allocation callbacks at
// mem::Tag::Audio with hardcoded lambdas. That tagging is preserved exactly —
// it now travels THROUGH the ABI instead of around it, which is what makes it
// survive swapping miniaudio for something else.
//
// What the provider still owns is its real-time thread. Nothing here may be
// called from it: mem::alloc can block on a heap lock and jobs::parallelFor
// waits for workers, and both are deadline violations on the audio thread.
#include <engine/engine_audio_provider.h>

#include "core/memory/mem.h"
#include "core/profiler.h"
#include "runtime/jobs/jobs.h"

namespace audio {

namespace detail {

inline void* hostAlloc(void* /*ud*/, uint64_t size, uint64_t alignment) {
    // Tag::Audio is the point of this indirection: every decoder, mixing buffer
    // and voice object a provider allocates lands in the audio heap and shows
    // up in the engine's memory telemetry.
    return mem::alloc((size_t)size, (size_t)alignment, mem::Tag::Audio);
}

inline void hostFree(void* /*ud*/, void* ptr) { mem::free(ptr); }

inline void hostParallelFor(void* /*ud*/, const char* name, uint32_t count,
                            uint32_t grain,
                            void (*fn)(void* ctx, uint32_t begin, uint32_t end),
                            void* ctx) {
    if (!fn || count == 0) return;
    // Tools and tests run without a pool. Doing the work inline is the correct
    // degradation — the provider asked for the range to be processed, not for
    // it to be processed on another thread.
    if (!jobs::initialized()) { fn(ctx, 0, count); return; }
    jobs::parallelFor(name ? name : "audio.provider", count, grain,
                      [fn, ctx](uint32_t b, uint32_t e) { fn(ctx, b, e); });
}

inline uint32_t hostWorkerCount(void* /*ud*/) {
    return jobs::initialized() ? jobs::workerCount() : 1u;
}

inline uint64_t hostNowNs(void* /*ud*/) { return prof::nowNs(); }

} // namespace detail

// The engine's monotonic clock is prof::nowNs, so a provider's
// EngineAudioStats::hostTimeNs is directly comparable with anything else the
// engine timestamps. That is the entire reason the clock is handed over rather
// than left to the provider: two monotonic clocks share no epoch, and a
// correlation between the audio clock and a clock the engine cannot read is
// worth nothing.
inline EngineAudioHostServices hostServices() {
    EngineAudioHostServices s{};
    s.structSize  = (uint32_t)sizeof(EngineAudioHostServices);
    s.userData    = nullptr;
    s.alloc       = detail::hostAlloc;
    s.free        = detail::hostFree;
    s.parallelFor = detail::hostParallelFor;
    s.workerCount = detail::hostWorkerCount;
    s.nowNs       = detail::hostNowNs;
    return s;
}

} // namespace audio

// The statically-linked miniaudio provider. A future build could dlopen a
// module instead and nothing above this line would change — which is the point
// of the interface, and why this declaration is the ONLY thing the engine knows
// about miniaudio now.
extern "C" const EngineAudioProviderV1* engineAudioProviderV1(void);
