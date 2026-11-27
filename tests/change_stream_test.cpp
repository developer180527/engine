// ── change_stream_test — what changed since you last looked (WO-072) ─────────
// components/change_stream.h: subscribe, read and unsubscribe per component;
// nothing exists until something subscribes; changes merged per read; ids not
// reused under a reader; declared writes, whole column or marked rows. Then the
// production writers, run by a real headless runtime with Jolt: resting
// (sleeping) bodies are not reported, a falling one is. Then the retained
// render scene's input (render/retained_input.h) driving the real RenderScene
// incrementally, with the rebuild-and-diff check as the oracle: a write that
// does not mark is caught by name.
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <flecs.h>

#include "components/change_stream.h"
#include "components/mesh_renderer.h"
#include "components/prev_transform.h"
#include "components/rigid_body.h"
#include "components/spinner.h"
#include "components/transform_hierarchy.h"
#include "core/transform.h"
#include "plugins/jolt_plugin.h"
#include "render/retained_input.h"
#include "render/world/render_scene.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

using changes::Change;
using changes::Kind;
using changes::Streams;

static std::vector<Change> drain(Streams& s, changes::StreamId id) {
    std::vector<Change> out;
    Change buf[4];
    uint32_t n;
    do {   // a small batch on purpose: the "call until fewer" loop is exercised
        n = s.read(id, buf, 4);
        out.insert(out.end(), buf, buf + n);
    } while (n == 4);
    return out;
}
static bool only(const std::vector<Change>& v, flecs::entity_t e, Kind k) {
    return v.size() == 1 && v[0].entity == e && v[0].kind == k;
}
static Transform at(float x, float y = 0, float z = 0) { return {{x, y, z}, {0, 0, 0, 1}, {1, 1, 1}}; }

// ── 1. Free with no subscriber ──────────────────────────────────────────────
static void freeWithoutSubscriber() {
    std::printf("1. nothing exists until something subscribes\n");
    flecs::world w;
    flecs::entity e = w.entity().set<Transform>(at(1));
    e.set<Transform>(at(2));
    changes::markChanged<Transform>(e);
    CHECK(Streams::of(w.c_ptr()) == nullptr, "writes, sets and a declared write: no stream object created");
    CHECK(!changes::Marks<Transform>(w).on(), "a writer's handle is off");

    Streams& s = Streams::ensure(w.c_ptr());
    const changes::StreamId id = s.subscribe(w.id<Transform>());
    CHECK(changes::Marks<Transform>(w).on(), "subscribed: the writer's handle is on");
    s.unsubscribe(id);
    CHECK(!s.watching(w.id<Transform>()) && !changes::Marks<Transform>(w).on(),
          "the last unsubscribe turns it off again");
    e.set<Transform>(at(3));
    const changes::StreamId again = s.subscribe(w.id<Transform>());
    const auto seen = drain(s, again);
    CHECK(only(seen, e.id(), Kind::Added), "a later subscriber sees only what exists, as ADDED (%zu)", seen.size());
    s.unsubscribe(again);
    s.unsubscribe(again);
    CHECK(true, "unsubscribing twice is harmless");
}

// ── 2. Merged per read ──────────────────────────────────────────────────────
static void mergedPerRead() {
    std::printf("2. merged per read: an entity appears once between two reads\n");
    flecs::world w;
    flecs::entity old = w.entity().set<Transform>(at(0));
    Streams& s = Streams::ensure(w.c_ptr());
    const changes::StreamId id = s.subscribe(w.id<Transform>());
    CHECK(only(drain(s, id), old.id(), Kind::Added), "subscribing queues what is already there as ADDED");

    for (int i = 0; i < 10; ++i) old.set<Transform>(at((float)i));
    CHECK(only(drain(s, id), old.id(), Kind::Changed), "changed ten times: one CHANGED");

    flecs::entity a = w.entity().set<Transform>(at(1));
    a.set<Transform>(at(2));
    CHECK(only(drain(s, id), a.id(), Kind::Added), "added then changed: ADDED");

    flecs::entity b = w.entity().set<Transform>(at(1));
    b.remove<Transform>();
    CHECK(drain(s, id).empty(), "added then removed: nothing");

    a.set<Transform>(at(5));
    a.remove<Transform>();
    CHECK(only(drain(s, id), a.id(), Kind::Removed), "changed then removed: REMOVED");

    old.remove<Transform>();
    old.set<Transform>(at(9));
    CHECK(only(drain(s, id), old.id(), Kind::Changed), "removed then added back: CHANGED (it existed then and does now)");

    // Partial reads: the rest waits, and keeps merging.
    std::vector<flecs::entity> many;
    for (int i = 0; i < 10; ++i) many.push_back(w.entity().set<Transform>(at((float)i)));
    Change buf[3];
    const uint32_t first = s.read(id, buf, 3);
    many[9].set<Transform>(at(99));      // still unread: merges into its ADDED
    const auto rest = drain(s, id);
    CHECK(first == 3 && rest.size() == 7 && rest.back().entity == many[9].id() && rest.back().kind == Kind::Added,
          "a short read leaves the rest queued, still merging (%u + %zu)", first, rest.size());

    // Two streams on one component are independent.
    const changes::StreamId other = s.subscribe(w.id<Transform>());
    drain(s, other);
    many[0].set<Transform>(at(-1));
    CHECK(only(drain(s, id), many[0].id(), Kind::Changed) && only(drain(s, other), many[0].id(), Kind::Changed),
          "two subscribers each see the change once");
    s.unsubscribe(other);
    many[1].set<Transform>(at(-1));
    CHECK(only(drain(s, id), many[1].id(), Kind::Changed), "ending one stream leaves the other running");
}

// ── 3. Deletion, and ids not reused under a reader ──────────────────────────
static void idsNotReused() {
    std::printf("3. a deleted entity is REMOVED, and its slot's next entity is a different id\n");
    flecs::world w;
    flecs::entity a = w.entity().set<Transform>(at(1));
    Streams& s = Streams::ensure(w.c_ptr());
    const changes::StreamId id = s.subscribe(w.id<Transform>());
    drain(s, id);
    const flecs::entity_t oldId = a.id();
    a.destruct();
    flecs::entity b = w.entity().set<Transform>(at(2));   // recycles a's index
    const auto seen = drain(s, id);
    const bool recycled = (uint32_t)b.id() == (uint32_t)oldId;
    CHECK(recycled && b.id() != oldId, "the new entity reuses the slot with a new generation (%llx vs %llx)",
          (unsigned long long)b.id(), (unsigned long long)oldId);
    const bool removedOld = std::any_of(seen.begin(), seen.end(), [&](const Change& c) {
        return c.entity == oldId && c.kind == Kind::Removed; });
    const bool addedNew = std::any_of(seen.begin(), seen.end(), [&](const Change& c) {
        return c.entity == b.id() && c.kind == Kind::Added; });
    CHECK(seen.size() == 2 && removedOld && addedNew,
          "one read reports the old id REMOVED and the new id ADDED, never merged (%zu entries)", seen.size());
}

// ── 4. Declared writes ──────────────────────────────────────────────────────
static void declaredWrites() {
    std::printf("4. a write in place is seen only when declared: a whole column, or marked rows\n");
    flecs::world w;
    std::vector<flecs::entity> es;
    for (int i = 0; i < 4; ++i) es.push_back(w.entity().set<Transform>(at((float)i)));
    Streams& s = Streams::ensure(w.c_ptr());
    const changes::StreamId id = s.subscribe(w.id<Transform>());
    drain(s, id);
    auto q = w.query<Transform>();

    q.each([](Transform& t) { t.position.y += 1; });
    CHECK(drain(s, id).empty(), "undeclared: invisible (this is why writes are declared)");

    q.run([&](flecs::iter& it) {
        const changes::Marks<Transform> marks(it.world());
        while (it.next()) {
            auto t = it.field<Transform>(0);
            for (auto i : it) t[i].position.y += 1;
            marks.column(it);
        }
    });
    CHECK(drain(s, id).size() == 4, "the whole column: every row reported");

    q.run([&](flecs::iter& it) {
        const changes::Marks<Transform> marks(it.world());
        while (it.next()) {
            auto t = it.field<Transform>(0);
            for (auto i : it) if (i == 1 || i == 2) t[i].position.y += 1;
            marks.rows(it, 1, 2);
        }
    });
    const auto marked = drain(s, id);
    CHECK(marked.size() == 2 && marked[0].entity == es[1].id() && marked[1].entity == es[2].id(),
          "marked rows: only those two reported (%zu)", marked.size());
}

// ── 5. Tags and pairs ───────────────────────────────────────────────────────
static void tagsAndPairs() {
    std::printf("5. what takes an entity in or out of a query: Disabled, a parent\n");
    flecs::world w;
    flecs::entity p = w.entity().set<Transform>(at(0));
    flecs::entity q = w.entity().set<Transform>(at(5));
    flecs::entity c = w.entity().set<Transform>(at(1));
    Streams& s = Streams::ensure(w.c_ptr());
    const changes::StreamId dis = s.subscribe(flecs::Disabled);
    const changes::StreamId par = s.subscribe(ecs_pair(flecs::ChildOf, flecs::Wildcard));
    const changes::StreamId tr  = s.subscribe(w.id<Transform>());
    // Each subscription first queues what already has it, flecs' own
    // entities included (its modules are children of flecs.core).
    const size_t seeded = drain(s, par).size();
    drain(s, dis); drain(s, tr);
    CHECK(seeded > 0, "the (ChildOf, *) stream starts with every existing child, as ADDED (%zu)", seeded);
    c.disable();
    CHECK(only(drain(s, dis), c.id(), Kind::Added), "disabling: Disabled ADDED");
    c.set<Transform>(at(2));
    CHECK(only(drain(s, tr), c.id(), Kind::Changed), "a disabled entity's change is still seen");
    c.enable();
    CHECK(only(drain(s, dis), c.id(), Kind::Removed), "re-enabling: Disabled REMOVED");
    c.child_of(p);
    CHECK(only(drain(s, par), c.id(), Kind::Added), "parenting: (ChildOf, *) ADDED");
    c.child_of(q);
    CHECK(only(drain(s, par), c.id(), Kind::Changed), "reparenting: CHANGED");
}

// ── 6. The production writers: sleeping bodies are not reported ────────────
static void physicsSleepers() {
    std::printf("6. a real runtime with Jolt: resting bodies sleep and are not reported, a falling one is\n");
    EngineConfig cfg;
    cfg.openAssetDatabase = false; cfg.autoDetectProject = false; cfg.defaultScene = false;
    EngineRuntime engine;
    CHECK(engine.init(cfg, std::make_unique<HeadlessPlatform>()), "a headless runtime");
    engine.plugins().add(std::make_shared<JoltPlugin>());
    engine.attachPlugins();
    flecs::world& w = engine.simWorld();
    RigidBody floorRb{}; floorRb.bodyType = PhysicsBodyType::Static; floorRb.halfExtent = {50, 0.5f, 50};
    w.entity().set<Transform>(at(0, -0.5f)).set<RigidBody>(floorRb);
    auto box = [&](float x, float y) {
        RigidBody rb{}; rb.bodyType = PhysicsBodyType::Dynamic; rb.halfExtent = {0.5f, 0.5f, 0.5f}; rb.mass = 1;
        return w.entity().set<Transform>(at(x, y)).set<RigidBody>(rb);
    };
    std::vector<flecs::entity> resting;
    for (int i = 0; i < 8; ++i) resting.push_back(box((float)i * 2.0f - 8.0f, 0.5f));
    const flecs::entity falling = box(30, 2000);
    flecs::entity spinner = w.entity().set<Transform>(at(-30)).set<Spinner>({1.0f, 0.0f});
    engine.startSimulation(EngineRuntime::SimMode::InPlace);
    for (int t = 0; t < 180; ++t) engine.tick(1.0f / 60.0f);   // 3 s: the boxes settle and sleep

    Streams& s = Streams::ensure(w.c_ptr());
    const changes::StreamId tr   = s.subscribe(w.id<Transform>());
    const changes::StreamId prev = s.subscribe(w.id<PrevTransform>());
    drain(s, tr); drain(s, prev);
    for (int t = 0; t < 10; ++t) engine.tick(1.0f / 60.0f);
    auto has = [](const std::vector<Change>& v, flecs::entity_t e) {
        return std::any_of(v.begin(), v.end(), [&](const Change& c) { return c.entity == e; }); };
    const auto moved = drain(s, tr);
    const bool anyResting = std::any_of(resting.begin(), resting.end(), [&](flecs::entity e) { return has(moved, e.id()); });
    CHECK(has(moved, falling.id()) && has(moved, spinner.id()) && !anyResting && moved.size() == 2,
          "Transform: the falling body and the spinner, no sleeping body (%zu reported of 10 movers and sleepers)",
          moved.size());
    const auto hist = drain(s, prev);
    const bool anyRestingPrev = std::any_of(resting.begin(), resting.end(), [&](flecs::entity e) { return has(hist, e.id()); });
    CHECK(has(hist, falling.id()) && !anyRestingPrev,
          "PrevTransform (prevSnapshot's marked write): the falling body, no sleeping body (%zu)", hist.size());
    engine.stopSimulation();
    engine.shutdown();
}

// ── 7. The retained render scene, incremental, against its rebuild ─────────
// What extraction does for parentless rows (renderer/extract.cpp), without a
// GPU: the same RetainedInput and RenderScene, rows from the same production
// localMatrixLerp. Each step returns the rebuild-and-diff result.
struct Mirror {
    rworld::RetainedInput input;
    rworld::RenderScene   scene;
    uint64_t frame = 1, meshRevision = 0, written = 0;
    float    alpha = 1.0f;
    std::string step(flecs::world& w) {
        const bool full = input.read(w, {meshRevision, 0, alpha});
        std::vector<rworld::CapturedRow> rows;
        w.query_builder<const Transform, const MeshRenderer, const PrevTransform*>()
            .without(flecs::ChildOf, flecs::Wildcard).build()
            .each([&](flecs::entity e, const Transform& t, const MeshRenderer& mr, const PrevTransform* p) {
                rworld::CapturedRow c;
                c.owner = e.id();
                localMatrixLerp(t, p, alpha, c.row.model.m);
                c.row.mesh = mr.mesh;
                c.row.material = mr.materialOverride;
                c.dirty = input.needsRow(e.id(), t, p);
                rows.push_back(c);
            });
        if (full) scene.apply(rows, frame); else scene.applyChanged(rows, input.touched(), frame);
        written = 0;
        for (const auto& r : rows) written += r.dirty ? 1 : 0;
        ++frame;
        return scene.diff(rows);
    }
};

static void retainedScene() {
    std::printf("7. the retained render scene, written from the streams, agrees with its rebuild\n");
    flecs::world w;
    std::vector<flecs::entity> es;
    for (int i = 0; i < 100; ++i)
        es.push_back(w.entity().set<Transform>(at((float)i)).set<MeshRenderer>({MeshHandle{1u + (uint32_t)(i % 3)}, {}}));
    Mirror m;
    std::string d = m.step(w);
    CHECK(d.empty() && m.written == 100 && m.scene.liveCount() == 100, "the first frame writes every row (%llu)",
          (unsigned long long)m.written);
    d = m.step(w);
    CHECK(d.empty() && m.written == 0, "nothing changed: nothing written, and the table still agrees (%llu)",
          (unsigned long long)m.written);

    es[3].set<Transform>(at(-3));
    changes::Marks<Transform>(w).entity(es[4].id());
    es[4].get_mut<Transform>().position.y = 7;   // in place, declared
    es[5].get_mut<MeshRenderer>().materialOverride = MaterialHandle{9};
    changes::markChanged<MeshRenderer>(es[5]);
    es[6].destruct();
    flecs::entity fresh = w.entity().set<Transform>(at(500)).set<MeshRenderer>({MeshHandle{2}, {}});
    es[7].disable();
    es[8].child_of(es[9]);
    d = m.step(w);
    CHECK(d.empty() && m.written == 4 && m.scene.liveCount() == 98,
          "set, marked, override, delete, new, disabled, parented: 4 rows written, 3 retired, and it agrees (%s)",
          d.empty() ? "agrees" : d.c_str());
    CHECK(!m.scene.idOf(es[6].id()).valid() && m.scene.idOf(fresh.id()).valid(), "the deleted one has no row, the new one has");

    // THE MUTATION THE ORACLE EXISTS FOR: a write in place that is not marked.
    es[10].get_mut<Transform>().position.x = 1234;
    d = m.step(w);
    CHECK(d.find("entity " + std::to_string(es[10].id())) != std::string::npos && d.find("model") != std::string::npos,
          "a write that does not mark is caught by the rebuild-and-diff, by entity and column: %s", d.c_str());
    changes::markChanged<Transform>(es[10]);
    d = m.step(w);
    CHECK(d.empty() && m.written == 1, "marked, the next frame repairs it");

    // Interpolation: a row whose history differs is rewritten every frame
    // (its matrix moves with alpha); one whose history equals it is not.
    m.alpha = 0.5f;
    d = m.step(w);
    CHECK(d.empty(), "interpolation switching on rewrites the table once");
    es[20].set<PrevTransform>({{20, 1, 0}, {0, 0, 0, 1}, {1, 1, 1}});   // history differs: interpolating
    es[21].set<PrevTransform>({{21, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}});   // history equals: at rest
    d = m.step(w);
    m.alpha = 0.25f;
    std::string d2 = m.step(w);
    CHECK(d.empty() && d2.empty() && m.written == 1,
          "alpha moves: only the interpolating row is written, the resting one with history is not (%llu)",
          (unsigned long long)m.written);

    // A mesh loaded or unloaded changes what a handle resolves to: all rows.
    m.meshRevision++;
    d = m.step(w);
    CHECK(d.empty() && m.written == m.scene.liveCount(), "a mesh registry change rewrites every row");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("change_stream_test\n");
    freeWithoutSubscriber();
    mergedPerRead();
    idsNotReused();
    declaredWrites();
    tagsAndPairs();
    physicsSleepers();
    retainedScene();
    std::printf(g_failures ? "\nFAILED: %d\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
