// ── render_pipeline_test — SUBMISSION, headless ──────────────────────────────
//
// The one thing `src/render` had no test for. `render_world_test` covers culling,
// sort keys and light packing; `render_registry_test` covers the three
// registries; what was left uncovered was the part that actually talks to bgfx —
// and it is the part that has been changed every week (instancing, the draw
// ceiling, the shadow cull).
//
// It was untestable for a concrete reason, now fixed: bgfx's Noop backend never
// reports draws. Its submit() writes timing, zeroes numPrims, and never touches
// the draw counter, so `bgfx::getStats()->numDraw` is 0 no matter what the
// pipeline does. rdiag::SubmitStats counts on OUR side of the API, which is what
// makes every assertion below possible with no GPU.
//
// TWO SEAMS make this a unit test rather than an engine boot:
//   * RenderView is a plain struct of spans — camera, lights, items. No ECS, no
//     Renderer, no world. The test builds one by hand, so it can state exactly
//     what the pipeline is given.
//   * RenderContext is three registry references. The test owns the registries.
//
// WHAT THIS CANNOT CHECK, stated so nobody reads a pass as more than it is: no
// pixels. Noop executes nothing, so "the shadow map is sampled with the right
// matrix" and "the instanced shader reads i_data0..3" are outside its reach —
// those still need a real device. What it does check is the submission DECISIONS:
// how many draws, which path, whether the invariants hold. Every bug found in
// this code so far has been a decision bug.
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <bgfx/bgfx.h>
#include <bx/math.h>

#include "render/forward_pipeline.h"
#include "render/render_context.h"
#include "render/asset_registry.h"
#include "render/material_registry.h"
#include "render/texture_registry.h"
#include "render/world/frustum.h"   // extractFrustumPlanes
#include "render/world/cull_stream.h" // the cull reads SoA streams, not items

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// ── Fixture ─────────────────────────────────────────────────────────────────
// One unit cube's worth of buffers, reused by every item. Noop hands out real
// handles for these; it just never draws with them.
struct Fixture {
    AssetRegistry    assets;
    TextureRegistry  textures;
    MaterialRegistry materials;
    bgfx::VertexLayout layout;
    bgfx::VertexBufferHandle vbh = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  ibh = BGFX_INVALID_HANDLE;
    bgfx::ViewId viewCursor = 5;
    // unique_ptr, NOT vector<Mesh>: RenderItem holds a raw Mesh*, so the
    // addresses must not move. A plain vector reallocating mid-test dangles
    // every item built earlier — which is exactly how the first run of this
    // file died, inside bgfx::setVertexBuffer on a moved-from handle.
    std::vector<std::unique_ptr<Mesh>> meshes;

    Fixture() {
        layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
        static const float verts[3 * 3] = { 0,0,0, 1,0,0, 0,1,0 };
        static const uint16_t idx[3]    = { 0, 1, 2 };
        vbh = bgfx::createVertexBuffer(bgfx::makeRef(verts, sizeof(verts)), layout);
        ibh = bgfx::createIndexBuffer(bgfx::makeRef(idx, sizeof(idx)));
    }
    ~Fixture() {
        // The meshes SHARE the fixture's vbh/ibh, so each must forget them
        // before its destructor runs — Mesh::destroy() owns what it holds, and
        // eight Meshes destroying one buffer is a double free.
        for (auto& m : meshes) { m->vbh = BGFX_INVALID_HANDLE;
                                 m->ibh = BGFX_INVALID_HANDLE; }
        meshes.clear();
        if (bgfx::isValid(ibh)) bgfx::destroy(ibh);
        if (bgfx::isValid(vbh)) bgfx::destroy(vbh);
    }

    // Batching keys are registry SLOT INDICES in production — small integers.
    // This started out using the Mesh POINTER as the key, which is distinct per
    // mesh and therefore looked fine, but blows the sort key's 21-bit mesh field;
    // the debug assert added for issues.md A1.6 caught it immediately. Keys here
    // are now small and sequential, like the real ones.
    uint32_t nextMeshKey = 1;

    // A mesh with unit bounds centred on the origin, so an item's model matrix
    // alone decides whether it is inside the frustum.
    Mesh* makeMesh(MaterialHandle mat, bool doubleSided = false) {
        meshes.push_back(std::make_unique<Mesh>());
        ++nextMeshKey;
        Mesh& m = *meshes.back();
        m.vbh = vbh; m.ibh = ibh; m.indexCount = 3;
        m.material = mat; m.doubleSided = doubleSided;
        m.boundsMin = { -0.5f, -0.5f, -0.5f };
        m.boundsMax = {  0.5f,  0.5f,  0.5f };
        return &m;
    }

    MaterialHandle makeMaterial() {
        Material mat;                 // fixed path: no shaderName, not dataDriven
        return materials.addMaterial(std::move(mat));
    }

    RenderContext context() {
        RenderContext rc{ assets, textures, materials };
        rc.viewCursor   = &viewCursor;
        rc.shadowViewId = 0;
        return rc;
    }
};

// The pipeline's cull reads the SoA streams (CullStreams), which extraction fills.
// A hand-built RenderView must do the same, through the shared helper so this test
// covers the production rule and not a copy of it.
static rworld::CullStreamStore g_stream;
static void setViewItems(RenderView& rv, const RenderItem* p, std::size_t n) {
    rv.items = { p, n };
    rworld::fillCullStream(rv.items, g_stream);
    rv.cull = g_stream.view();
}

// A camera at +Z looking down -Z, matching the engine's convention.
static void buildCamera(RenderView& rv, float aspect = 1.0f) {
    const bx::Vec3 eye{ 0.0f, 0.0f, 20.0f }, at{ 0.0f, 0.0f, 0.0f },
                   up{ 0.0f, 1.0f, 0.0f };
    bx::mtxLookAt(rv.view.m, eye, at, up);
    bx::mtxProj(rv.proj.m, 60.0f, aspect, 0.1f, 500.0f,
                bgfx::getCaps()->homogeneousDepth);
    rv.camPos = { eye.x, eye.y, eye.z, 1.0f };
    float vp[16];
    bx::mtxMul(vp, rv.view.m, rv.proj.m);
    rworld::extractFrustumPlanes(vp, rv.frustum);
    rv.target.fb = BGFX_INVALID_HANDLE;   // backbuffer
    rv.target.w = 64; rv.target.h = 64;
    rv.baseViewId = 1;
    rv.ambient = 0.25f;
}

static RenderItem itemAt(Mesh* mesh, uint32_t meshKey, MaterialHandle mat,
                        float x, float y, float z) {
    RenderItem it;
    bx::mtxTranslate(it.model.m, x, y, z);
    it.mesh     = mesh;
    it.material = mat;
    it.meshKey  = meshKey;                    // batching keys off mesh+material
    it.matKey   = mat.id;
    it.hasBounds    = mesh->hasBounds();
    it.boundsCenter = mesh->boundsCenter();
    it.boundsSize   = mesh->boundsSize();
    return it;
}

// ── 1. Nothing in, nothing out ───────────────────────────────────────────────
static void testEmptyView(Fixture& fx, ForwardPipeline& pipe) {
    std::printf("\n-- an empty view submits nothing --\n");
    RenderView rv; buildCamera(rv);
    RenderContext rc = fx.context();
    pipe.render(rv, rc);
    const auto& s = pipe.submitStats();
    CHECK(s.itemsConsidered == 0 && s.draws == 0,
          "0 items considered, 0 draws (%u, %u)", s.itemsConsidered, s.draws);
    CHECK(s.drawsDropped == 0, "and nothing was dropped");
}

// ── 2. The cull is wired to the pipeline, not just unit-tested in rworld ─────
static void testCulling(Fixture& fx, ForwardPipeline& pipe) {
    std::printf("\n-- items behind the camera are culled --\n");
    MaterialHandle mat = fx.makeMaterial();
    Mesh* mesh = fx.makeMesh(mat);

    std::vector<RenderItem> items;
    for (int i = 0; i < 10; ++i)                      // in front, visible
        items.push_back(itemAt(mesh, fx.nextMeshKey, mat, (float)(i - 5), 0.0f, 0.0f));
    for (int i = 0; i < 7; ++i)                       // far BEHIND the camera
        items.push_back(itemAt(mesh, fx.nextMeshKey, mat, 0.0f, 0.0f, 400.0f));

    RenderView rv; buildCamera(rv);
    setViewItems(rv, items.data(), items.size());
    RenderContext rc = fx.context();
    pipe.render(rv, rc);
    const auto& s = pipe.submitStats();
    CHECK(s.itemsConsidered == 17, "17 items considered (%u)", s.itemsConsidered);
    CHECK(s.itemsCulled == 7, "the 7 behind the camera are culled (%u)",
          s.itemsCulled);
    CHECK(s.draws > 0, "the visible 10 still produced draws (%u)", s.draws);
}

// ── 3. Batching and instancing ──────────────────────────────────────────────
// batchRuns is the PREDICTION (draws instancing could remove); instancedItems is
// what it actually removed. Asserting both together is what stops the two
// drifting apart — a prediction nobody checks is how R5 sat open for weeks.
static void testInstancedRun(Fixture& fx, ForwardPipeline& pipe) {
    std::printf("\n-- one mesh + one material = one batch run --\n");
    MaterialHandle mat = fx.makeMaterial();
    Mesh* mesh = fx.makeMesh(mat);

    std::vector<RenderItem> items;
    for (int i = 0; i < 64; ++i)
        items.push_back(itemAt(mesh, fx.nextMeshKey, mat, (float)(i % 8) - 4.0f,
                               (float)(i / 8) - 4.0f, 0.0f));

    RenderView rv; buildCamera(rv);
    setViewItems(rv, items.data(), items.size());
    RenderContext rc = fx.context();
    pipe.render(rv, rc);
    const auto& s = pipe.submitStats();

    CHECK(s.itemsConsidered == 64 && s.itemsCulled == 0,
          "all 64 visible (%u considered, %u culled)", s.itemsConsidered,
          s.itemsCulled);
    CHECK(s.batchRuns == 1,
          "64 identical draws collapse to ONE batch run (%u)", s.batchRuns);

    const bool instancing =
        0 != (bgfx::getCaps()->supported & BGFX_CAPS_INSTANCING);
    if (instancing) {
        CHECK(s.instancedDraws == 1, "submitted as 1 instanced draw (%u)",
              s.instancedDraws);
        CHECK(s.instancedItems == 64, "covering all 64 items (%u)",
              s.instancedItems);
        CHECK(s.draws == 1, "so the whole batch is ONE submit (%u)", s.draws);
    } else {
        // Recorded rather than skipped: which branch ran is itself information.
        std::printf("  note  backend reports no BGFX_CAPS_INSTANCING — "
                    "asserting the per-draw fallback instead\n");
        CHECK(s.draws == 64, "fallback submits one draw per item (%u)", s.draws);
        CHECK(s.instancedDraws == 0, "and nothing claims to be instanced");
    }
    CHECK(s.materialBinds >= 1, "the material was bound at least once (%u)",
          s.materialBinds);
}

// ── 4. Distinct materials cannot batch ──────────────────────────────────────
// The negative case for the above. If this ever reports one run, the sort key
// stopped separating materials and instancing would render with the wrong one.
static void testDistinctMaterialsDoNotBatch(Fixture& fx, ForwardPipeline& pipe) {
    std::printf("\n-- distinct materials do NOT share a batch run --\n");
    std::vector<RenderItem> items;
    std::vector<MaterialHandle> mats;
    for (int i = 0; i < 8; ++i) {
        mats.push_back(fx.makeMaterial());
        Mesh* mesh = fx.makeMesh(mats.back());
        items.push_back(itemAt(mesh, fx.nextMeshKey, mats.back(), (float)(i - 4), 0.0f, 0.0f));
    }
    RenderView rv; buildCamera(rv);
    setViewItems(rv, items.data(), items.size());
    RenderContext rc = fx.context();
    pipe.render(rv, rc);
    const auto& s = pipe.submitStats();
    CHECK(s.batchRuns == 8, "8 materials give 8 runs, not 1 (%u)", s.batchRuns);
    CHECK(s.draws == 8, "and 8 separate submits (%u)", s.draws);
    CHECK(s.instancedDraws == 0, "none of them instanced");
}

// ── 5. The draw ceiling refuses draws instead of corrupting memory ──────────
// This is the guard on the crash that cost a day: bgfx's Metal backend commits
// per-draw uniforms into a fixed 8 MB buffer WITH NO BOUNDS CHECK, so ~8192
// draws in a frame segfaults inside the backend. The pipeline caps itself and
// reports the shortfall. The cap must hold on every backend, not just Metal —
// a test that only ran on Metal would not have caught the ordering bug where
// the counter was bumped after the submit.
static void testDrawCeiling(Fixture& fx, ForwardPipeline& pipe) {
    std::printf("\n-- the draw ceiling is enforced, and reported --\n");
    // Non-batchable on purpose: distinct materials, so nothing can be collapsed
    // into an instanced submit and the item count IS the draw demand.
    const int kItems = 6000;
    std::vector<RenderItem> items;
    std::vector<MaterialHandle> mats;
    items.reserve(kItems);
    for (int i = 0; i < kItems; ++i) {
        mats.push_back(fx.makeMaterial());
        Mesh* mesh = fx.makeMesh(mats.back());
        // A tight grid well inside the frustum: all of them must survive the cull,
        // or the test would be measuring culling instead of the ceiling.
        items.push_back(itemAt(mesh, fx.nextMeshKey, mats.back(),
                              (float)((i % 60) - 30) * 0.1f,
                              (float)((i / 60) % 60 - 30) * 0.1f, 0.0f));
    }
    RenderView rv; buildCamera(rv);
    setViewItems(rv, items.data(), items.size());
    RenderContext rc = fx.context();
    pipe.render(rv, rc);
    const auto& s = pipe.submitStats();

    CHECK(s.draws <= 4096, "draws never exceed kMaxDrawsPerFrame (%u)", s.draws);
    CHECK(s.drawsDropped > 0,
          "the refusal is REPORTED, not silent (%u dropped)", s.drawsDropped);
    CHECK(s.draws + s.drawsDropped >= (uint32_t)(kItems - s.itemsCulled),
          "submitted + dropped accounts for every visible item "
          "(%u + %u vs %u visible)", s.draws, s.drawsDropped,
          s.itemsConsidered - s.itemsCulled);
}

// ── 6. The R4 invariant, as a test rather than a printed warning ────────────
// A bone palette belongs to the skinned MESH: one upload per item, however many
// submeshes it draws. Uploading per submesh re-sent 4.7 KB of identical matrices
// per range per frame. submit_stats.h states the invariant; this enforces it.
static void testBonePalettePerItem(Fixture& fx, ForwardPipeline& pipe) {
    std::printf("\n-- one bone-palette upload per skinned ITEM, not per draw --\n");
    MaterialHandle mat = fx.makeMaterial();
    Mesh* mesh = fx.makeMesh(mat);
    // Four submesh ranges over the same buffers → four draws for one item.
    for (int i = 0; i < 4; ++i)
        mesh->submeshes.push_back(SubmeshRange{ 0, 3, mat });

    static float bones[16 * 4] = {};
    for (int b = 0; b < 4; ++b) bx::mtxIdentity(bones + b * 16);

    RenderItem it = itemAt(mesh, fx.nextMeshKey, mat, 0.0f, 0.0f, 0.0f);
    it.boneMatrices = bones;
    it.boneCount    = 4;

    RenderView rv; buildCamera(rv);
    setViewItems(rv, &it, 1);
    RenderContext rc = fx.context();
    pipe.render(rv, rc);
    const auto& s = pipe.submitStats();

    CHECK(s.skinnedItems == 1, "1 skinned item (%u)", s.skinnedItems);
    CHECK(s.submeshDraws == 4, "drawn as 4 submesh ranges (%u)", s.submeshDraws);
    CHECK(s.bonePaletteUploads == 1,
          "but only ONE palette upload (%u) — not one per draw",
          s.bonePaletteUploads);
}

// ── 7. The shadow pass culls against the LIGHT, and counts separately ───────
static void testShadowPassCulls(Fixture& fx, ForwardPipeline& pipe) {
    std::printf("\n-- a shadow caster drives a separate, culled shadow pass --\n");
    MaterialHandle mat = fx.makeMaterial();
    Mesh* mesh = fx.makeMesh(mat);

    std::vector<RenderItem> items;
    for (int i = 0; i < 32; ++i)
        items.push_back(itemAt(mesh, fx.nextMeshKey, mat, (float)(i % 8) - 4.0f, 0.0f,
                               (float)(i / 8) - 2.0f));

    LightItem sun;
    sun.type        = LightType::Directional;
    sun.color       = { 1.0f, 1.0f, 1.0f };
    sun.intensity   = 3.0f;
    sun.direction   = bx::normalize(bx::Vec3{ 0.3f, -1.0f, 0.2f });
    sun.position    = { 0.0f, 50.0f, 0.0f };
    sun.castShadows = true;

    RenderView rv; buildCamera(rv);
    setViewItems(rv, items.data(), items.size());
    rv.lights = { &sun, 1 };
    RenderContext rc = fx.context();
    pipe.render(rv, rc);
    const auto& s = pipe.submitStats();

    CHECK(s.shadowItemsConsidered == 32,
          "the shadow pass considered all 32 items (%u)",
          s.shadowItemsConsidered);
    CHECK(s.shadowDraws > 0, "and submitted shadow draws (%u)", s.shadowDraws);
    CHECK(s.shadowBonePaletteUploads == 0,
          "no skinned casters, so no palettes uploaded");

    // The light frustum is not the camera frustum: with no caster, there is no
    // shadow pass at all. That difference is the whole point of counting the
    // shadow pass separately.
    LightItem noShadow = sun; noShadow.castShadows = false;
    rv.lights = { &noShadow, 1 };
    pipe.render(rv, rc);
    CHECK(pipe.submitStats().shadowDraws == 0,
          "with castShadows off, the shadow pass submits nothing (%u)",
          pipe.submitStats().shadowDraws);
    CHECK(pipe.submitStats().draws > 0,
          "while the colour pass still draws (%u)", pipe.submitStats().draws);
}

// ── 8. Stats are per view, not cumulative ──────────────────────────────────
// Every assertion above depends on this. If render() ever forgot its reset, the
// counters would climb across frames and every threshold check would pass by
// accident.
static void testStatsResetPerView(Fixture& fx, ForwardPipeline& pipe) {
    std::printf("\n-- submit stats reset per view --\n");
    MaterialHandle mat = fx.makeMaterial();
    Mesh* mesh = fx.makeMesh(mat);
    RenderItem it = itemAt(mesh, fx.nextMeshKey, mat, 0.0f, 0.0f, 0.0f);
    RenderView rv; buildCamera(rv);
    setViewItems(rv, &it, 1);
    RenderContext rc = fx.context();

    pipe.render(rv, rc);
    const uint32_t first = pipe.submitStats().draws;
    pipe.render(rv, rc);
    const uint32_t second = pipe.submitStats().draws;
    CHECK(first == second && first > 0,
          "two identical views report identical counts, not a sum (%u, %u)",
          first, second);
    CHECK(pipe.submitStats().itemsConsidered == 1,
          "itemsConsidered is 1, not 2 (%u)",
          pipe.submitStats().itemsConsidered);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("render_pipeline_test: ForwardPipeline submission, headless\n");

    bgfx::renderFrame();                      // single-threaded, as the engine does
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    init.resolution.width = 64; init.resolution.height = 64;
    if (!bgfx::init(init)) { std::printf("FAIL bgfx init\n"); return 1; }
    bgfx::frame();

    {
        Fixture fx;
        ForwardPipeline pipe;
        RenderContext rc = fx.context();
        pipe.onAttach(rc);

        testEmptyView(fx, pipe);
        testCulling(fx, pipe);
        testInstancedRun(fx, pipe);
        testDistinctMaterialsDoNotBatch(fx, pipe);
        testDrawCeiling(fx, pipe);
        testBonePalettePerItem(fx, pipe);
        testShadowPassCulls(fx, pipe);
        testStatsResetPerView(fx, pipe);

        pipe.onDetach();
    }

    bgfx::frame();
    bgfx::shutdown();

    if (g_failures) {
        std::printf("\nrender_pipeline_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nrender_pipeline_test: all checks passed\n");
    return 0;
}
