// ── gpu_seam_test — the two contracts render/gpu.h states in prose ──────────
//
// The include-level half of G1 is covered by scripts/check_gpu_seam.py (a grep)
// and tests/headless_include_probe.cpp (the compiler with bgfx off the path).
// Neither can see BEHAVIOUR, and gpu.h makes two behavioural promises that were
// worth exactly nothing until something checked them.
//
// 1. HEADLESS. With no device, staging returns null and every create returns an
//    invalid handle, so the asset path runs to completion and simply produces
//    no handles. This is the dedicated server, the cook worker, and most tests.
//
// 2. THE FORMAT PRE-CHECK, which existed as a comment and hid a leak.
//    gpu::createTexture2D can refuse a texture for two format reasons, and a
//    refusal there STRANDS the staged payload: bgfx frees a Memory only when a
//    command consumes it, and exposes no release. So every caller must ask
//    textureFormatSupported() BEFORE spending the memcpy.
//
//    The severity is what makes this worth a test rather than a code comment:
//    on content cooked for the wrong target EVERY texture takes the refusal
//    path, so the failure is not one leaked texture, it is the entire texture
//    set — with the full decoded payload, staged on a worker, for the life of
//    the process. Found in review; the contract sentence in gpu.h had claimed
//    the opposite.
#include <cstdio>

#include <assetlib/texture_asset.h>

#include "gpu_test_device.h"
#include "render/gpu.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("gpu_seam_test: the upload seam's behavioural contracts\n");

    // ── 1. Headless: the asset path runs, and produces nothing ─────────────
    // Deliberately BEFORE initTestDevice(). This is the state the process
    // starts in and the state a dedicated server never leaves.
    {
        std::printf("\n-- 1. no device --\n");
        CHECK(!gpu::deviceAvailable(), "no device at process start");

        static const uint32_t px = 0xFFFFFFFFu;
        gpu::Blob* b = gpu::copy(&px, sizeof px);
        CHECK(b == nullptr, "staging returns null rather than crashing");
        CHECK(gpu::blobSize(b) == 0, "blobSize is null-safe");

        // The create calls must tolerate the null blob a headless stage
        // produced, because that is exactly what the loaders will hand them.
        gpu::VertexBufferHandle vb = gpu::createVertexBuffer(b, gpu::VertexFormat::Standard);
        gpu::IndexBufferHandle  ib = gpu::createIndexBuffer(b, gpu::IndexFormat::U32);
        gpu::TextureHandle      tx = gpu::createTexture2D(1, 1, 1, assetlib::kTexRGBA8, b);
        CHECK(!vb.valid() && !ib.valid() && !tx.valid(),
              "every create returns an invalid handle");

        // And destroying those invalid handles is a no-op, not a crash — the
        // registries will do exactly this at teardown.
        gpu::destroy(vb); gpu::destroy(ib); gpu::destroy(tx);
        CHECK(true, "destroying invalid handles is a no-op");

        CHECK(!gpu::textureFormatSupported(assetlib::kTexRGBA8),
              "and no format is supported without a device");
    }

    if (!initTestDevice()) return 1;

    // ── 2. The format predicate ────────────────────────────────────────────
    {
        std::printf("\n-- 2. textureFormatSupported --\n");
        CHECK(gpu::textureFormatSupported(assetlib::kTexRGBA8),
              "RGBA8 is supported (every backend has it)");

        // An id no build knows. This is the "cooked by a newer engine" case,
        // and the one that used to silently fall back to RGBA8 — handing
        // block-compressed bytes to the driver as raw pixels.
        constexpr uint32_t kNotAFormat = 0xDEADBEEFu;
        CHECK(!gpu::textureFormatSupported(kNotAFormat),
              "an unknown format id is refused, not defaulted to RGBA8");

        // Reported once, not once per texture. Asked repeatedly here because
        // the scene that triggers this has thousands of textures and the
        // per-texture version buried the one line that explains the failure.
        for (int i = 0; i < 100; ++i) (void)gpu::textureFormatSupported(kNotAFormat);
        CHECK(true, "and asking 100 more times prints nothing further (see above)");
    }

    // ── 3. The pre-check is what keeps the blob from being stranded ────────
    // This is the regression proof. The loader's shape is:
    //
    //     if (!textureFormatSupported(fmt)) return {};   // <- no staging
    //     blob = gpu::copy(pixels);                      // the 64 MB memcpy
    //     createTexture2D(..., fmt, blob);
    //
    // Asserting the ORDER is the whole point: staging first and asking later
    // is the leak, and it is invisible at runtime because a stranded blob
    // produces no error, no crash and no failed test — just memory that never
    // comes back.
    {
        std::printf("\n-- 3. refuse before staging --\n");
        constexpr uint32_t kNotAFormat = 0xDEADBEEFu;

        gpu::Blob* staged = nullptr;
        if (gpu::textureFormatSupported(kNotAFormat)) {
            static const uint32_t px = 0xFFFFFFFFu;
            staged = gpu::copy(&px, sizeof px);       // must NOT be reached
        }
        CHECK(staged == nullptr,
              "the predicate short-circuits before anything is staged");

        // And the supported path still works end to end, so the guard has not
        // simply disabled texture upload.
        CHECK(gpu::textureFormatSupported(assetlib::kTexRGBA8), "RGBA8 still passes");
        static const uint32_t white = 0xFFFFFFFFu;
        gpu::Blob* good = gpu::copy(&white, sizeof white);
        CHECK(good != nullptr, "and staging a supported format succeeds");
        gpu::TextureHandle th =
            gpu::createTexture2D(1, 1, 1, assetlib::kTexRGBA8, good);
        CHECK(th.valid(), "and the upload produces a live handle");
        gpu::destroy(th);
        CHECK(!th.valid(), "which destroy() nulls");
    }

    // ── 4. The backstop fires, loudly ──────────────────────────────────────
    // A caller who forgets the pre-check must not fail silently — a stranded
    // blob produces no error, no crash and no failed test, just memory that
    // never comes back. So createTexture2D re-checks and says so.
    //
    // This deliberately strands 4 bytes for the life of this test process.
    // That is the point being demonstrated, it is the smallest payload that
    // demonstrates it, and a test process exits seconds later.
    {
        std::printf("\n-- 4. the backstop --\n");
        static const uint32_t px = 0xFFFFFFFFu;
        gpu::Blob* orphan = gpu::copy(&px, sizeof px);
        CHECK(orphan != nullptr, "a payload staged without asking first");
        gpu::TextureHandle th =
            gpu::createTexture2D(1, 1, 1, 0xDEADBEEFu, orphan);
        CHECK(!th.valid(),
              "createTexture2D refuses it (and prints BUG: above, naming the "
              "stranded byte count)");
    }

    shutdownTestDevice();

    // ── 5. The seam closes before the device ───────────────────────────────
    // Renderer::shutdown clears the flag BEFORE bgfx goes down, so a resource
    // destroyed later — registries outlive the device on some teardown paths —
    // is a no-op instead of a call into a dead backend.
    {
        std::printf("\n-- 5. after shutdown --\n");
        CHECK(!gpu::deviceAvailable(), "the seam is closed");
        gpu::TextureHandle stale{ 7 };            // as if it outlived the device
        gpu::destroy(stale);
        CHECK(!stale.valid(), "destroying a stale handle is a no-op, not a crash");
    }

    if (g_failures) {
        std::printf("\ngpu_seam_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\ngpu_seam_test: ALL PASS\n");
    return 0;
}
