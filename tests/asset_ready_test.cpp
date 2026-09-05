// ── asset_ready_test — failed loads must be VISIBLE (audit H.6) ─────────────
// A failed async load used to vanish: no handle, no longer in flight —
// indistinguishable from "never requested". SceneService::isSceneReady's
// "no handle AND still loading" check then fell through and reported scenes
// containing assets that will never appear as ready. This gauntlet proves
// the failure-tracking primitive: loadFailed() distinguishes "will never
// load" from "still loading", and a fresh request clears it (retry).
// Headless on bgfx Noop. Exits non-zero on first failure.
#include <chrono>
#include <cstdio>
#include <thread>

#include "gpu_test_device.h"

#include "runtime/services/asset_service.h"
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// Drain until the in-flight queue empties (worker settles), then flush.
static void settle(AssetService& svc, int budgetMs = 10000) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count() < budgetMs) {
        while (svc.drainUploads()) {}
        if (svc.pendingCount() == 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("asset_ready_test: failed-load visibility gauntlet\n");

    if (!initTestDevice()) return 1;

    {
        AssetRegistry    meshes;
        TextureRegistry  textures;
        MaterialRegistry materials;
        AssetService svc({meshes, textures, materials});

        const char* doomed = "meshs/no-such-asset.cooked";

        // ── 1. A failed load becomes VISIBLE, not invisible ──────────────
        svc.loadMeshAsync(doomed);
        settle(svc);
        CHECK(svc.queryMesh(doomed) == 0,  "failed load yields no handle");
        CHECK(!svc.isLoading(doomed),      "failed load is not 'loading'");
        CHECK(svc.loadFailed(doomed),      "failed load IS 'failed' — the H.6 signal");

        // Pre-fix state check spelled out: the exact predicate isSceneReady
        // used ("no handle AND still loading" -> not ready) is FALSE here,
        // so pre-fix pollers judged this asset ready. loadFailed() is the
        // signal that breaks that lie.
        CHECK(!(svc.queryMesh(doomed) == 0 && svc.isLoading(doomed)),
              "old ready-predicate falls through on failure (the lie)");

        // ── 2. Retry semantics: a fresh request clears the failure ───────
        svc.loadMeshAsync(doomed);
        // Immediately after re-request the failure flag must be gone
        // (either in flight again or re-failed later — never stale).
        bool clearedOrInFlight = !svc.loadFailed(doomed) || svc.isLoading(doomed);
        CHECK(clearedOrInFlight, "fresh request clears the stale failure");
        settle(svc);
        CHECK(svc.loadFailed(doomed), "re-failed load is visible again");

        // ── 3. Unknown paths are not 'failed' ─────────────────────────────
        CHECK(!svc.loadFailed("never/requested.cooked"),
              "never-requested path is not 'failed'");
    }

    shutdownTestDevice();

    if (g_failures) { std::printf("asset_ready_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("asset_ready_test: ALL PASS\n");
    return 0;
}
