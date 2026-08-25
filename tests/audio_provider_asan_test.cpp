// ── audio_provider_asan_test — the test that would have caught the over-read ──
//
// `maRealloc` in the miniaudio provider read past the end of every block it
// grew, on every sound decode, for as long as the provider has existed. ASan
// runs on every CI push and never saw it. TWO reasons, and this file exists to
// remove both:
//
//   1. `audio_abi_conformance` — the ONLY test that drives createSound — is
//      excluded from sanitizer builds (`NOT ENGINE_SANITIZE` in
//      tests/CMakeLists.txt, because the Rust harness aborts under a dlopen'd
//      instrumented library). The one test that exercises the code was the one
//      test the sanitizer could not see.
//
//   2. Even had it run, the engine's host allocator hands out slices of a large
//      TLSF mmap. An over-read of 64 KB inside a 2 MB mapped block touches
//      nothing ASan objects to and nothing the OS objects to. THAT is why the
//      bug needed a Linux container with a different heap layout to show itself
//      as a SIGSEGV, months after it was written.
//
// So this test is C++ (no Rust harness, so no sanitizer exclusion) and it
// deliberately backs host `alloc` with plain `malloc`. Under ASan every block
// then carries redzones, and reading past one is a reported error rather than a
// coin flip against the heap layout. Under a normal build it still exercises the
// decode path and asserts the allocator's own invariants.
//
// WHAT IT ASSERTS, beyond "did not crash":
//   * every pointer the provider frees was one it was given (no prefix/offset
//     mismatch, which is the failure mode a size-prefixed allocator introduces)
//   * allocations and frees BALANCE across the sound's lifetime
//   * the decode actually grew a block, i.e. maRealloc was really exercised —
//     otherwise this whole file could pass while testing nothing
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <engine/engine_audio_provider.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// ── A malloc-backed host allocator, on purpose ──────────────────────────────
// The engine's real allocator is TLSF over a few large mappings, which is what
// made the over-read invisible. This one gives ASan something to guard.
namespace host {

struct Counters {
    // ── EVERY FIELD BELOW IS TOUCHED FROM MORE THAN ONE THREAD ───────────────
    // The provider is explicitly allowed to allocate off the game thread — the
    // ABI hands it parallelFor and tells it to decode and refill streams there —
    // so a host allocator that keeps unsynchronised bookkeeping is the HOST's
    // bug, not the provider's. This one kept a std::map and plain counters, and
    // TSan reported races inside libc++'s red-black tree the first time the
    // suite ran under the thread sanitizer: __tree_remove, __find_equal,
    // __insert_node_at. A corrupted tree would eventually have been read as
    // "the provider freed a pointer we never handed out" — the test accusing the
    // code it was written to vindicate.
    //
    // One mutex over the whole record, not atomics per counter: `live`, `peak`
    // and `blocks` have to move TOGETHER or the peak is nonsense, and this is a
    // test fixture where contention costs nothing.
    std::mutex m;
    uint64_t allocs = 0, frees = 0, bytes = 0, peak = 0, live = 0;
    // Sizes of every live block, so a free of an address we never handed out —
    // or of an interior pointer — is caught here rather than by the allocator
    // going quietly wrong later.
    std::map<void*, uint64_t> blocks;
    uint64_t badFrees = 0;
    // The largest block ever seen. If the decode never grows anything, this
    // test is not exercising realloc and says so.
    uint64_t largest = 0;
};

void* alloc(void* ud, uint64_t bytes, uint64_t alignment) {
    auto* c = static_cast<Counters*>(ud);
    if (bytes == 0) return nullptr;
    // Over-allocate and hand back an aligned interior pointer, storing the base
    // so free() can recover it. aligned_alloc is avoided deliberately: its size
    // must be a multiple of the alignment, which miniaudio's sizes are not, and
    // getting that wrong is undefined rather than merely wasteful.
    const uint64_t extra = alignment + sizeof(void*);
    void* base = std::malloc((size_t)(bytes + extra));
    if (!base) return nullptr;
    auto raw = reinterpret_cast<uintptr_t>(base) + sizeof(void*);
    auto aligned = (raw + (alignment - 1)) & ~(uintptr_t)(alignment - 1);
    std::memcpy(reinterpret_cast<void*>(aligned - sizeof(void*)), &base,
                sizeof(void*));
    void* p = reinterpret_cast<void*>(aligned);

    // The malloc above is deliberately OUTSIDE the lock: it is thread-safe on
    // its own, and holding our mutex across it would serialise the very
    // parallel decode this test exists to exercise.
    {
        std::lock_guard<std::mutex> lk(c->m);
        ++c->allocs;
        c->bytes += bytes;
        c->live  += bytes;
        if (c->live > c->peak) c->peak = c->live;
        if (bytes > c->largest) c->largest = bytes;
        c->blocks[p] = bytes;
    }
    return p;
}

void freeFn(void* ud, void* p) {
    auto* c = static_cast<Counters*>(ud);
    if (!p) return;
    {
        std::lock_guard<std::mutex> lk(c->m);
        auto it = c->blocks.find(p);
        if (it == c->blocks.end()) {
            // The provider handed back something we never gave it. With a
            // size-prefixed allocator inside the provider, an off-by-one on the
            // prefix arithmetic lands exactly here.
            ++c->badFrees;
            return;
        }
        c->live -= it->second;
        c->blocks.erase(it);
        ++c->frees;
    }
    void* base = nullptr;
    std::memcpy(&base, reinterpret_cast<char*>(p) - sizeof(void*), sizeof(void*));
    std::free(base);
}

void parallelFor(void*, const char*, uint32_t count, uint32_t,
                 void (*fn)(void*, uint32_t, uint32_t), void* ctx) {
    if (fn && count) fn(ctx, 0, count);   // inline; this test is not about jobs
}
uint32_t workerCount(void*) { return 1; }
uint64_t nowNs(void*) {
    // Monotonic enough for a decode test; the clock's contract is the Rust
    // suite's business.
    static uint64_t t = 0;
    t += 1000000;
    return t;
}

} // namespace host

// ── A WAV big enough to make the decoder grow its buffer several times ──────
// miniaudio's full-decode path reallocs as it goes, so the point is SIZE, not
// content: three seconds of 48 kHz stereo PCM16 is ~1.1 MB, which grows the
// output buffer repeatedly. A short beep would decode inside the first
// allocation and prove nothing.
static std::vector<uint8_t> makeWav(uint32_t seconds = 3,
                                   uint32_t rate = 48000,
                                   uint16_t channels = 2) {
    const uint32_t frames = rate * seconds;
    const uint32_t dataBytes = frames * channels * 2;
    std::vector<uint8_t> w;
    w.reserve(44 + dataBytes);

    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) w.push_back((uint8_t)(v >> (8 * i))); };
    auto u16 = [&](uint16_t v) { for (int i = 0; i < 2; ++i) w.push_back((uint8_t)(v >> (8 * i))); };
    auto tag = [&](const char* s) { for (int i = 0; i < 4; ++i) w.push_back((uint8_t)s[i]); };

    tag("RIFF"); u32(36 + dataBytes); tag("WAVE");
    tag("fmt "); u32(16); u16(1); u16(channels); u32(rate);
    u32(rate * channels * 2); u16((uint16_t)(channels * 2)); u16(16);
    tag("data"); u32(dataBytes);

    // A ramp rather than silence: an encoder or decoder that skips zero runs
    // would shrink the work and weaken the test.
    for (uint32_t f = 0; f < frames; ++f)
        for (uint16_t c = 0; c < channels; ++c) {
            const int16_t s = (int16_t)((f * 37 + c * 991) % 32000 - 16000);
            u16((uint16_t)s);
        }
    return w;
}

// ── Loading the module ──────────────────────────────────────────────────────
static const EngineAudioProviderV1* loadProvider(const char* path) {
#if defined(_WIN32)
    HMODULE h = LoadLibraryA(path);
    if (!h) { std::printf("LoadLibrary failed (%lu)\n", GetLastError()); return nullptr; }
    auto sym = (const EngineAudioProviderV1* (*)())
        GetProcAddress(h, ENGINE_AUDIO_PROVIDER_ENTRY);
#else
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { std::printf("dlopen failed: %s\n", dlerror()); return nullptr; }
    auto sym = (const EngineAudioProviderV1* (*)())
        dlsym(h, ENGINE_AUDIO_PROVIDER_ENTRY);
#endif
    // Deliberately never closed: the table's function pointers live as long as
    // this process uses them.
    if (!sym) { std::printf("module exports no %s\n", ENGINE_AUDIO_PROVIDER_ENTRY); return nullptr; }
    return sym();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("audio_provider_asan_test\n");

#if !defined(ENGINE_AUDIO_MODULE_PATH)
    std::printf("ENGINE_AUDIO_MODULE_PATH not defined — nothing to test.\n");
    return 1;
#else
    // No CI runner has dependable audio hardware; see maCreate. Set before
    // create() so the provider may fall back to miniaudio's null backend and
    // stops asserting hardware timing.
#if defined(_WIN32)
    _putenv_s("ENGINE_AUDIO_NO_HARDWARE", "1");
#else
    setenv("ENGINE_AUDIO_NO_HARDWARE", "1", 1);
#endif

    const EngineAudioProviderV1* p = loadProvider(ENGINE_AUDIO_MODULE_PATH);
    CHECK(p != nullptr, "the provider module loads (%s)", ENGINE_AUDIO_MODULE_PATH);
    if (!p) return 1;
    CHECK(p->structSize >= sizeof(EngineAudioProviderV1) ||
          p->structSize > 0, "the table reports a size (%u)", p->structSize);

    host::Counters counters;
    EngineAudioHostServices services{};
    services.structSize  = (uint32_t)sizeof(EngineAudioHostServices);
    services.userData    = &counters;
    services.alloc       = host::alloc;
    services.free        = host::freeFn;
    services.parallelFor = host::parallelFor;
    services.workerCount = host::workerCount;
    services.nowNs       = host::nowNs;

    EngineAudioDeviceDesc desc{};
    desc.structSize   = (uint32_t)sizeof(EngineAudioDeviceDesc);
    desc.sampleRate   = 48000;
    desc.bufferFrames = 512;

    void* self = nullptr;
    const EngineAudioResult rc = p->create(&desc, &services, &self);
    if (rc == ENGINE_AUDIO_E_NO_DEVICE) {
        // Correct behaviour on a machine with nothing to play through, and NOT
        // this test's subject. Loud, because a permanently skipped test is how
        // the original bug survived.
        std::printf("  SKIP  create() reported E_NO_DEVICE — no audio device and "
                    "no null-backend fallback on this host. The decode path was "
                    "NOT exercised.\n");
        return 0;
    }
    CHECK(rc == ENGINE_AUDIO_OK && self != nullptr,
          "create() succeeded (rc %d)", rc);
    if (rc != ENGINE_AUDIO_OK || !self) return 1;

    // ── The decode. This is the line the over-read died on. ─────────────────
    const std::vector<uint8_t> wav = makeWav();
    const uint64_t allocsBefore = counters.allocs;

    EngineSoundId sound = 0;
    const EngineAudioResult src = p->createSound(
        self, wav.data(), (uint64_t)wav.size(), 0, "asan_probe", &sound);
    CHECK(src == ENGINE_AUDIO_OK && sound != 0,
          "createSound decoded %zu bytes of WAV (rc %d, id %llu)",
          wav.size(), src, (unsigned long long)sound);

    // If the decode did not GROW an allocation, maRealloc never ran and this
    // file is decoration. ~1.1 MB of PCM cannot land in one small block.
    CHECK(counters.allocs > allocsBefore,
          "the decode allocated through host services (%llu calls)",
          (unsigned long long)(counters.allocs - allocsBefore));
    CHECK(counters.largest >= 256 * 1024,
          "and grew a block to at least 256 KB (largest %llu B) — proof the "
          "realloc path was actually exercised",
          (unsigned long long)counters.largest);

    // The invariant a size-prefixed allocator can break: every pointer freed
    // must be one we handed out. An off-by-one on the prefix arithmetic in the
    // provider surfaces here and nowhere else.
    CHECK(counters.badFrees == 0,
          "every pointer the provider freed was one it was given (%llu bad)",
          (unsigned long long)counters.badFrees);

    if (sound) p->destroySound(self, sound);
    p->destroy(self);

    CHECK(counters.badFrees == 0,
          "still no foreign frees after teardown (%llu)",
          (unsigned long long)counters.badFrees);
    // Everything the provider took, it gave back. A leak here is a leak in a
    // shipping game's audio, and the tagged-heap telemetry would show it as
    // "audio grows forever" with no line number.
    CHECK(counters.live == 0 && counters.blocks.empty(),
          "every host allocation was returned (%llu B live, %zu blocks, "
          "%llu allocs / %llu frees)",
          (unsigned long long)counters.live, counters.blocks.size(),
          (unsigned long long)counters.allocs, (unsigned long long)counters.frees);

    std::printf("\npeak host usage: %.2f MB across %llu allocations\n",
                counters.peak / (1024.0 * 1024.0),
                (unsigned long long)counters.allocs);

    if (g_failures) {
        std::printf("\naudio_provider_asan_test: FAIL — %d\n", g_failures);
        return 1;
    }
    std::printf("\naudio_provider_asan_test: PASS\n");
    return 0;
#endif
}
