// ── render_scene_test — the retained scene's table, ids and lifetime (WO-019) ──
//
// P3a of the retained scene (docs/plans/renderer-program.md §9): one row per
// drawable object, a stable id per row, and the LIVE -> RETIRED -> FREE rule
// that keeps a slot the GPU may still read from being reused. Hermetic: the
// table is GPU-free and runtime-free (render/world/), so this needs no device,
// no world and no clock: frames are counters.
//
//   1. ids: a stale id is DETECTED, never aliased (the generation bump)
//   2. the fence: a retired slot is not reused until its frame has completed
//   3. apply(): wholesale writes create, update, retire, and re-home a row
//      whose object gained or lost a skin
//   4. diff(): agrees after apply, and names the entity and column when the
//      retained table has drifted from a rebuild
#include <cstdio>
#include <string>
#include <vector>

#include "render/world/render_scene.h"

using namespace rworld;

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

static RowData row(float x, uint32_t mesh = 1, bool skinned = false) {
    RowData r;
    r.model.m[12] = x;
    r.sphere = {x, 0.0f, 0.0f, 1.0f};
    r.keyBase = ((uint64_t)mesh << 32) | 7u;
    r.mesh.id = mesh;
    r.material.id = 7;
    r.flags = kRowHasBounds | (skinned ? kRowSkinned : 0);
    if (skinned) r.palette = 3;
    return r;
}
static CapturedRow cap(uint64_t owner, float x, uint32_t mesh = 1, bool skinned = false) {
    return {owner, row(x, mesh, skinned)};
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("render_scene_test — retained scene table, ids, lifetime\n");

    // ── 1. A stale id is detected, never aliased ──────────────────────────
    std::printf("1. ids and generations\n");
    {
        RenderTable t;
        const RenderObjectId a = t.create(100, row(1));
        CHECK(a.valid() && t.alive(a) && t.owner(a) == 100, "create gives a LIVE id owned by its entity");
        CHECK(t.destroy(a, /*frame*/ 1), "destroy");
        CHECK(!t.alive(a), "the id is dead the moment it retires, not a fence later");
        CHECK(!t.update(a, row(9)), "update through the dead id is refused");
        t.collect(1);   // fenced: the slot is FREE now
        const RenderObjectId b = t.create(200, row(2));
        CHECK(b.index == a.index, "the freed slot is reused (index %u)", b.index);
        CHECK(b != a, "but under a NEW generation (%u vs %u): the old id cannot alias the new row",
              b.generation, a.generation);
        CHECK(!t.alive(a) && !t.update(a, row(9)), "the stale id still reads as dead after reuse");
        RowData r; t.read(b, r);
        CHECK(t.owner(b) == 200 && r.model.m[12] == 2.0f, "and the new row was not touched by it");
        CHECK(!RenderObjectId{}.valid() && !t.alive(RenderObjectId{}), "the default id is invalid");
        NullRenderScene null;
        CHECK(!null.create(1, row(1)).valid() && !null.update({}, row(1)) && !null.destroy({}, 0),
              "the null scene: invalid id, updates and destroys are no-ops");
    }

    // ── 2. A retired slot waits for its fence ─────────────────────────────
    std::printf("2. LIVE -> RETIRED -> FREE, by frame fence\n");
    {
        RenderTable t;
        const RenderObjectId a = t.create(1, row(1));
        t.destroy(a, /*frame*/ 10);
        CHECK(t.retiredCount() == 1 && t.liveCount() == 0, "RETIRED, not FREE, on the frame it dies");
        t.collect(9);   // the GPU has finished frame 9, not 10
        const RenderObjectId b = t.create(2, row(2));
        CHECK(b.index != a.index, "before the fence the slot is NOT reused (new slot %u, retired %u)",
              b.index, a.index);
        CHECK(t.collect(10) == 1, "once frame 10 completes it is collected");
        const RenderObjectId c = t.create(3, row(3));
        CHECK(c.index == a.index, "and only then reused");
        CHECK(t.capacity() == 2, "two slots served three objects (capacity %zu)", t.capacity());
    }

    // ── 3. apply(): wholesale writes ───────────────────────────────────────
    std::printf("3. apply\n");
    {
        RenderScene s;
        s.apply({cap(10, 1), cap(11, 2), cap(12, 3, 1, true)}, /*frame*/ 1);
        CHECK(s.staticTable().liveCount() == 2 && s.skinnedTable().liveCount() == 1,
              "three objects: two static rows, one skinned (two tables, §9.2 #13)");
        const RenderObjectId id10 = s.idOf(10);

        s.apply({cap(10, 5), cap(11, 2), cap(12, 3, 1, true)}, 2);
        RowData r; s.staticTable().read(s.idOf(10), r);
        CHECK(s.idOf(10) == id10 && r.model.m[12] == 5.0f,
              "a moved object keeps its id and its row is rewritten");

        s.apply({cap(10, 5), cap(12, 3, 1, true)}, 3);
        CHECK(!s.idOf(11).valid() && s.staticTable().liveCount() == 1 && s.staticTable().retiredCount() == 1,
              "an object absent this frame is retired");

        s.apply({cap(10, 5), cap(12, 3, 1, false)}, 4);
        CHECK(s.skinnedTable().liveCount() == 0 && s.staticTable().liveCount() == 2,
              "an object that lost its skin moves to the static table");

        // Frames keep coming: the retired rows are collected kFramesInFlight later.
        for (uint64_t f = 5; f < 5 + RenderScene::kFramesInFlight + 1; ++f)
            s.apply({cap(10, 5), cap(12, 3)}, f);
        CHECK(s.staticTable().retiredCount() == 0, "retired rows are collected once the fence passes");
        CHECK(s.appliedThisFrame(5 + RenderScene::kFramesInFlight) && !s.appliedThisFrame(99),
              "the scene knows which frame it was applied for (one apply per frame, §9.3)");
    }

    // ── 4. The rebuild-and-diff check ──────────────────────────────────────
    std::printf("4. rebuild-and-diff\n");
    {
        RenderScene s;
        std::vector<CapturedRow> f1 = {cap(10, 1), cap(11, 2), cap(12, 3, 2, true)};
        s.apply(f1, 1);
        CHECK(s.diff(f1).empty(), "agrees with a rebuild right after apply");
        std::vector<CapturedRow> f2 = {cap(10, 4), cap(12, 3, 2, true)};
        s.apply(f2, 2);
        CHECK(s.diff(f2).empty(), "and after a move plus a removal");

        // Drift, as a missed change hook would cause it (P3b's failure mode):
        // the object moved, the table was not told.
        std::vector<CapturedRow> moved = {cap(10, 9), cap(12, 3, 2, true)};
        std::string d = s.diff(moved);
        CHECK(d.find("entity 10") != std::string::npos && d.find("'model'") != std::string::npos,
              "a row that was not rewritten: named by entity and column (%s)", d.c_str());
        std::vector<CapturedRow> spawned = {cap(10, 4), cap(12, 3, 2, true), cap(13, 1)};
        d = s.diff(spawned);
        CHECK(d.find("entity 13") != std::string::npos && d.find("no row") != std::string::npos,
              "a missed create (%s)", d.c_str());
        std::vector<CapturedRow> despawned = {cap(12, 3, 2, true)};
        d = s.diff(despawned);
        CHECK(d.find("entity 10") != std::string::npos && d.find("not extracted") != std::string::npos,
              "a missed destroy (%s)", d.c_str());
        std::vector<CapturedRow> reskinned = {cap(10, 4), cap(12, 3, 2, false)};
        d = s.diff(reskinned);
        CHECK(d.find("entity 12") != std::string::npos && d.find("table") != std::string::npos,
              "a row in the wrong table (%s)", d.c_str());
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
