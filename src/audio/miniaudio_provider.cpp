// ── miniaudio_provider — miniaudio behind EngineAudioProviderV1 ──────────────
//
// The first real implementation of the audio provider ABI, and therefore the
// thing that decides whether that ABI was designed or merely written down.
//
// It is built TWO ways from this one source:
//
//   engine_audio_miniaudio          a static library the engine links — today's
//                                   path, no dlopen in a shipping game
//   engine_audio_miniaudio_module   a shared module the Rust conformance suite
//                                   dlopens and holds to the contract
//
// Same source, same table, so the suite tests the code the game actually runs.
//
// ── What makes this a PROVIDER and not just more engine ─────────────────────
// It includes exactly two things: miniaudio and the ABI header. No engine
// symbol is referenced and no engine header is included, so the module build
// links against nothing at all. Every engine facility it uses arrives through
// EngineAudioHostServices, handed to create(). That is what lets it be a .so a
// third party could have written — and the property that would silently rot if
// nothing ever built it standalone, which is why the module target exists.
//
// ── Where the memory goes ───────────────────────────────────────────────────
// miniaudio's allocation callbacks are pointed at services->alloc/free, so
// every decoder, mixing buffer and voice object lands in the host's audio heap.
// The engine puts mem::Tag::Audio behind that, so this is the SAME tagging the
// old AudioPlugin did with hardcoded lambdas — it now travels through the ABI
// instead of around it. The conformance suite substitutes a counting allocator,
// which turns "the provider used host services" into an assertion.
//
// ── Nothing allocates once the device is running ────────────────────────────
// Sounds and voices live in fixed-capacity arrays inside a single block taken
// at create(). No map, no rehash, no per-play allocation. Handles are
// index+generation, so a stale id is REJECTED rather than silently rebound to
// whatever reused the slot — and the engine legitimately sends stale voice ids,
// because a voice can finish between frames. That is a correctness property,
// not a micro-optimisation.
#include <engine/engine_audio_provider.h>

#include "miniaudio.h"

// Explicit, not inherited: miniaudio.h happens to pull both in, which is
// exactly the transitive dependency that builds on libc++ and fails on
// libstdc++. scripts/check_std_includes.py flagged these the moment they were
// written, which is the whole point of having it.
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include <atomic>     // the device clock, published from the real-time thread
#include <chrono>     // provider-private monotonic clock (NOT a host call)
#include <new>        // placement new for the single host-allocated block
#include <string.h>

namespace {

// Committed up front. A real title tunes these; what matters for the contract
// is that they are FIXED.
constexpr uint32_t kMaxSounds = 256;
constexpr uint32_t kMaxVoices = 128;

// ── Handles ─────────────────────────────────────────────────────────────────
// (generation << 32) | (index + 1). Zero is never valid, which is what makes
// NO_SOUND / NO_VOICE work with zero-initialised structs.
inline uint64_t makeId(uint32_t index, uint32_t gen) {
    return ((uint64_t)gen << 32) | (uint64_t)(index + 1);
}
inline bool splitId(uint64_t id, uint32_t cap, uint32_t* outIdx, uint32_t* outGen) {
    const uint32_t low = (uint32_t)(id & 0xFFFFFFFFu);
    if (low == 0 || low > cap) return false;
    *outIdx = low - 1;
    *outGen = (uint32_t)(id >> 32);
    return true;
}

struct Sound {
    uint32_t generation = 0;
    bool     inUse      = false;

    // Two shapes, and the difference is who owns the samples.
    //
    //   decoded — decoded up front into `pcm`, which WE own, so the engine may
    //     free its bytes the moment createSound returns. Many voices play it at
    //     once: each gets its own cursor over the shared PCM via
    //     ma_audio_buffer_ref, so rapid-fire SFX costs one copy, not N.
    //
    //   streaming — a ma_decoder pulling from the engine's buffer (F_STREAM) or
    //     through its read callback (createStream). A decoder holds ONE cursor,
    //     so a streamed sound supports one voice at a time. That is the right
    //     trade for what it exists for: 60 MB of music, played once.
    bool       streaming  = false;
    bool       decoderInit = false;
    ma_decoder decoder{};
    void*      pcm        = nullptr;
    ma_uint64  frames     = 0;
    ma_uint32  channels   = 0;

    // createStream only. The ABI's read is ABSOLUTE (so a loop can seek) while
    // ma_decoder's callback is sequential, so the cursor lives here.
    EngineAudioStreamSource src{};
    uint64_t                cursor = 0;
    bool                    isPull = false;
};

struct Voice {
    uint32_t generation = 0;
    bool     inUse      = false;
    bool     soundInit  = false;
    bool     usesRef    = false;
    ma_sound sound{};
    ma_audio_buffer_ref ref{};
    uint32_t soundIndex = 0;
};

struct Provider {
    EngineAudioHostServices host{};
    ma_allocation_callbacks maAlloc{};

    // ── The provider owns the device, so it owns the clock ──────────────────
    // ma_engine will happily create its own device, and the first version of
    // this file let it. That was wrong in a way only the conformance suite
    // caught: ma_engine_get_time_in_pcm_frames is the NODE GRAPH's time, which
    // advances only when the graph is read — so with nothing playing it does
    // not move at all. As a device clock it is unusable, and the suite's
    // clock-correlation check failed with "0.0 ms of samples vs 125.2 ms of
    // host", which is exactly the bug that assertion was written to find.
    //
    // Passing our own ma_device (engine config's pDevice) means WE run the data
    // callback. Three things follow, all of them contract obligations:
    //   * samplesPlayed becomes a real free-running device clock
    //   * callbackOverruns becomes measurable instead of hardcoded 0
    //   * desc->bufferFrames is actually honoured
    ma_device  device{};
    bool       deviceInit = false;
    // True when this host has no DEPENDABLE audio hardware, so timing-based
    // assertions mean nothing. Written before the release store below, so the
    // callback's acquire makes it visible.
    //
    // Covers two situations that look different and are the same: we fell back to
    // miniaudio's null backend, OR the OS handed us a device that is not backed
    // by real hardware. The second is what a Linux CI runner actually does —
    // ALSA has no card, spews "cannot find card '0'", and then returns a working
    // 48 kHz device anyway. So a flag meaning "we used the null backend" was too
    // narrow: it never triggered, and the callback counted 9 phantom overruns
    // against a dummy device's irregular cadence.
    bool       noHardware = false;
    ma_engine  engine{};
    // ATOMIC, and published with RELEASE as the last step of create.
    //
    // It was a plain bool, and TSan caught the read on CoreAudio's callback
    // thread against the write in maCreate. The ORDERING was already right —
    // engineInit is set before ma_device_start, exactly as the comment there
    // says — but ordering in the source is not a happens-before edge, and the
    // audio callback thread already exists by then (ma_device_init creates it),
    // so there was nothing making the write visible to it. Benign in practice on
    // arm64 with an opaque call in between; still undefined, and still the kind
    // of thing that stops being benign when a compiler inlines more.
    std::atomic<bool> engineInit{false};
    ma_uint32  sampleRate   = 0;
    ma_uint32  bufferFrames = 0;
    ma_uint32  channels     = 0;

    // Written on the real-time thread, read on any thread by getStats.
    std::atomic<uint64_t> framesPlayed{0};
    std::atomic<uint64_t> overruns{0};
    // lastCallbackNs really is callback-local: written and read only on the
    // audio thread.
    uint64_t   lastCallbackNs = 0;
    // expectedPeriodNs is NOT, and the comment that used to cover both claimed it
    // was. It is computed in maCreate on the creating thread and read in the
    // callback on the audio thread — a second race in the same function, hidden
    // behind a sentence that said it could not happen. Grouping a
    // cross-thread field under a "callback-local" comment is how it stayed
    // invisible.
    std::atomic<uint64_t> expectedPeriodNs{0};

    Sound sounds[kMaxSounds];
    Voice voices[kMaxVoices];
    uint32_t nextSoundGen = 1;
    uint32_t nextVoiceGen = 1;
};

inline Provider* prov(void* p) { return static_cast<Provider*>(p); }

// ── Host-backed allocation for miniaudio ────────────────────────────────────
// Every block carries its SIZE in a 16-byte prefix, because realloc cannot be
// implemented correctly without it.
//
// THE BUG THIS FIXES was a heap over-read on every sound decode. The host
// interface has no realloc — deliberately, one fewer thing every host and every
// provider must agree on — so this shim did alloc + copy + free, and copied the
// NEW size out of the OLD block:
//
//     memcpy(n, ptr, sz);   // sz is the NEW size
//
// The comment above it claimed that was "safe only while miniaudio uses this
// path to GROW". That reasoning is inverted: growing is exactly the unsafe case.
// Going from 64 KB to 128 KB read 64 KB past the end of the old allocation. On
// macOS the over-read landed inside the tagged heap's mapped region and nothing
// ever went wrong; in a Linux container it hit an unmapped page and SIGSEGV'd
// inside ma_decoder__full_decode_and_uninit.
//
// Worth noting how long it hid: ASan runs on every CI push and would have caught
// this instantly, but audio_abi_conformance is excluded from sanitizer builds
// (see tests/CMakeLists.txt — the Rust harness aborts under a dlopen'd
// instrumented library). The one test that exercises this code is the one test
// the sanitizer cannot see.
struct alignas(16) MaBlockHeader {
    uint64_t size;
    uint64_t _pad;
};
static_assert(sizeof(MaBlockHeader) == 16,
              "the prefix must keep the payload 16-byte aligned");

void* maMalloc(size_t sz, void* ud) {
    auto* pv = static_cast<Provider*>(ud);
    void* base = pv->host.alloc(pv->host.userData,
                                (uint64_t)sz + sizeof(MaBlockHeader), 16);
    if (!base) return nullptr;
    static_cast<MaBlockHeader*>(base)->size = (uint64_t)sz;
    return static_cast<char*>(base) + sizeof(MaBlockHeader);
}

void maFreeCb(void* ptr, void* ud) {
    if (!ptr) return;
    auto* pv = static_cast<Provider*>(ud);
    pv->host.free(pv->host.userData,
                  static_cast<char*>(ptr) - sizeof(MaBlockHeader));
}

void* maRealloc(void* ptr, size_t sz, void* ud) {
    if (!ptr)    return maMalloc(sz, ud);
    if (sz == 0) { maFreeCb(ptr, ud); return nullptr; }

    const uint64_t oldSize = reinterpret_cast<const MaBlockHeader*>(
        static_cast<char*>(ptr) - sizeof(MaBlockHeader))->size;

    void* n = maMalloc(sz, ud);
    if (!n) return nullptr;
    // The MINIMUM of the two, which is the only bound that is correct in both
    // directions — and the whole point of tracking the old size.
    std::memcpy(n, ptr, (size_t)(oldSize < (uint64_t)sz ? oldSize : (uint64_t)sz));
    maFreeCb(ptr, ud);
    return n;
}

// ── The real-time callback ──────────────────────────────────────────────────
// This is the audio thread. Nothing here allocates, locks, or calls into the
// engine — including host services, which the ABI forbids here because alloc
// can block and parallelFor waits. The clock read below is deliberately the
// PROVIDER'S OWN std::chrono, not services->nowNs, for exactly that reason;
// getStats pairs the counter with the host clock later, on the game thread.
void onDeviceData(ma_device* dev, void* out, const void* /*in*/, ma_uint32 frameCount) {
    auto* pv = static_cast<Provider*>(dev->pUserData);
    if (!pv) return;

    const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // ACQUIRE FIRST. This pairs with the release store at the end of maCreate
    // and is what makes everything that function published — the engine, the
    // period, the rates — visible on this thread. Reading it up here rather than
    // at the point of use is deliberate: it orders the whole callback body.
    const bool ready = pv->engineInit.load(std::memory_order_acquire);
    const uint64_t period = pv->expectedPeriodNs.load(std::memory_order_relaxed);

    // A missed deadline is an audible click, and it is the only audio failure a
    // player notices instantly. Two periods late means the previous callback
    // did not return in time — count it. First callback has no predecessor.
    //
    // NOT WITHOUT REAL HARDWARE. An overrun means "we missed a HARDWARE
    // deadline". With no hardware there is no deadline — the cadence is whatever
    // the OS scheduler and a dummy device felt like — so a gap wider than two
    // periods measures the runner and nothing else. Counting it failed "no
    // overruns while actually mixing", which is an assertion about our mixer
    // reporting a fact about the machine. The counter stays fully live on real
    // devices, which is the only place it means anything.
    if (!pv->noHardware && pv->lastCallbackNs != 0 && period != 0 &&
        (now - pv->lastCallbackNs) > period * 2) {
        pv->overruns.fetch_add(1, std::memory_order_relaxed);
    }
    pv->lastCallbackNs = now;

    // Because we supplied pDevice, driving the engine's mix is OUR job.
    // (miniaudio's own comment on the pDevice field names a "ma_engine_data_
    // callback" that is not a public symbol — ma_engine_read_pcm_frames is what
    // its internal callback actually calls.)
    if (ready)
        ma_engine_read_pcm_frames(&pv->engine, out, frameCount, nullptr);

    // Published last: a reader seeing this count knows the frames are mixed.
    pv->framesPlayed.fetch_add(frameCount, std::memory_order_release);
}

// ── Pull-streaming bridge: EngineAudioStreamSource -> ma_decoder ────────────
ma_result onStreamRead(ma_decoder* dec, void* out, size_t bytes, size_t* outRead) {
    auto* s = static_cast<Sound*>(dec->pUserData);
    if (!s || !s->src.read) return MA_ERROR;
    const int64_t n = s->src.read(s->src.userData, s->cursor, out, (uint64_t)bytes);
    if (n < 0) return MA_ERROR;
    s->cursor += (uint64_t)n;
    if (outRead) *outRead = (size_t)n;
    return (n == 0) ? MA_AT_END : MA_SUCCESS;
}
ma_result onStreamSeek(ma_decoder* dec, ma_int64 offset, ma_seek_origin origin) {
    auto* s = static_cast<Sound*>(dec->pUserData);
    if (!s) return MA_ERROR;
    int64_t base = 0;
    if (origin == ma_seek_origin_current)   base = (int64_t)s->cursor;
    else if (origin == ma_seek_origin_end)  base = (int64_t)s->src.totalBytes;
    const int64_t target = base + offset;
    if (target < 0) return MA_ERROR;
    // totalBytes == 0 means "unknown" (a live source), so only clamp when the
    // engine actually told us a length.
    if (s->src.totalBytes != 0 && (uint64_t)target > s->src.totalBytes) return MA_ERROR;
    s->cursor = (uint64_t)target;
    return MA_SUCCESS;
}

// ── Slots ───────────────────────────────────────────────────────────────────
Sound* resolveSound(Provider* pv, EngineSoundId id, uint32_t* outIdx) {
    uint32_t i, g;
    if (!splitId(id, kMaxSounds, &i, &g)) return nullptr;
    Sound& s = pv->sounds[i];
    if (!s.inUse || s.generation != g) return nullptr;
    if (outIdx) *outIdx = i;
    return &s;
}
Voice* resolveVoice(Provider* pv, EngineVoiceId id) {
    uint32_t i, g;
    if (!splitId(id, kMaxVoices, &i, &g)) return nullptr;
    Voice& v = pv->voices[i];
    if (!v.inUse || v.generation != g) return nullptr;
    return &v;
}

void releaseVoice(Provider* pv, Voice& v) {
    if (v.soundInit) { ma_sound_uninit(&v.sound); v.soundInit = false; }
    if (v.usesRef)   { ma_audio_buffer_ref_uninit(&v.ref); v.usesRef = false; }
    v.inUse = false;
}

// Voices are reaped when they finish. The engine is never told; it simply finds
// its id stale next time it uses one, which the ABI says is normal.
void cullFinished(Provider* pv) {
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        Voice& v = pv->voices[i];
        if (v.inUse && v.soundInit && ma_sound_at_end(&v.sound)) releaseVoice(pv, v);
    }
}

void releaseSound(Provider* pv, uint32_t index) {
    Sound& s = pv->sounds[index];
    if (!s.inUse) return;
    // Voices referencing it must die first: they hold a data source that is
    // about to be freed, and a mixer reading freed PCM is the worst bug this
    // file could ship.
    for (uint32_t i = 0; i < kMaxVoices; ++i) {
        Voice& v = pv->voices[i];
        if (v.inUse && v.soundIndex == index) releaseVoice(pv, v);
    }
    if (s.decoderInit) { ma_decoder_uninit(&s.decoder); s.decoderInit = false; }
    if (s.pcm)         { ma_free(s.pcm, &pv->maAlloc); s.pcm = nullptr; }
    s.inUse = false;
}

// Shared tail of createSound/createStream: take a slot and register a decoder
// that is already initialised.
EngineAudioResult adoptStreamingDecoder(Provider* pv, Sound& s, uint32_t idx,
                                        EngineSoundId* out) {
    s.streaming   = true;
    s.decoderInit = true;
    s.channels    = pv->channels;
    s.inUse       = true;
    s.generation  = pv->nextSoundGen++;
    *out = makeId(idx, s.generation);
    return ENGINE_AUDIO_OK;
}

int findFreeSound(Provider* pv) {
    for (uint32_t i = 0; i < kMaxSounds; ++i) if (!pv->sounds[i].inUse) return (int)i;
    return -1;
}
int findFreeVoice(Provider* pv) {
    for (uint32_t i = 0; i < kMaxVoices; ++i) if (!pv->voices[i].inUse) return (int)i;
    return -1;
}

} // namespace

// ── Provider entry points ───────────────────────────────────────────────────
// No exception may cross the ABI. Nothing here allocates through C++ or uses a
// throwing facility, so there is nothing to catch — which is a stronger
// guarantee than a try/catch wrapper and the reason this file uses no std
// containers.
extern "C" {

static EngineAudioResult maCreate(const EngineAudioDeviceDesc* desc,
                                  const EngineAudioHostServices* services,
                                  void** outSelf) {
    if (!outSelf) return ENGINE_AUDIO_E_BAD_ARG;
    *outSelf = nullptr;
    if (!services || !services->alloc || !services->free || !services->nowNs)
        return ENGINE_AUDIO_E_BAD_ARG;

    void* block = services->alloc(services->userData, (uint64_t)sizeof(Provider), 16);
    if (!block) return ENGINE_AUDIO_E_OOM;
    auto* pv = new (block) Provider();
    pv->host = *services;   // by VALUE — the struct outlives the pointer

    pv->maAlloc.pUserData = pv;
    pv->maAlloc.onMalloc  = maMalloc;
    pv->maAlloc.onRealloc = maRealloc;
    pv->maAlloc.onFree    = maFreeCb;

    // ── Our device, our callback, our clock ─────────────────────────────────
    ma_device_config dcfg = ma_device_config_init(ma_device_type_playback);
    dcfg.playback.format   = ma_format_f32;   // the engine mixes in f32
    dcfg.playback.channels = desc && desc->channelHint ? desc->channelHint : 0;
    dcfg.sampleRate        = desc && desc->sampleRate  ? desc->sampleRate  : 0;
    if (desc && desc->bufferFrames) dcfg.periodSizeInFrames = desc->bufferFrames;
    dcfg.dataCallback = onDeviceData;
    dcfg.pUserData    = pv;

    // ── The device, and the headless machine that has none ──────────────────
    // A CI runner has no sound card. ALSA fails to even find a config
    // ("snd_func_card_id ... No such file or directory"), ma_device_init
    // returns an error, and this correctly reports E_NO_DEVICE — at which point
    // the conformance suite cannot exercise a single line of the real provider,
    // on every Linux leg, forever. Skipping that leg would leave the ONLY test
    // of the shipping backend permanently unrun; asserting through it would
    // demand hardware no runner has.
    //
    // miniaudio's NULL backend is the answer, and it is a real answer rather
    // than a stub: it runs its own thread, honours the period, advances the
    // clock and mixes into a discarded buffer. Every contract obligation the
    // suite checks — a free-running device clock, measurable overruns, honoured
    // bufferFrames — is exercised for real. What it cannot check is that sound
    // is audible, which no headless test could anyway.
    //
    // OPT-IN, via the environment, and never a silent fallback. A game whose
    // audio device failed must say so and not pretend it is playing: the whole
    // point of E_NO_DEVICE is that the caller gets to decide. CI sets this; a
    // shipped player does not.
    // ENGINE_AUDIO_NO_HARDWARE is the host telling us "there is no dependable
    // audio device here" — which is the truth on every CI runner. It does two
    // things, and they are one idea: allow the null-backend fallback, and stop
    // asserting hardware-timing properties. A shipped game never sets it, so a
    // real device failure still surfaces as E_NO_DEVICE rather than silently
    // pretending to play.
    const char* noHw = std::getenv("ENGINE_AUDIO_NO_HARDWARE");
    pv->noHardware = noHw && *noHw && noHw[0] != '0';

    bool deviceOk = ma_device_init(nullptr, &dcfg, &pv->device) == MA_SUCCESS;
    if (!deviceOk && pv->noHardware) {
        // A FAILED ma_device_init may have partly written the struct; miniaudio
        // expects a zeroed device for a fresh attempt.
        std::memset(&pv->device, 0, sizeof(pv->device));
        const ma_backend nullBackend[] = { ma_backend_null };
        deviceOk = ma_device_init_ex(nullBackend, 1, nullptr, &dcfg,
                                     &pv->device) == MA_SUCCESS;
        if (deviceOk)
            std::fprintf(stderr, "[miniaudio] no device; running on the NULL "
                                 "backend (ENGINE_AUDIO_NO_HARDWARE)\n");
    }
    if (!deviceOk) {
        // The expected outcome on CI and dedicated servers. Not an error to
        // abort on — the engine simply runs silent.
        pv->~Provider();
        services->free(services->userData, block);
        return ENGINE_AUDIO_E_NO_DEVICE;
    }
    pv->deviceInit   = true;
    pv->sampleRate   = pv->device.sampleRate;
    pv->channels     = pv->device.playback.channels;
    pv->bufferFrames = pv->device.playback.internalPeriodSizeInFrames;
    pv->expectedPeriodNs.store(pv->sampleRate
        ? (uint64_t)pv->bufferFrames * 1000000000ull / pv->sampleRate : 0,
        std::memory_order_relaxed);   // ordered by the release store below

    ma_engine_config cfg = ma_engine_config_init();
    cfg.allocationCallbacks = pv->maAlloc;
    cfg.pDevice             = &pv->device;   // engine mixes; WE drive the device
    if (ma_engine_init(&cfg, &pv->engine) != MA_SUCCESS) {
        ma_device_uninit(&pv->device);
        pv->~Provider();
        services->free(services->userData, block);
        return ENGINE_AUDIO_E_NO_DEVICE;
    }
    // RELEASE: everything written above — the engine, the period, the rates —
    // becomes visible to the audio thread that acquires this flag.
    pv->engineInit.store(true, std::memory_order_release);

    // Started only now: the callback dereferences pv->engine, so starting
    // before ma_engine_init would race a half-built engine on the audio thread.
    if (ma_device_start(&pv->device) != MA_SUCCESS) {
        ma_engine_uninit(&pv->engine);
        ma_device_uninit(&pv->device);
        pv->~Provider();
        services->free(services->userData, block);
        return ENGINE_AUDIO_E_NO_DEVICE;
    }

    *outSelf = pv;
    return ENGINE_AUDIO_OK;
}

static void maDestroy(void* p) {
    if (!p) return;
    auto* pv = prov(p);
    // Order matters: stop the device before touching anything the callback
    // reads. Tearing down voices under a running mixer is a use-after-free on
    // the one thread where it is least debuggable.
    if (pv->deviceInit) ma_device_stop(&pv->device);
    for (uint32_t i = 0; i < kMaxVoices; ++i)
        if (pv->voices[i].inUse) releaseVoice(pv, pv->voices[i]);
    for (uint32_t i = 0; i < kMaxSounds; ++i) releaseSound(pv, i);
    if (pv->engineInit.load(std::memory_order_acquire)) {
        ma_engine_uninit(&pv->engine);
        pv->engineInit.store(false, std::memory_order_release);
    }
    if (pv->deviceInit) { ma_device_uninit(&pv->device); pv->deviceInit = false; }
    EngineAudioHostServices host = pv->host;   // copy: pv dies below
    pv->~Provider();
    host.free(host.userData, p);
}

static void maSuspend(void* p, int32_t suspended) {
    auto* pv = prov(p);
    if (!pv || !pv->deviceInit) return;
    // Our device, so we start and stop it — ma_engine_start would reach for the
    // device it did not create.
    if (suspended) ma_device_stop(&pv->device);
    else           ma_device_start(&pv->device);
}

static EngineAudioResult maCreateSound(void* p, const void* bytes, uint64_t count,
                                       uint32_t flags, const char* /*debugName*/,
                                       EngineSoundId* outSound) {
    if (outSound) *outSound = ENGINE_AUDIO_NO_SOUND;
    auto* pv = prov(p);
    if (!pv || !outSound) return ENGINE_AUDIO_E_BAD_ARG;
    if (!bytes || count == 0) return ENGINE_AUDIO_E_BAD_ARG;

    // No container format. Saying UNSUPPORTED rather than trying to parse it is
    // the difference between an absent capability and a broken one.
    if (flags & ENGINE_AUDIO_F_BANK) return ENGINE_AUDIO_E_UNSUPPORTED;

    const int slot = findFreeSound(pv);
    if (slot < 0) return ENGINE_AUDIO_E_OOM;
    Sound& s = pv->sounds[slot];

    ma_decoder_config dcfg = ma_decoder_config_init(ma_format_f32, pv->channels,
                                                    pv->sampleRate);
    dcfg.allocationCallbacks = pv->maAlloc;

    if (flags & ENGINE_AUDIO_F_STREAM) {
        // Decodes on demand straight out of the ENGINE's buffer, which the ABI
        // therefore requires the engine to keep alive until destroySound. This
        // is the case that made STREAM a resource property instead of a
        // play-time flag: the decision has to be made here, not later.
        if (ma_decoder_init_memory(bytes, (size_t)count, &dcfg, &s.decoder) != MA_SUCCESS)
            return ENGINE_AUDIO_E_BAD_DATA;
        s.isPull = false;
        return adoptStreamingDecoder(pv, s, (uint32_t)slot, outSound);
    }

    ma_uint64 frameCount = 0;
    void*     pcm        = nullptr;
    if (ma_decode_memory(bytes, (size_t)count, &dcfg, &frameCount, &pcm) != MA_SUCCESS)
        return ENGINE_AUDIO_E_BAD_DATA;
    if (!pcm || frameCount == 0) {
        if (pcm) ma_free(pcm, &pv->maAlloc);
        return ENGINE_AUDIO_E_BAD_DATA;
    }

    s.streaming  = false;
    s.pcm        = pcm;
    s.frames     = frameCount;
    s.channels   = pv->channels;
    s.inUse      = true;
    s.generation = pv->nextSoundGen++;
    *outSound = makeId((uint32_t)slot, s.generation);
    return ENGINE_AUDIO_OK;
}

static void maDestroySound(void* p, EngineSoundId id) {
    auto* pv = prov(p);
    if (!pv) return;
    uint32_t idx = 0;
    if (!resolveSound(pv, id, &idx)) return;   // unknown id is a no-op
    releaseSound(pv, idx);
}

static EngineAudioResult maCreateStream(void* p, const EngineAudioStreamSource* src,
                                        uint32_t /*flags*/, const char* /*debugName*/,
                                        EngineSoundId* outSound) {
    if (outSound) *outSound = ENGINE_AUDIO_NO_SOUND;
    auto* pv = prov(p);
    if (!pv || !outSound || !src || !src->read) return ENGINE_AUDIO_E_BAD_ARG;

    const int slot = findFreeSound(pv);
    if (slot < 0) return ENGINE_AUDIO_E_OOM;
    Sound& s = pv->sounds[slot];

    // Set up BEFORE ma_decoder_init: it reads the header immediately, through
    // our callbacks, which reach back into these fields.
    s.src    = *src;
    s.cursor = 0;
    s.isPull = true;

    ma_decoder_config dcfg = ma_decoder_config_init(ma_format_f32, pv->channels,
                                                    pv->sampleRate);
    dcfg.allocationCallbacks = pv->maAlloc;
    if (ma_decoder_init(onStreamRead, onStreamSeek, &s, &dcfg, &s.decoder) != MA_SUCCESS) {
        s.src = EngineAudioStreamSource{};
        s.isPull = false;
        return ENGINE_AUDIO_E_BAD_DATA;
    }
    return adoptStreamingDecoder(pv, s, (uint32_t)slot, outSound);
}

static EngineAudioResult maFindSound(void* p, uint64_t nameHash,
                                     const char* /*debugName*/, EngineSoundId* outSound) {
    if (outSound) *outSound = ENGINE_AUDIO_NO_SOUND;
    auto* pv = prov(p);
    if (!pv || !outSound) return ENGINE_AUDIO_E_BAD_ARG;
    if (nameHash == 0) return ENGINE_AUDIO_E_BAD_ARG;   // reserved
    // miniaudio has no bank or event concept. This is exactly the axis on which
    // a Wwise or FMOD adapter would differ, and answering UNSUPPORTED is what
    // lets the engine treat the capability as absent rather than broken.
    return ENGINE_AUDIO_E_UNSUPPORTED;
}

static EngineVoiceId maPlay(void* p, const EngineAudioPlayDesc* desc) {
    auto* pv = prov(p);
    if (!pv || !desc || !pv->engineInit.load(std::memory_order_acquire))
        return ENGINE_AUDIO_NO_VOICE;

    uint32_t soundIdx = 0;
    Sound* s = resolveSound(pv, desc->sound, &soundIdx);
    if (!s) return ENGINE_AUDIO_NO_VOICE;

    cullFinished(pv);
    const int slot = findFreeVoice(pv);
    if (slot < 0) return ENGINE_AUDIO_NO_VOICE;   // voice budget is OUR policy
    Voice& v = pv->voices[slot];

    ma_uint32 sflags = 0;
    if (!(desc->flags & ENGINE_AUDIO_F_SPATIAL)) sflags |= MA_SOUND_FLAG_NO_SPATIALIZATION;

    ma_data_source* source = nullptr;
    if (s->streaming) {
        // One cursor, so one voice. Re-triggering restarts it rather than
        // layering, which is what music and long ambience want anyway.
        for (uint32_t i = 0; i < kMaxVoices; ++i)
            if (pv->voices[i].inUse && pv->voices[i].soundIndex == soundIdx)
                releaseVoice(pv, pv->voices[i]);
        ma_decoder_seek_to_pcm_frame(&s->decoder, 0);
        source = &s->decoder;
    } else {
        if (ma_audio_buffer_ref_init(ma_format_f32, s->channels, s->pcm,
                                     s->frames, &v.ref) != MA_SUCCESS)
            return ENGINE_AUDIO_NO_VOICE;
        v.usesRef = true;
        source = &v.ref;
    }

    if (ma_sound_init_from_data_source(&pv->engine, source, sflags, nullptr,
                                       &v.sound) != MA_SUCCESS) {
        if (v.usesRef) { ma_audio_buffer_ref_uninit(&v.ref); v.usesRef = false; }
        return ENGINE_AUDIO_NO_VOICE;
    }
    v.soundInit  = true;
    v.soundIndex = soundIdx;

    if (desc->flags & ENGINE_AUDIO_F_LOOP) ma_sound_set_looping(&v.sound, MA_TRUE);
    if (desc->flags & ENGINE_AUDIO_F_SPATIAL) {
        ma_sound_set_position(&v.sound, desc->position[0], desc->position[1], desc->position[2]);
        ma_sound_set_velocity(&v.sound, desc->velocity[0], desc->velocity[1], desc->velocity[2]);
    }
    if (desc->volume > 0.0f) ma_sound_set_volume(&v.sound, desc->volume);
    if (desc->pitch  > 0.0f) ma_sound_set_pitch(&v.sound, desc->pitch);

    // Sample-accurate scheduling. miniaudio's start time is on the same global
    // clock getStats() reports as samplesPlayed, so the engine's computed
    // instant maps straight through with no conversion to get wrong.
    if (desc->startSampleTime != 0)
        ma_sound_set_start_time_in_pcm_frames(&v.sound, desc->startSampleTime);

    if (ma_sound_start(&v.sound) != MA_SUCCESS) {
        releaseVoice(pv, v);
        return ENGINE_AUDIO_NO_VOICE;
    }

    v.inUse      = true;
    v.generation = pv->nextVoiceGen++;
    return makeId((uint32_t)slot, v.generation);
}

static void maStop(void* p, EngineVoiceId id, uint32_t fadeMs) {
    auto* pv = prov(p);
    if (!pv) return;
    Voice* v = resolveVoice(pv, id);
    if (!v) return;   // already finished, or never existed: both routine
    if (fadeMs > 0 && v->soundInit) {
        // Let it fade and reap it later; tearing the voice down now is the
        // click the fade was asked for to avoid.
        ma_sound_stop_with_fade_in_milliseconds(&v->sound, fadeMs);
        return;
    }
    releaseVoice(pv, *v);
}

static void maUpdateEmitters(void* p, const EngineAudioEmitterUpdate* ups,
                             uint32_t count, uint32_t stride) {
    auto* pv = prov(p);
    if (!pv) return;
    cullFinished(pv);
    if (!ups || count == 0) return;   // an empty update is normal
    if (stride < sizeof(EngineAudioEmitterUpdate)) return;   // malformed

    const auto* base = reinterpret_cast<const unsigned char*>(ups);
    for (uint32_t i = 0; i < count; ++i) {
        // Walk by the ENGINE's stride, never by sizeof — that is what lets a
        // newer engine add a field without misaligning every row here.
        const auto* u = reinterpret_cast<const EngineAudioEmitterUpdate*>(
            base + (size_t)i * stride);
        Voice* v = resolveVoice(pv, u->voice);
        if (!v || !v->soundInit) continue;   // stale ids are expected input
        if (u->flags & ENGINE_AUDIO_F_SPATIAL) {
            ma_sound_set_position(&v->sound, u->position[0], u->position[1], u->position[2]);
            ma_sound_set_velocity(&v->sound, u->velocity[0], u->velocity[1], u->velocity[2]);
        }
        if (u->volume >= 0.0f) ma_sound_set_volume(&v->sound, u->volume);
        if (u->pitch  >  0.0f) ma_sound_set_pitch(&v->sound, u->pitch);
    }
}

static void maSetListener(void* p, const EngineAudioListener* l) {
    auto* pv = prov(p);
    if (!pv || !l || !pv->engineInit.load(std::memory_order_acquire)) return;
    ma_engine_listener_set_position(&pv->engine, 0, l->position[0], l->position[1], l->position[2]);
    ma_engine_listener_set_velocity(&pv->engine, 0, l->velocity[0], l->velocity[1], l->velocity[2]);
    ma_engine_listener_set_direction(&pv->engine, 0, l->forward[0], l->forward[1], l->forward[2]);
    ma_engine_listener_set_world_up(&pv->engine, 0, l->up[0], l->up[1], l->up[2]);
}

static EngineAudioResult maSetGeometry(void* p, const EngineAcousticGeometry* /*g*/) {
    if (!prov(p)) return ENGINE_AUDIO_E_BAD_ARG;
    // miniaudio does distance attenuation and panning, not propagation. This is
    // the honest answer, and the engine keeps working without occlusion — which
    // is precisely what swapping in a propagation-capable provider would fix
    // without the engine changing at all.
    return ENGINE_AUDIO_E_UNSUPPORTED;
}

static void maSetParam(void* p, uint64_t /*obj*/, uint64_t /*hash*/, float /*v*/) {
    // Nothing provider-specific to expose yet. Swallowing unknown hashes is the
    // contract: the engine never interprets them, so a provider must never
    // assume it recognises one.
    (void)prov(p);
}

static void maGetStats(void* p, EngineAudioStats* out) {
    auto* pv = prov(p);
    if (!pv || !out) return;
    // The ENGINE sets structSize here; write only what fits. This is the one
    // direction frozen layout cannot protect, because the writer is the newer
    // party.
    if (out->structSize < sizeof(EngineAudioStats)) return;

    uint32_t active = 0;
    for (uint32_t i = 0; i < kMaxVoices; ++i) if (pv->voices[i].inUse) ++active;
    out->activeVoices = active;
    out->sampleRate   = pv->sampleRate;
    out->bufferFrames = pv->bufferFrames;
    out->callbackOverruns = pv->overruns.load(std::memory_order_relaxed);
    // Read the pair ADJACENTLY: the engine correlates them to place a future
    // sample on its own timeline, and work between them becomes scheduling
    // error nothing downstream can detect.
    //
    // NOT ma_engine_get_time_in_pcm_frames — that is the node graph's time and
    // stalls whenever nothing is mixing, which makes it useless as a clock.
    // This counter is incremented by the device callback itself.
    out->samplesPlayed = pv->framesPlayed.load(std::memory_order_acquire);
    out->hostTimeNs    = pv->host.nowNs(pv->host.userData);
    out->cpuLoad       = -1.0f;   // unknown
}

static const EngineAudioProviderV1 kTable = {
    ENGINE_AUDIO_PROVIDER_V,
    (uint32_t)sizeof(EngineAudioProviderV1),
    maCreate, maDestroy, maSuspend,
    maCreateSound, maDestroySound,
    maCreateStream, maFindSound,
    maPlay, maStop,
    maUpdateEmitters, maSetListener,
    maSetGeometry, maSetParam, maGetStats,
};

#if defined(_WIN32)
#  define ENGINE_AUDIO_EXPORT __declspec(dllexport)
#else
#  define ENGINE_AUDIO_EXPORT __attribute__((visibility("default")))
#endif

// The one symbol a provider module exports. Static table, never freed.
ENGINE_AUDIO_EXPORT const EngineAudioProviderV1* engineAudioProviderV1(void) {
    return &kTable;
}

} // extern "C"
