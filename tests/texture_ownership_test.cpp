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
#include <assetlib/mesh_asset.h>
#include <assetlib/texture_asset.h>

#include "animation/clip_registry.h"
#include "animation/skeleton_registry.h"
#include "gpu_test_device.h"
#include "render/asset_registry.h"
#include "render/material_registry.h"
#include "render/texture_registry.h"
#include "render/vertex.h"
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

// A cooked mesh whose ONE embedded material names `texRel` as its base colour.
// The geometry is a single degenerate triangle — this fixture exists to carry a
// material reference, not to be drawn.
static bool writeCookedMesh(const fs::path& out, const char* texRel) {
    assetlib::MeshAsset m;
    m.header.vertexStride = sizeof(Vertex);
    m.header.vertexCount  = 3;
    m.header.indexCount   = 3;
    m.header.indexStride  = 4;
    m.header.submeshCount = 1;
    m.header.materialCount = 1;
    m.vertexData.assign(3 * sizeof(Vertex), 0);
    const uint32_t idx[3] = {0, 1, 2};
    m.indexData.assign(reinterpret_cast<const uint8_t*>(idx),
                       reinterpret_cast<const uint8_t*>(idx) + sizeof idx);

    assetlib::MeshSubmesh sub;
    sub.indexOffset = 0; sub.indexCount = 3; sub.materialIndex = 0;
    m.submeshes.push_back(sub);

    assetlib::CookedMaterial cm;
    cm.flags = assetlib::kMatFlag_HasBaseColor;
    std::snprintf(cm.baseColorPath, sizeof cm.baseColorPath, "%s", texRel);
    m.materials.push_back(cm);

    return assetlib::saveMesh(m, out);
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

    // ── 6. Eviction: the payoff, and the thing it must never do ────────────
    // Wiring evictOverBudget was blocked on the refcounts being right — the
    // ctor's note said so, and BUG-0051 is why they were not. Now they are, so
    // the budget can be enforced. The property that makes it SAFE is not that
    // it frees memory; it is that it refuses to free the wrong thing.
    {
        std::printf("\n-- 6. eviction --\n");

        // A second distinct texture, so the cache has something to choose
        // between and "LRU picks the older one" is a real assertion.
        const fs::path other = root / ".cache" / "other.ctex";
        if (!writeCtex(other)) { std::printf("  FAIL  fixture\n"); return 1; }
        const std::string otherKey = other.lexically_normal().generic_string();

        const TextureHandle held = svc.loadTexture(kRel);        // refs 1, KEPT
        const TextureHandle loose = svc.loadTexture("other.ctex");
        svc.unloadTexture(loose);                                 // refs 0

        CHECK(svc.textureCache().refCount(key) == 1 &&
              svc.textureCache().refCount(otherKey) == 0,
              "one texture referenced, one at zero");

        // A budget of 1 byte: everything unreferenced must go, and everything
        // referenced must stay. The extreme value is the point — it removes any
        // doubt that the survivor survived for the right reason.
        svc.setTextureBudget(1);
        CHECK(svc.textureBudget() == 1, "budget set through the service");

        const size_t freed = svc.evictTexturesOverBudget();
        CHECK(freed > 0, "eviction freed %zu bytes", freed);

        CHECK(textures.getTexture(held) != nullptr,
              "THE REFERENCED TEXTURE SURVIVED — exceeding a budget is "
              "recoverable, evicting something a draw still points at is not");
        CHECK(svc.textureCache().refCount(key) == 1,
              "and it kept its reference (%u)", svc.textureCache().refCount(key));
        CHECK(!svc.textureCache().contains(otherKey),
              "the unreferenced one is gone from the cache");
        CHECK(textures.getTexture(loose) == nullptr,
              "and its registry slot is freed — the destroyer ran");

        // Reload after eviction is a MISS that re-uploads, not a hit on a dead
        // entry. This is the half that the stale-map bug would have broken.
        const TextureHandle back = svc.loadTexture("other.ctex");
        CHECK(back.valid(), "and it reloads cleanly afterwards");
        CHECK(textures.getTexture(back) != nullptr, "into a live registry slot");

        svc.setTextureBudget(0);   // unbounded again
        CHECK(svc.evictTexturesOverBudget() == 0,
              "with no budget, eviction is a no-op");
    }

    // ── 7. A COOKED MESH owning a texture, then unloaded ───────────────────
    // The path a game actually uses, and the one the earlier sections did not
    // reach: nothing here calls loadTexture directly. loadMesh resolves the
    // mesh's material textures and ACQUIRES a reference for each; unloadMesh
    // released none of them, so a scene's textures sat at refs > 0 forever.
    //
    // That was invisible until texture eviction was wired — at which point the
    // cache correctly refused to evict them and the budget reclaimed nothing on
    // the normal path. A feature that works only for direct loadTexture callers
    // is a feature no game benefits from.
    {
        std::printf("\n-- 7. a cooked mesh owns its material textures --\n");

        const fs::path meshTex = root / ".cache" / "meshtex.ctex";
        const fs::path cooked  = root / ".cache" / "withmat.cooked";
        if (!writeCtex(meshTex) || !writeCookedMesh(cooked, "meshtex.ctex")) {
            std::printf("  FAIL  could not author the mesh fixture\n");
            return 1;
        }
        const std::string meshTexKey =
            meshTex.lexically_normal().generic_string();

        CHECK(svc.textureCache().refCount(meshTexKey) == 0,
              "the texture starts unreferenced");

        const MeshHandle mh = svc.loadMesh("withmat.cooked");
        CHECK(mh.valid(), "the cooked mesh loads");
        CHECK(svc.textureCache().refCount(meshTexKey) == 1,
              "and loading it ACQUIRED its material texture (%u)",
              svc.textureCache().refCount(meshTexKey));

        svc.unloadMesh(mh);
        CHECK(svc.textureCache().refCount(meshTexKey) == 0,
              "unloading the mesh RELEASES it again (%u) — without this the "
              "reference is permanent and eviction can never reclaim the "
              "texture, which is every texture a game loads",
              svc.textureCache().refCount(meshTexKey));

        // And now it is actually evictable, which is the whole point.
        svc.setTextureBudget(1);
        svc.evictTexturesOverBudget();
        CHECK(!svc.textureCache().contains(meshTexKey),
              "so a budget can now reclaim it");
        svc.setTextureBudget(0);
    }

    fs::remove_all(root);

    if (g_failures) {
        std::printf("\ntexture_ownership_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\ntexture_ownership_test: ALL PASS\n");
    return 0;
}
