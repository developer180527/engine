// ── texture_ownership_test — unloading a SHARED texture ─────────────────────
//
// AssetService::unloadTexture was one line:
//
//     bool AssetService::unloadTexture(TextureHandle h) {
//         return m_textures.removeTexture(h);
//     }
//
// A texture can be shared. m_texCache dedups cooked textures by path and
// refcounts them, so two materials referencing the same .ctex hold ONE handle
// between them — and that line destroyed it on the first unload regardless of
// the count. Three consequences, all silent:
//
//   1. the second material draws from a freed registry slot;
//   2. TextureRegistry recycles slots off a free list with NO generation
//      counter, so that slot soon belongs to an unrelated texture — it renders
//      something plausible rather than failing;
//   3. m_texCache and the async path->handle map both keep naming the dead
//      handle, so a later load or queryTexture() hands it out again.
//
// The same failure unloadMaterial's name-cache invalidation was written to
// prevent, in the one unload that never got it.
//
// THIS TEST AUTHORS A REAL .ctex AND DRIVES THE REAL PATH. An earlier draft of
// a different regression test in this repo asserted a constant against itself
// (BUG-0049); the guard against repeating that here is that every assertion
// below reads state the production code owns — refcounts from the cache the
// service actually uses, handles from the registry it actually writes.
#include <cstdio>
#include <filesystem>
#include <vector>

#include <assetlib/asset_registry.h>
#include <assetlib/texture_asset.h>

#include "animation/clip_registry.h"
#include "animation/skeleton_registry.h"
#include "gpu_test_device.h"
#include "render/asset_registry.h"
#include "render/material_registry.h"
#include "render/texture_registry.h"
#include "runtime/services/asset_service.h"

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

// A 2x2 RGBA8 cooked texture. Smallest thing the real loader accepts, so the
// test exercises the production load path rather than a stand-in for it.
static bool writeCtex(const fs::path& out) {
    assetlib::TextureAsset t;
    t.header.width    = 2;
    t.header.height   = 2;
    t.header.format   = assetlib::kTexRGBA8;
    t.header.mipCount = 1;
    t.pixels.assign(2 * 2 * 4, 0xFF);
    return assetlib::saveTexture(t, out);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("texture_ownership_test: unloading a shared texture\n");

    if (!initTestDevice()) return 1;

    const fs::path root = fs::temp_directory_path() / "engine_tex_ownership_test";
    fs::remove_all(root);
    fs::create_directories(root / ".cache");
    const fs::path ctex = root / ".cache" / "shared.ctex";
    if (!writeCtex(ctex)) {
        std::printf("  FAIL  could not author a .ctex fixture\n");
        return 1;
    }

    AssetRegistry            meshes;
    TextureRegistry          textures;
    MaterialRegistry         materials;
    assetlib::AssetRegistry  assetLib;

    AssetService::Config cfg{ meshes, textures, materials, &assetLib, root };
    AssetService svc(cfg);

    // loadTexture() resolves a relative path against <projectRoot>/.cache, which
    // is where the fixture was written — so this is the same resolution a game
    // performs, not a test-only shortcut.
    const char* kRel = "shared.ctex";

    const std::string key = ctex.lexically_normal().generic_string();

    // ── 1. Two acquisitions of the same path share ONE texture ─────────────
    {
        std::printf("\n-- 1. two materials, one texture --\n");
        const TextureHandle a = svc.loadTexture(kRel);
        const TextureHandle b = svc.loadTexture(kRel);
        CHECK(a.valid() && a.id == b.id,
              "the same path yields the same handle (%u, %u) — dedup, not two "
              "uploads", a.id, b.id);
        CHECK(svc.textureCache().refCount(key) == 2,
              "and the cache counts TWO references (%u)",
              svc.textureCache().refCount(key));
        CHECK(textures.getTexture(a) != nullptr, "the registry holds it");
    }

    // ── 2. ONE unload must not destroy it ──────────────────────────────────
    // The regression. Before the fix this removed the texture from the registry
    // outright, leaving the second holder pointing at a freed slot.
    {
        std::printf("\n-- 2. one unload of two --\n");
        const TextureHandle h = svc.loadTexture(kRel);   // refs -> 3
        svc.unloadTexture(h);                                 // refs -> 2

        CHECK(svc.textureCache().refCount(key) == 2,
              "one unload drops ONE reference (%u), it does not destroy",
              svc.textureCache().refCount(key));
        CHECK(textures.getTexture(h) != nullptr,
              "and the texture is STILL RESIDENT for the other holders — this "
              "is the assertion that fails with the old one-line unload");
    }

    // ── 3. Down to zero: evictable, still not dead ─────────────────────────
    // gpu_resource_cache.h's design note: "refs == 0 MEANS EVICTABLE, NOT
    // DEAD." A cache is not a unique_ptr, and releasing the last reference is
    // not a destroy — it makes the entry a candidate under budget pressure.
    {
        std::printf("\n-- 3. the last reference --\n");
        const TextureHandle h = svc.loadTexture(kRel);
        svc.unloadTexture(h);
        svc.unloadTexture(h);
        svc.unloadTexture(h);

        CHECK(svc.textureCache().refCount(key) == 0,
              "refs reach zero (%u)", svc.textureCache().refCount(key));
        CHECK(textures.getTexture(h) != nullptr,
              "and it is STILL RESIDENT — zero references is evictable, not "
              "dead, so a reload is a cache hit rather than a re-upload");

        // Which means a reload costs nothing and returns the same handle.
        const TextureHandle again = svc.loadTexture(kRel);
        CHECK(again.id == h.id, "reload is a HIT on the retained entry (%u == %u)",
              again.id, h.id);
        CHECK(svc.textureCache().refCount(key) == 1,
              "with the reference count back to one (%u)",
              svc.textureCache().refCount(key));
        svc.unloadTexture(again);
    }

    // ── 4. Over-release is refused, not wrapped ────────────────────────────
    // An unbalanced unload is a caller bug — a kit releasing twice, a scene
    // unloading something it never took. It must be REPORTED, not silently
    // underflowed to four billion, which would pin the texture resident forever
    // and look like a leak with no cause.
    {
        std::printf("\n-- 4. over-release --\n");
        const TextureHandle h = svc.loadTexture(kRel);
        CHECK(svc.unloadTexture(h), "the balanced unload succeeds");
        CHECK(svc.textureCache().refCount(key) == 0, "refs are zero");
        CHECK(!svc.unloadTexture(h),
              "and a SECOND unload returns false rather than underflowing");
        CHECK(svc.textureCache().refCount(key) == 0,
              "the count stays at zero (%u), not UINT32_MAX",
              svc.textureCache().refCount(key));
    }

    // ── 5. A texture the cache does NOT own is removed directly ────────────
    // The async drain and the importers add to the registry without going
    // through the cache, so those have no refcount and a direct removal is the
    // right answer. unloadTexture must handle both populations.
    {
        std::printf("\n-- 5. a non-cached texture --\n");
        static const uint32_t px = 0xFFFFFFFFu;
        gpu::TextureHandle raw = gpu::createTexture2D(
            1, 1, 1, assetlib::kTexRGBA8, gpu::copy(&px, sizeof px));
        CHECK(raw.valid(), "created a texture outside the cache");

        Texture t(raw, 1, 1);
        const TextureHandle h = textures.addTexture(std::move(t));
        CHECK(svc.textureCache().keyFor(h).empty(),
              "the cache does not own it");
        CHECK(svc.unloadTexture(h), "unloadTexture removes it directly");
        CHECK(textures.getTexture(h) == nullptr, "and the slot is freed");
    }

    fs::remove_all(root);

    if (g_failures) {
        std::printf("\ntexture_ownership_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\ntexture_ownership_test: ALL PASS\n");
    return 0;
}
