// ── render_registry_test — the three registries the renderer is built on ─────
//
// `src/render` was the only subsystem at `tier: prototype` that is not
// deliberately contained, with the pipeline's own note saying so: the
// registries were "only exercised incidentally by asset tests". Incidentally is
// not a test. These three hold every mesh, texture and material the renderer
// draws, and they share one design that has already produced a real bug:
//
//   HANDLES ARE BARE SLOT INDICES OVER A FREE LIST, WITH NO GENERATION COUNTER.
//
// So a removed handle does not go invalid — the slot is reused, and the stale
// handle silently starts naming whatever took it. That is exactly how
// loadMaterialAsset's name cache handed back the wrong material (fixed in
// fd08408, and the fix is why AssetService keeps a handle->name reverse map).
// The aliasing itself is asserted here, on purpose: it is the registries'
// documented contract, and anything relying on handles going invalid is wrong.
//
// Headless on bgfx Noop: Texture and Mesh own bgfx handles whose RAII
// destructors call bgfx::destroy, so a registry test cannot run with no bgfx at
// all. Noop needs no GPU.
//
// NOT covered, and worth stating: submission. Culling, sort keys, visibility and
// light packing already live in src/render/world (render_world_test); what is
// left in ForwardPipeline is bgfx binding, and bgfx's Noop backend does not
// report numDraw — its submit() sets timing and zeroes numPrims and never
// touches the draw counter — so draw counts cannot be asserted headlessly.
// Proving submission needs either a real-GPU harness or a counting seam in the
// pipeline, and neither is invented here for a test alone.
#include <cstdio>

#include <bgfx/bgfx.h>

#include "render/asset_registry.h"
#include "render/material_registry.h"
#include "render/texture_registry.h"

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// A 1x1 texture — the cheapest thing bgfx will give a real handle for.
static Texture makeTexture() {
    static const uint32_t px = 0xFFFFFFFFu;
    return Texture(bgfx::createTexture2D(1, 1, false, 1,
                       bgfx::TextureFormat::RGBA8, 0, bgfx::copy(&px, 4)),
                   1, 1);
}

// ── MaterialRegistry — pure, no GPU resources at all ────────────────────────
static void testMaterialRegistry() {
    std::printf("\n-- MaterialRegistry --\n");
    MaterialRegistry reg;

    CHECK(reg.materialCount() == 0, "a fresh registry holds nothing (%zu)",
          reg.materialCount());
    CHECK(reg.getMaterial(MaterialHandle{0}) == nullptr,
          "handle 0 is the invalid sentinel, never a material");

    Material a; a.shaderName = "alpha";
    const MaterialHandle ha = reg.addMaterial(std::move(a));
    CHECK(ha.valid(), "add returns a valid handle (id %u)", ha.id);
    CHECK(ha.id != 0, "...and never id 0, which would read as invalid");
    CHECK(reg.materialCount() == 1, "count is 1, excluding reserved slot 0");
    const Material* got = reg.getMaterial(ha);
    CHECK(got && got->shaderName == "alpha", "get returns what was added");

    // Out of range must be a nullptr, not a read past the end.
    CHECK(reg.getMaterial(MaterialHandle{9999}) == nullptr,
          "an out-of-range handle reads as absent, not out of bounds");

    CHECK(reg.removeMaterial(ha), "remove succeeds");
    CHECK(reg.getMaterial(ha) == nullptr, "...and the handle stops resolving");
    CHECK(reg.materialCount() == 0, "count drops back to 0");
    CHECK(!reg.removeMaterial(ha), "removing twice is refused, not double-freed");

    // THE ALIASING CONTRACT. Not a bug being tolerated — a documented property
    // callers must design around, which is why AssetService keeps a reverse map.
    Material b; b.shaderName = "beta";
    const MaterialHandle hb = reg.addMaterial(std::move(b));
    CHECK(hb.id == ha.id,
          "a freed slot IS reused: the old handle now names the new material");
    const Material* nowB = reg.getMaterial(ha);   // deliberately the OLD handle
    CHECK(nowB && nowB->shaderName == "beta",
          "...so a stale handle silently resolves to the wrong material");

    // clear() must leave a USABLE registry. It empties the vector including the
    // reserved slot 0, so without re-reserving it: count() computes
    // size()-1-free == 0-1-0 and underflows to SIZE_MAX, and the next add hands
    // back id 0 — an INVALID handle for a resource that is really there, making
    // it permanently unreachable. Only reachable today because clear() is
    // called once, at shutdown.
    reg.clear();
    CHECK(reg.materialCount() == 0,
          "after clear, count is 0 and not an underflowed SIZE_MAX (%zu)",
          reg.materialCount());
    Material c; c.shaderName = "gamma";
    const MaterialHandle hc = reg.addMaterial(std::move(c));
    CHECK(hc.valid(), "after clear, add still returns a VALID handle (id %u)",
          hc.id);
    const Material* gotC = reg.getMaterial(hc);
    CHECK(gotC && gotC->shaderName == "gamma",
          "...and the material is reachable through it");
}

// ── TextureRegistry — same shape, plus real bgfx handle lifetime ────────────
static void testTextureRegistry() {
    std::printf("\n-- TextureRegistry --\n");

    // bgfx's live-handle counts come from the API-layer allocators, so they are
    // meaningful on Noop even though draw counters are not. That makes them a
    // LEAK check: create, destroy, and the count must come back.
    // bgfx DEFERS destroy() to frame submission, and getStats() reflects the
    // last SUBMITTED frame — so a live count read straight after a remove still
    // shows the texture. Pump a frame first. (Learned by asserting the naive
    // version and watching it report a leak that was not there.)
    auto liveTextures = [] {
        bgfx::frame();
        const bgfx::Stats* s = bgfx::getStats();
        return s ? s->numTextures : uint16_t(0);
    };
    const uint16_t base = liveTextures();

    TextureRegistry reg;
    CHECK(reg.textureCount() == 0, "a fresh registry holds nothing");
    CHECK(reg.getTexture(TextureHandle{0}) == nullptr, "handle 0 is invalid");

    const TextureHandle ht = reg.addTexture(makeTexture());
    CHECK(ht.valid() && ht.id != 0, "add returns a valid non-zero handle");
    CHECK(reg.textureCount() == 1, "count is 1");
    const Texture* t = reg.getTexture(ht);
    CHECK(t && t->width == 1 && t->height == 1, "get returns the texture");
    CHECK(liveTextures() == uint16_t(base + 1),
          "bgfx sees one more live texture (%u -> %u)", base, liveTextures());

    CHECK(reg.removeTexture(ht), "remove succeeds");
    CHECK(reg.getTexture(ht) == nullptr, "the handle stops resolving");
    CHECK(!reg.removeTexture(ht), "removing twice is refused");
    CHECK(liveTextures() == base,
          "...and the bgfx texture was DESTROYED, not leaked (%u -> %u)",
          base, liveTextures());

    // Slot reuse, same contract as materials.
    const TextureHandle h2 = reg.addTexture(makeTexture());
    CHECK(h2.id == ht.id, "a freed slot is reused");
    CHECK(reg.getTexture(ht) != nullptr,
          "...so the stale handle resolves to the NEW texture");

    reg.clear();
    CHECK(reg.textureCount() == 0, "after clear, count is 0 not SIZE_MAX (%zu)",
          reg.textureCount());
    CHECK(liveTextures() == base, "clear destroyed the GPU textures too");
    const TextureHandle h3 = reg.addTexture(makeTexture());
    CHECK(h3.valid(), "after clear, add returns a VALID handle (id %u)", h3.id);
    CHECK(reg.getTexture(h3) != nullptr, "...and it resolves");
    reg.clear();
    CHECK(liveTextures() == base, "no textures leaked across the whole test");
}

// ── AssetRegistry (meshes) — same contract ──────────────────────────────────
static void testMeshRegistry() {
    std::printf("\n-- AssetRegistry (meshes) --\n");
    AssetRegistry reg;

    CHECK(reg.meshCount() == 0, "a fresh registry holds nothing");
    CHECK(reg.getMesh(MeshHandle{0}) == nullptr, "handle 0 is invalid");
    CHECK(reg.getMesh(MeshHandle{9999}) == nullptr,
          "an out-of-range handle reads as absent");
    CHECK(!reg.removeMesh(MeshHandle{0}), "removing the invalid handle is refused");

    reg.clear();
    CHECK(reg.meshCount() == 0, "after clear, count is 0 not SIZE_MAX (%zu)",
          reg.meshCount());
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("render_registry_test: mesh/texture/material registry contracts\n");

    bgfx::renderFrame();                    // single-threaded, as the engine does
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    init.resolution.width = 16; init.resolution.height = 16;
    if (!bgfx::init(init)) { std::printf("FAIL bgfx init\n"); return 1; }
    // getStats() reflects the last SUBMITTED frame, so one frame must go through
    // before the handle counts mean anything.
    bgfx::frame();

    testMaterialRegistry();
    testTextureRegistry();
    testMeshRegistry();

    bgfx::frame();
    bgfx::shutdown();

    if (g_failures) {
        std::printf("\nrender_registry_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nrender_registry_test: all checks passed\n");
    return 0;
}
