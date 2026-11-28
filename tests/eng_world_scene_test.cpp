// ── eng_world_scene_test — a C API component in scenes and in the sim hash ───
// WO-073. What eng_world_test (plain C) cannot see from C: a component a kit
// registers through engRegisterComponent is saved and loaded with scenes field
// by field under its name, an entity field is saved as the target's stable id
// and linked again on load, a value it cannot read keeps the default, and the
// determinism gate classifies and hashes it (field by field, never padding).
// A BLOB field (part 2) saves its bytes as base64, loads into a blob of its
// own, and hashes by its bytes, never by its handle.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <eng/engine.h>
#include <nlohmann/json.hpp>

#include "capi/eng_internal.h"
#include "components/entity_id_util.h"
#include "runtime/runtime.h"
#include "runtime/sim_hash.h"
#include "scene/entity_serializer.h"

namespace { int g_failures = 0; }
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL  " __VA_ARGS__); std::printf("  (%s:%d)\n", __FILE__, __LINE__); ++g_failures; } \
                           else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } } while (0)

using nlohmann::json;
using namespace EntitySerde;

struct Thruster {
    float    thrust;
    uint32_t armed;       // BOOL32
    EngEntity target;
    float    dir[4];      // VEC4
    char     label[16];   // FIXED_STRING
};
struct Route {
    EngBlob  points;      // BLOB
    float    width;
    uint32_t pad;
};

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("eng_world_scene_test\n");

    EngEngineDesc ed = ENG_INIT(EngEngineDesc);
    ed.api_version = ENG_API_VERSION;
    EngEngine* engine = nullptr;
    EngWorld* world = nullptr;
    CHECK(engCreateEngine(&ed, &engine) == ENG_OK && engCreateWorld(engine, nullptr, &world) == ENG_OK, "an engine and its world");

    const EngFieldDesc fields[] = {
        {"thrust", ENG_FIELD_F32,          offsetof(Thruster, thrust), 1, 0},
        {"armed",  ENG_FIELD_BOOL32,       offsetof(Thruster, armed),  1, 0},
        {"target", ENG_FIELD_ENTITY,       offsetof(Thruster, target), 1, 0},
        {"dir",    ENG_FIELD_VEC4,         offsetof(Thruster, dir),    1, 0},
        {"label",  ENG_FIELD_FIXED_STRING, offsetof(Thruster, label),  16, 0},
    };
    Thruster defaults{};
    defaults.thrust = 7.5f;
    EngComponentDesc d = ENG_INIT(EngComponentDesc);
    d.name = "apollo.Thruster";
    d.size = sizeof(Thruster);
    d.align = alignof(Thruster);
    d.fields = fields;
    d.field_count = 5;
    d.defaults = &defaults;
    EngComponent thruster = 0;
    CHECK(engRegisterComponent(world, &d, &thruster) == ENG_OK, "a kit's component with every kind of field this version saves");

    EngEntity a = 0, b = 0;
    engCreateEntity(world, "lander", &a);
    engCreateEntity(world, "target", &b);
    Thruster t{};
    t.thrust = 2500.0f;
    t.armed = 1;
    t.target = b;
    t.dir[0] = 0; t.dir[1] = 1; t.dir[2] = 0; t.dir[3] = 0.5f;
    std::strcpy(t.label, "descent");
    CHECK(engAddComponent(world, a, thruster, &t) == ENG_OK, "set on an entity, naming another entity");

    const EngFieldDesc routeFields[] = {
        {"points", ENG_FIELD_BLOB, offsetof(Route, points), 1, 0},
        {"width",  ENG_FIELD_F32,  offsetof(Route, width),  1, 0},
    };
    EngComponentDesc rd = ENG_INIT(EngComponentDesc);
    rd.name = "apollo.Route"; rd.size = sizeof(Route); rd.align = alignof(Route);
    rd.fields = routeFields; rd.field_count = 2;
    EngComponent route = 0;
    CHECK(engRegisterComponent(world, &rd, &route) == ENG_OK, "a kit's component with a BLOB field");
    const unsigned char pts[] = {'a', 'b', 'c', 0x00, 0xFF};
    EngBlob blob = 0;
    engCreateBlob(world, pts, sizeof pts, &blob);
    Route r{blob, 1.5f, 0};
    CHECK(engAddComponent(world, a, route, &r) == ENG_OK, "holding a blob of five bytes, a NUL and 0xFF among them");

    flecs::world& w = engInternalRuntime(engine)->simWorld();
    assignMissingIds(w);   // as every scene save does first
    SerdeContext ctx;
    ctx.mode = SerdeMode::Disk;
    const json savedA = saveEntity(flecs::entity(w, a), ctx);
    const json savedB = saveEntity(flecs::entity(w, b), ctx);
    const json& th = savedA["apollo.Thruster"];
    const uint64_t bStable = ensureEntityId(flecs::entity(w, b));
    CHECK(th.is_object() && th["thrust"] == 2500.0 && th["armed"] == true && th["label"] == "descent"
          && th["dir"] == json::array({0.0, 1.0, 0.0, 0.5}) && th["target"] == bStable,
          "saved field by field under its name, the target as its stable id: %s", th.dump().c_str());
    CHECK(savedA["apollo.Route"]["points"] == "YWJjAP8=" && savedA["apollo.Route"]["width"] == 1.5,
          "a blob saved as its bytes in base64: %s", savedA["apollo.Route"].dump().c_str());

    // Load both into the world again, as a scene loader does: refs linked once all exist.
    std::vector<SerdeContext::EntityRef> refs;
    SerdeContext lctx;
    lctx.mode = SerdeMode::Disk;
    lctx.entityRefs = &refs;
    json sa = savedA, sb = savedB;
    sa["id"] = 900001; sb["id"] = 900002;
    sa["apollo.Thruster"]["target"] = 900002;     // the copy names the copy
    sa["apollo.Thruster"]["thrust"] = "not a number";   // unreadable: the default
    flecs::entity la = createEntity(w, sa, lctx, IdPolicy::Preserve);
    flecs::entity lb = createEntity(w, sb, lctx, IdPolicy::Preserve);
    resolveEntityRefs(w, lctx);
    Thruster back{};
    CHECK(engGetComponent(world, la.id(), thruster, &back, sizeof back) == ENG_OK && back.target == lb.id()
          && back.armed == 1 && std::strcmp(back.label, "descent") == 0 && back.dir[3] == 0.5f,
          "loaded back, the entity field linked to the loaded target");
    CHECK(back.thrust == 7.5f, "a value it cannot read keeps the registered default (%.1f)", back.thrust);
    Route rb{};
    const void* bp = nullptr;
    uint32_t bn = 0;
    CHECK(engGetComponent(world, la.id(), route, &rb, sizeof rb) == ENG_OK && rb.points && rb.points != blob
          && engGetBlob(world, rb.points, &bp, &bn) == ENG_OK && bn == sizeof pts && std::memcmp(bp, pts, bn) == 0,
          "loaded into a blob of its own, the same five bytes");
    {
        json sc = savedA;
        sc["id"] = 900003;
        sc.erase("apollo.Thruster");
        sc["apollo.Route"]["points"] = "not base64!";
        std::vector<SerdeContext::EntityRef> refs2;
        SerdeContext c2;
        c2.mode = SerdeMode::Disk;
        c2.entityRefs = &refs2;
        flecs::entity lc = createEntity(w, sc, c2, IdPolicy::Preserve);
        Route rc{};
        CHECK(engGetComponent(world, lc.id(), route, &rc, sizeof rc) == ENG_OK && rc.points == 0 && rc.width == 1.5f,
              "text that is not base64 keeps the default (no blob), and the rest loads");
        engDestroyEntity(world, lc.id());
    }

    // The determinism gate: classified, and hashed by value.
    std::vector<std::string> unclassified;
    const bool covered = simhash::auditCoverage(w, unclassified);
    CHECK(covered, "the sim hash's audit finds nothing unclassified (%zu)", unclassified.size());
    const uint64_t h0 = simhash::hashWorld(w);
    t.thrust = 2501.0f;
    engSetComponent(world, a, thruster, &t, sizeof t);
    const uint64_t h1 = simhash::hashWorld(w);
    t.thrust = 2500.0f;
    engSetComponent(world, a, thruster, &t, sizeof t);
    CHECK(h0 != h1 && simhash::hashWorld(w) == h0, "a field change changes the world's hash, and setting it back restores it");
    const unsigned char other[] = {'a', 'b', 'd', 0x00, 0xFF};
    engSetBlob(world, blob, other, sizeof other);
    const uint64_t h2 = simhash::hashWorld(w);
    EngBlob same = 0;
    engCreateBlob(world, pts, sizeof pts, &same);
    r.points = same;   // a new handle with the first bytes; the changed one is freed
    engSetComponent(world, a, route, &r, sizeof r);
    CHECK(h2 != h0 && simhash::hashWorld(w) == h0, "a blob's bytes reach the hash; its handle does not");


    // ── A vector read as SchemaBuilder reads one (review, 2026-11-28) ───────
    // dynamic_schema.h says its rules are SchemaBuilder's, and the scene format
    // promises short arrays fill what they can: [1, 2] into a VEC4 is
    // (1, 2, default, default), and a bad element keeps only its own default.
    {
        std::vector<SerdeContext::EntityRef> refs3;
        SerdeContext c3; c3.mode = SerdeMode::Disk; c3.entityRefs = &refs3;
        json sv = savedA;
        sv["id"] = 900010;
        sv.erase("apollo.Route");
        sv["apollo.Thruster"]["dir"] = json::array({1.0, 2.0});
        flecs::entity sh = createEntity(w, sv, c3, IdPolicy::Preserve);
        json sw = savedA;
        sw["id"] = 900011;
        sw.erase("apollo.Route");
        sw["apollo.Thruster"]["dir"] = json::array({3.0, "x", 5.0, 6.0});
        flecs::entity bad = createEntity(w, sw, c3, IdPolicy::Preserve);
        resolveEntityRefs(w, c3);
        Thruster s1{}, s2{};
        engGetComponent(world, sh.id(), thruster, &s1, sizeof s1);
        engGetComponent(world, bad.id(), thruster, &s2, sizeof s2);
        CHECK(s1.dir[0] == 1.0f && s1.dir[1] == 2.0f && s1.dir[2] == 0.0f && s1.dir[3] == 0.0f,
              "a short array fills what it has, the rest default (%g %g %g %g)", s1.dir[0], s1.dir[1], s1.dir[2], s1.dir[3]);
        CHECK(s2.dir[0] == 3.0f && s2.dir[1] == 0.0f && s2.dir[2] == 5.0f && s2.dir[3] == 6.0f,
              "an element that is not a number keeps its own default (%g %g %g %g)", s2.dir[0], s2.dir[1], s2.dir[2], s2.dir[3]);
    }

    // ── What a kit made goes when it stops, slot reused or not (review) ─────
    // WO-094: a kit's queries are destroyed at its stop. The second Play reuses
    // the slots the first one freed.
    {
        static const int kitA = 0;
        const EngQueryTerm term{thruster, ENG_ACCESS_READ, {0, 0}};
        EngQueryDesc qd = ENG_INIT(EngQueryDesc);
        qd.terms = &term; qd.term_count = 1;
        for (int play = 1; play <= 2; ++play) {
            const void* was = engInternalSetOwner(world, &kitA);
            EngQuery q = 0;
            engCreateQuery(world, &qd, &q);
            engInternalSetOwner(world, was);
            const uint32_t dropped = engInternalDropOwned(world, &kitA);
            EngQueryIter* it = nullptr;
            const EngResult r = engQueryBegin(world, q, &it);
            if (it) engQueryEnd(it);
            CHECK(dropped == 1 && r == ENG_ERROR_STALE_HANDLE,
                  "play %d: the kit's query is destroyed at its stop (dropped %u)", play, dropped);
        }
    }

    engDestroyWorld(world);
    engDestroyEngine(engine);
    std::printf(g_failures ? "\nFAILED: %d\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
