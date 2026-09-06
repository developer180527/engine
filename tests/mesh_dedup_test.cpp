// ── mesh_dedup_test — one cooked mesh, one pair of GPU buffers ───────────────
//
// Pins the fix for a bug that silently dropped 92% of a scene.
//
// AssetService::loadMesh used to create a NEW vertex+index buffer per CALL. Two
// entities referencing the same cooked mesh each got their own pair, and bgfx's
// pool is BGFX_CONFIG_MAX_INDEX_BUFFERS = 4096: a 50 000-entity scene drawing 176
// distinct cooked meshes loaded 4 089 of them and then failed 45 911 times with
// "bgfx buffer creation failed". Nothing crashed. The scene just rendered 8% of
// itself, and the error text blamed the files rather than the handle pool.
//
// Textures already had this dedup (renderer audit R1 — "every load of the same
// cooked texture uploaded ANOTHER copy to the GPU"). Meshes never got it, and every
// stress scene in the repo hid the gap by using engine://primitive/cube: one shared
// mesh, so one buffer pair no matter how many entities point at it.
//
// THE LOAD COUNT HERE IS DELIBERATELY 5 000 — past bgfx's 4 096 — so this test
// reproduces the original failure rather than merely checking a handle is reused. A
// version that loaded twice would pass against a broken implementation.
//
// Real cooked meshes (MeshCooker on a tiny OBJ), real AssetService, bgfx Noop, no
// project assets required. bgfx's live-handle counts come from the API-layer
// allocators, so they are meaningful on Noop even though draw counters are not.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <bgfx/bgfx.h>
#include "gpu_test_device.h"

#include "runtime/services/asset_service.h"
#include "assets/cookers/mesh/mesh_cooker.h"
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"

namespace fs = std::filesystem;
namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// getStats() reflects the last SUBMITTED frame and bgfx defers destroy() to frame
// submission, so a count read without pumping a frame is stale. (Learned the hard
// way in render_registry_test, which reported a leak that did not exist.)
struct Buffers { uint16_t vb, ib; };
static Buffers liveBuffers() {
    bgfx::frame();
    const bgfx::Stats* s = bgfx::getStats();
    return s ? Buffers{ s->numVertexBuffers, s->numIndexBuffers }
             : Buffers{ 0, 0 };
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("mesh_dedup_test: one cooked mesh must cost one buffer pair\n");

    if (!initTestDevice()) return 1;

    // Two distinct cooked meshes: the same bytes under two names, which is exactly
    // what a kit of props looks like to the loader.
    const fs::path root  = fs::temp_directory_path() / "engine_mesh_dedup_test";
    const fs::path cache = root / ".cache" / "meshs";
    fs::remove_all(root);
    fs::create_directories(cache);
    {
        const fs::path obj = root / "tri.obj";
        { std::ofstream f(obj); f << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"; }
        MeshCooker cooker;
        assetlib::CookContext ctx;
        ctx.sourcePath = obj;
        ctx.outputPath = cache / "shared.cooked";
        if (!cooker.cook(ctx).success) { std::printf("FAIL cook\n"); return 1; }
        fs::copy_file(cache / "shared.cooked", cache / "other.cooked");
        // A third name, for the unload/slot-recycle case below.
        fs::copy_file(cache / "shared.cooked", cache / "other2.cooked");
    }

    {
        AssetRegistry    meshes;
        TextureRegistry  textures;
        MaterialRegistry materials;
        AssetService svc({meshes, textures, materials});
        svc.setProjectRoot(root);

        const Buffers before = liveBuffers();

        // ── 1. The regression itself ────────────────────────────────────────
        // 5 000 loads of ONE path. Pre-fix this exhausted bgfx at ~4 096 and the
        // rest returned invalid handles.
        const int kLoads = 5000;
        MeshHandle first = svc.loadMesh("meshs/shared.cooked");
        CHECK(first.valid(), "the first load succeeds (handle %u)", first.id);

        int sameHandle = 0, invalid = 0;
        for (int i = 1; i < kLoads; ++i) {
            MeshHandle h = svc.loadMesh("meshs/shared.cooked");
            if (!h.valid())      ++invalid;
            else if (h == first) ++sameHandle;
        }
        CHECK(invalid == 0,
              "all %d loads of one path succeed — none hit the buffer pool "
              "(%d invalid)", kLoads, invalid);
        CHECK(sameHandle == kLoads - 1,
              "and every one returns the SAME handle (%d of %d)",
              sameHandle, kLoads - 1);

        const Buffers afterShared = liveBuffers();
        CHECK(afterShared.vb == before.vb + 1 && afterShared.ib == before.ib + 1,
              "%d loads created exactly ONE buffer pair (vb %u->%u, ib %u->%u)",
              kLoads, before.vb, afterShared.vb, before.ib, afterShared.ib);
        CHECK(meshes.meshCount() == 1,
              "and ONE mesh in the registry, not %d (%zu)", kLoads,
              meshes.meshCount());

        // ── 2. The negative case ────────────────────────────────────────────
        // Dedup must be BY PATH. Without this, an implementation that always
        // returned the first handle would pass everything above.
        MeshHandle other = svc.loadMesh("meshs/other.cooked");
        CHECK(other.valid() && other != first,
              "a DIFFERENT cooked path gets its own handle (%u vs %u)",
              other.id, first.id);
        const Buffers afterOther = liveBuffers();
        CHECK(afterOther.vb == afterShared.vb + 1
              && afterOther.ib == afterShared.ib + 1,
              "...and its own buffer pair (vb %u->%u)", afterShared.vb,
              afterOther.vb);
        CHECK(meshes.meshCount() == 2, "two meshes now (%zu)", meshes.meshCount());

        // ── 3. A missing path must not poison the cache ─────────────────────
        // A failed load must stay failed-but-harmless: no handle, no buffers, and
        // it must not make a later good load return the failure.
        MeshHandle missing = svc.loadMesh("meshs/does_not_exist.cooked");
        CHECK(!missing.valid(), "a missing cooked file yields an invalid handle");
        const Buffers afterMissing = liveBuffers();
        CHECK(afterMissing.vb == afterOther.vb && afterMissing.ib == afterOther.ib,
              "and allocates no buffers (vb %u, ib %u)", afterMissing.vb,
              afterMissing.ib);
        CHECK(svc.loadMesh("meshs/shared.cooked") == first,
              "a good path still resolves after a failed one");

        // ── 4. UNLOADING must not leave the cache holding a recycled slot ───
        // AssetRegistry::removeMesh pushes the slot onto a free list and the next
        // addMesh pops it. The dedup map keys a path to a HANDLE, and
        // Handle::valid() is just `id != 0` — it never asks the registry whether
        // the slot is still that mesh. So an entry left behind after an unload is
        // not merely stale: once anything else loads into that slot,
        // `loadMesh(samePath)` hits the cache, believes the handle, and returns A
        // DIFFERENT MESH. Wrong geometry, no crash, nothing in the log.
        //
        // The sequence below is exactly that, and it is reachable from
        // SceneService::unloadScene and from Lua's assets.unloadMesh.
        // ── ONE unload is no longer enough, and that is BUG-0052's fix ──────
        // This section used to call unloadMesh ONCE and expect the slot freed.
        // That expectation WAS the defect: loadMesh dedups by path, so every
        // load above handed another caller the same handle, and one unload
        // destroying it for all of them is what BUG-0052 records.
        // MeshResidency::refs now counts load/unload PAIRS, so a real
        // destruction needs every reference given back — and the recycled-slot
        // hazard this section exists for still needs a real destruction to set
        // it up, so the references are drained rather than the assertion
        // weakened.
        //
        // kLoads + 1, and the +1 is not slack: section 3's
        // `CHECK(svc.loadMesh(...) == first)` above is itself a load and takes a
        // reference like any other. Spelled out rather than looped-until-null so
        // that adding a load without adding an unload FAILS here instead of
        // being absorbed.
        const int kSharedLoads = kLoads + 1;
        const uint32_t recycled = first.id;
        CHECK(svc.unloadMesh(first),
              "one unload of a %d-times-shared mesh succeeds and does NOT "
              "destroy it (BUG-0052)", kSharedLoads);
        CHECK(meshes.getMesh(first) != nullptr,
              "...the mesh is still resident for the other %d holders",
              kSharedLoads - 1);
        for (int i = 1; i < kSharedLoads; ++i) svc.unloadMesh(first);
        CHECK(meshes.getMesh(first) == nullptr,
              "and only the LAST of %d unloads destroys it", kSharedLoads);
        MeshHandle taken = svc.loadMesh("meshs/other2.cooked");
        CHECK(taken.valid(), "another mesh loads afterwards (handle %u)", taken.id);
        CHECK(taken.id == recycled,
              "...and lands in the freed slot %u, which is what makes a stale "
              "cache entry dangerous rather than merely wrong", recycled);

        MeshHandle reloaded = svc.loadMesh("meshs/shared.cooked");
        CHECK(reloaded.valid(), "the unloaded path reloads (handle %u)", reloaded.id);
        CHECK(reloaded != taken,
              "and does NOT hand back the mesh now occupying its old slot "
              "(reloaded %u vs the other mesh's %u)", reloaded.id, taken.id);
        const Mesh* rm = meshes.getMesh(reloaded);
        CHECK(rm && rm->sourcePath.find("shared.cooked") != std::string::npos,
              "the reloaded handle really is shared.cooked (%s)",
              rm ? rm->sourcePath.c_str() : "null");
    }

    // The registries went out of scope; their destructors release the meshes,
    // which bgfx frees at frame submission.
    const Buffers end = liveBuffers();
    std::printf("  note  buffers after teardown: vb %u ib %u\n", end.vb, end.ib);

    bgfx::frame();
    shutdownTestDevice();
    fs::remove_all(fs::temp_directory_path() / "engine_mesh_dedup_test");

    if (g_failures) {
        std::printf("\nmesh_dedup_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nmesh_dedup_test: all checks passed\n");
    return 0;
}
