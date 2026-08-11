#pragma once
/* ── EngineAudioProviderV1 — audio, replaceable down to the device ───────────
 *
 * The contract a REPLACEMENT audio engine implements: miniaudio (the default),
 * an FMOD or Wwise adapter, a Rust spatial engine, someone's ray-traced audio
 * experiment. The engine calls this; nothing here calls back into the engine.
 *
 * ── The one decision everything else follows from ────────────────────────────
 * THE PROVIDER OWNS THE DEVICE, THE REAL-TIME THREAD, AND THE MIXER.
 *
 * The engine never fills a buffer and never runs on the audio thread. If the
 * engine owned the device and called the provider to fill buffers, this ABI
 * would sit INSIDE the real-time callback and inherit every constraint on both
 * sides of it — allocation, locking and unwinding rules would become joint
 * problems. Provider-owns-device is what makes the boundary cost structurally
 * zero: at 48 kHz with a 128-frame buffer the mixer runs 375 times a second and
 * crosses this interface ZERO times.
 *
 * What crosses is CONTROL, at frame rate: ~1 batched call per frame, a few
 * nanoseconds each, against a 16 600 µs budget.
 *
 * ── Why this interface has no HRTF, no Atmos, no ray-tracing switches ────────
 * THE ENGINE MODELS THE SCENE. THE PROVIDER MODELS THE AUDIO.
 *
 * Spatial audio, object-based rendering, Atmos, binaural/HRTF and acoustic
 * propagation all need the SAME three things from the engine: emitter poses, a
 * listener pose, and the world's acoustic geometry. Whether that becomes stereo
 * panning, an object bed, ambisonics or a path-traced impulse response is
 * entirely the provider's business, configured through the PROVIDER's own
 * config — never through this header.
 *
 * That is the whole complexity story. A `setHRTF`/`setAtmosBed`/`enableRayTrace`
 * interface grows with every audio technology ever invented, and every provider
 * has to implement all of it. This one does not grow: the features above add
 * zero functions. Anything genuinely provider-specific goes through setParam,
 * which the engine forwards without interpreting.
 *
 * The test to apply before adding a function here: COULD AN FMOD OR WWISE
 * ADAPTER IMPLEMENT IT? Both are event/parameter systems with C APIs. If the
 * answer is no, it is describing our implementation rather than the scene.
 *
 * ── Three rates, three budgets ───────────────────────────────────────────────
 *   48 kHz     mixing, filtering, HRTF convolution   provider, ZERO ABI
 *   60-120 Hz  emitter + listener updates            ONE batched call/frame
 *   5-30 Hz    propagation, ray tracing, reverb      provider's own workers
 *
 * The expensive AAA features live on the bottom row: a tracer running at 15 Hz
 * on a worker produces gains, delays and filter coefficients that the mixer
 * applies per sample. Updating occlusion 15 times a second is inaudible as an
 * update — you hear a smooth change, not steps. So the costly work is off the
 * real-time thread, off the frame thread, and never touches this ABI.
 *
 * ── ABI rules (see engine_api_table.h for the full contract) ─────────────────
 * Fixed-width types only: no `bool`, no `size_t`, no plain `enum`. Structs are
 * append-only and carry structSize. No C++ types cross. No exception or panic
 * may escape any function here — a Rust provider must use `panic = "abort"` or
 * catch_unwind at every entry, and unwinding out of the audio thread is the
 * worst place to discover otherwise.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENGINE_AUDIO_PROVIDER_V 1

/* The symbol a provider module exports. Returns a static table; the engine
 * never frees it. */
#define ENGINE_AUDIO_PROVIDER_ENTRY "engineAudioProviderV1"

/* ── Results — no exceptions cross this boundary ─────────────────────────── */
typedef int32_t EngineAudioResult;
#define ENGINE_AUDIO_OK            0
#define ENGINE_AUDIO_E_FAIL       -1   /* unspecified provider failure       */
#define ENGINE_AUDIO_E_NO_DEVICE  -2   /* no output device (headless/server) */
#define ENGINE_AUDIO_E_BAD_ARG    -3
#define ENGINE_AUDIO_E_BAD_DATA   -4   /* undecodable sound bytes            */
#define ENGINE_AUDIO_E_OOM        -5
#define ENGINE_AUDIO_E_UNSUPPORTED -6  /* optional capability absent         */

/* Opaque handles. Zero is always "none" so zero-initialised structs are safe. */
typedef uint64_t EngineSoundId;   /* a decoded/streamable sound resource */
typedef uint64_t EngineVoiceId;   /* one playing instance of a sound     */
#define ENGINE_AUDIO_NO_SOUND ((EngineSoundId)0)
#define ENGINE_AUDIO_NO_VOICE ((EngineVoiceId)0)

/* ── Flags ───────────────────────────────────────────────────────────────── */
#define ENGINE_AUDIO_F_LOOP     0x1u
#define ENGINE_AUDIO_F_SPATIAL  0x2u  /* positioned; else played "in the head" */
#define ENGINE_AUDIO_F_STREAM   0x4u  /* stream from the resource, do not fully
                                       * decode — music and long ambience     */

/* ── Device ──────────────────────────────────────────────────────────────── */
/* Zero means "provider chooses". bufferFrames is the DOMINANT latency term:
 * 128 frames @ 48 kHz = 2.67 ms per callback. Smaller is tighter and costs
 * more callbacks per second; the provider may clamp to what the device allows
 * and reports what it actually got through getStats. */
typedef struct EngineAudioDeviceDesc {
    uint32_t structSize;
    uint32_t sampleRate;      /* 0 = provider default (typically 48000) */
    uint32_t bufferFrames;    /* 0 = provider default                   */
    uint32_t channelHint;     /* 0 = auto. A HINT: a provider doing Atmos or
                               * ambisonics decides its own output layout. */
} EngineAudioDeviceDesc;

/* ── Scene state ─────────────────────────────────────────────────────────── */
typedef struct EngineAudioListener {
    /* Every other struct in this header carries one; this one did not, which
     * made it the only member of the scene state that could never be extended.
     * Passed by pointer, so a provider reads it and adapts. */
    uint32_t structSize;
    float position[3];
    float velocity[3];   /* metres/sec, for doppler */
    float forward[3];
    float up[3];
} EngineAudioListener;

/* One row of the bulk update. THE batching primitive: a busy scene updates
 * hundreds of emitters per frame, and doing that as hundreds of calls is the
 * per-object anti-pattern this interface exists to avoid. Static emitters
 * (ambience, machinery) need no row at all — send only what moved.
 *
 * NO structSize HERE, ON PURPOSE — it is an array element, and a size field per
 * row would cost 4 bytes times hundreds every frame to say the same thing every
 * time. The stride is passed ONCE instead, to updateEmitters. That is what makes
 * this struct extensible at all: without it, adding a field would silently
 * misalign every read in every provider already compiled, because the provider
 * indexes the array with ITS OWN definition. Vulkan and D3D12 both pass strides
 * for exactly this reason.
 *
 * A provider must therefore walk the array with the stride the ENGINE gave it,
 * never with sizeof(EngineAudioEmitterUpdate), and must ignore trailing bytes it
 * does not recognise. */
typedef struct EngineAudioEmitterUpdate {
    EngineVoiceId voice;
    float    position[3];
    float    velocity[3];
    float    volume;      /* linear gain, 1.0 = unattenuated */
    float    pitch;       /* 1.0 = unmodified */
    uint32_t flags;       /* ENGINE_AUDIO_F_*; LOOP/STREAM ignored here */
} EngineAudioEmitterUpdate;

typedef struct EngineAudioPlayDesc {
    uint32_t      structSize;
    EngineSoundId sound;
    /* SAMPLE-ACCURATE SCHEDULING. 0 = start as soon as possible. Otherwise an
     * absolute value on the provider's own clock (getStats().samplesPlayed),
     * so the mixer starts the voice at an exact sample WITHIN a buffer rather
     * than at the buffer boundary.
     *
     * Without this, a sound triggered mid-frame quantises to the next callback
     * — audible on rapid fire and anything rhythmic. It is one uint64 here and
     * it cannot be added later without changing every provider, which is why
     * it exists in v1 with no implementation behind it yet. */
    uint64_t startSampleTime;
    float    position[3];
    float    velocity[3];
    float    volume;
    float    pitch;
    uint32_t flags;
    /* Provider-defined attenuation model id; 0 = the provider's default. NOT a
     * curve the engine describes: distance falloff is an audio-design decision
     * and belongs with the provider's own configuration. */
    uint32_t attenuationId;
} EngineAudioPlayDesc;

/* ── Acoustic geometry ───────────────────────────────────────────────────── */
/* The one thing that unlocks occlusion, propagation and ray-traced audio, and
 * the reason it is DATA rather than a callback: a provider tracing rays must
 * not call back into the engine. Wrong thread (its workers, not ours), wrong
 * rate (5-30 Hz, not per-frame), and it would couple the two lifetimes.
 *
 * The provider copies or builds its own acceleration structure during the call
 * and owns it afterwards; the engine's arrays need not outlive the call.
 * Geometry changes rarely — a load-time handoff, plus occasional updates for
 * doors and destruction.
 *
 * materialIds are opaque to the engine: the provider maps them to absorption
 * and scattering through its own material table. That is the same "express
 * intent, not implementation" rule as everything else here. */
typedef struct EngineAcousticGeometry {
    uint32_t        structSize;
    const float*    vertices;      /* xyz triples, world space  */
    uint32_t        vertexCount;
    const uint32_t* indices;       /* triangle list             */
    uint32_t        indexCount;
    const uint32_t* materialIds;   /* one per TRIANGLE, or null */
} EngineAcousticGeometry;

/* ── Diagnostics ─────────────────────────────────────────────────────────── */
typedef struct EngineAudioStats {
    uint32_t structSize;
    uint32_t activeVoices;
    uint32_t sampleRate;       /* what the device ACTUALLY runs at */
    uint32_t bufferFrames;     /* what it ACTUALLY got             */
    /* THE number that matters. A callback that missed its deadline is an
     * audible click, and it is the only audio failure a player notices
     * immediately. A conformance test asserts this stays zero under load. */
    uint64_t callbackOverruns;
    /* The provider's clock, for computing startSampleTime. Monotonic. */
    uint64_t samplesPlayed;
    float    cpuLoad;          /* 0..1 of the real-time budget, -1 = unknown */
} EngineAudioStats;

/* ── The provider ────────────────────────────────────────────────────────────
 * THREADING, per function. "Game thread" means the engine's main/sim thread;
 * these may block briefly. NOTHING here is called from the audio thread.
 *
 * The engine's side of the bargain: it will not call two of these concurrently
 * for the same instance — WITH ONE EXCEPTION, getStats, which is marked "any
 * thread" below and may therefore overlap any other call. A diagnostics overlay
 * reading stats while the game thread calls play() is exactly that overlap, so
 * getStats must be safe against it: publish the counters atomically (or under a
 * lock the mixer never takes) rather than assuming the no-concurrency rule
 * covers it. Without this carve-out the two statements contradicted each other,
 * and a provider author following the general rule would have raced.
 *
 * The provider's side: none of these may block on the audio thread — a mutex
 * shared with the mixer turns a play() call into a frame hitch, which is a
 * priority inversion that presents as inexplicable stutter far from its
 * cause. */
typedef struct EngineAudioProviderV1 {
    uint32_t version;      /* ENGINE_AUDIO_PROVIDER_V */
    uint32_t structSize;

    /* ── Lifecycle (game thread) ─────────────────────────────────────────── */
    /* Opens the device and starts the real-time thread. A host with no output
     * device is NORMAL, not an error to abort on: return E_NO_DEVICE and the
     * engine runs silent (dedicated servers, CI). */
    EngineAudioResult (*create)(const EngineAudioDeviceDesc* desc, void** outSelf);
    void (*destroy)(void* self);
    /* Application focus loss / OS interruption. suspended != 0 stops the
     * device without destroying voices. */
    void (*suspend)(void* self, int32_t suspended);

    /* ── Resources (game thread; may block, may decode) ──────────────────── */
    /* `bytes` is a cooked audio asset. debugName is for the provider's logs
     * and may be null.
     *
     * `flags` takes ENGINE_AUDIO_F_STREAM only; LOOP and SPATIAL are playback
     * decisions and are ignored here.
     *
     * BUFFER LIFETIME, and it is decided HERE because it cannot be decided
     * anywhere else:
     *
     *   flags == 0 (fully decoded) — the provider decodes during this call and
     *     owns the result; the engine may free `bytes` the moment it returns.
     *   ENGINE_AUDIO_F_STREAM      — the provider may read from `bytes` for as
     *     long as the sound exists, so THE ENGINE MUST KEEP IT ALIVE until
     *     destroySound. Music is tens of megabytes; decoding it up front to
     *     avoid this would cost far more resident memory than the file does,
     *     and a memory-mapped file keeps it virtual.
     *
     * STREAM USED TO BE A PLAY-TIME FLAG, and that made this contract
     * unsatisfiable by either side: the provider had to choose retain-or-decode
     * during createSound, before any play() told it which, and the engine had to
     * know whether to keep `bytes` alive on the strength of a call that had not
     * happened yet. Streaming is a property of the RESOURCE, so it belongs on
     * the call that creates one.
     *
     * A provider that cannot stream from memory may decode instead — correct,
     * just heavier. It must NOT retain a pointer it was not promised. */
    EngineAudioResult (*createSound)(void* self, const void* bytes, uint64_t byteCount,
                                     uint32_t flags, const char* debugName,
                                     EngineSoundId* outSound);
    void (*destroySound)(void* self, EngineSoundId sound);

    /* ── Playback (game thread) ──────────────────────────────────────────── */
    /* Returns NO_VOICE if the sound is invalid or the provider's voice budget
     * is full. Voice limiting is the PROVIDER's policy — the engine does not
     * tell it how many voices to run or which to cull. */
    EngineVoiceId (*play)(void* self, const EngineAudioPlayDesc* desc);
    void (*stop)(void* self, EngineVoiceId voice, uint32_t fadeMs);

    /* ── Per-frame scene sync (game thread) ──────────────────────────────── */
    /* ONE call carrying every emitter that moved. See EngineAudioEmitterUpdate
     * for why this is not one call per emitter, and why `stride` is here rather
     * than a size field on every row. Walk the array with `stride`, never with
     * sizeof — that is what lets the engine add a field without breaking every
     * provider already built. */
    void (*updateEmitters)(void* self, const EngineAudioEmitterUpdate* updates,
                           uint32_t count, uint32_t stride);
    void (*setListener)(void* self, const EngineAudioListener* listener);

    /* ── Acoustic scene (game thread, rare) ──────────────────────────────── */
    /* E_UNSUPPORTED from a provider that does no propagation is expected and
     * fine — the engine keeps working, it simply has no occlusion. */
    EngineAudioResult (*setGeometry)(void* self, const EngineAcousticGeometry* geom);

    /* ── Passthrough (game thread) ───────────────────────────────────────── */
    /* The escape hatch that keeps this interface from growing: a Wwise RTPC, a
     * "switch to the expensive HRTF dataset" toggle, an experimental knob that
     * exists for one afternoon. objectId is a voice, or 0 for global. The
     * engine NEVER interprets nameHash — it forwards it. */
    void (*setParam)(void* self, uint64_t objectId, uint64_t nameHash, float value);

    /* ── Diagnostics (any thread) ────────────────────────────────────────── */
    void (*getStats)(void* self, EngineAudioStats* outStats);
} EngineAudioProviderV1;

/* ── Frozen layout ───────────────────────────────────────────────────────────
 * The same mechanism engine_api_table.h uses, and here for a second reason: the
 * conformance suite (tests/audio_conformance) hand-transcribes these structs as
 * Rust `#[repr(C)]` and asserts hardcoded sizes — against ITS OWN layout. That
 * catches Rust-side drift and is blind to C-side drift, so a change here would
 * leave the suite green while the two halves disagreed. These assertions are the
 * other half of that check: the numbers must match the ones in
 * audio_conformance/src/lib.rs, and now neither side can move alone.
 *
 * 64-bit only, deliberately — every pointer-bearing struct here would differ on
 * a 32-bit target, and there is none. */
#if defined(__cplusplus)
#  define ENGINE_AUDIO_FROZEN(t, n) static_assert(sizeof(t) == (n), #t " layout changed")
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define ENGINE_AUDIO_FROZEN(t, n) _Static_assert(sizeof(t) == (n), #t " layout changed")
#else
/* Pre-C11 has no static assert. A provider on such a compiler gets no check
 * here; the C++ and C11 builds in this repo do, which is where drift is caught. */
#  define ENGINE_AUDIO_FROZEN(t, n)
#endif

#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
ENGINE_AUDIO_FROZEN(EngineAudioDeviceDesc,     16);
ENGINE_AUDIO_FROZEN(EngineAudioListener,       52);
ENGINE_AUDIO_FROZEN(EngineAudioEmitterUpdate,  48);
ENGINE_AUDIO_FROZEN(EngineAudioPlayDesc,       64);
ENGINE_AUDIO_FROZEN(EngineAcousticGeometry,    48);
ENGINE_AUDIO_FROZEN(EngineAudioStats,          40);
ENGINE_AUDIO_FROZEN(EngineAudioProviderV1,    104);
#endif

/* The module entry point: `const EngineAudioProviderV1* engineAudioProviderV1(void)` */
typedef const EngineAudioProviderV1* (*EngineAudioProviderGetV1Fn)(void);

#ifdef __cplusplus
}
#endif
