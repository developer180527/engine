// ── scene_parents_test — the hierarchy post-pass over a hostile scene ────────
// `createEntity` is now fuzzed (tests/fuzz_entity_serde_test.cpp) and every
// component read inside it is type- and bounds-checked. But entity creation is
// only HALF of loading a scene. Parent links cannot be restored during the
// entity pass — a child may appear before its parent — so they run afterwards
// in `SceneSerializer::restoreParents`, reading `id` and `parentId` straight
// off the same untrusted JSON.
//
// That function is reached from BOTH scene paths:
//   • `loadAsync`      — a `.scene` file from disk. Hostile input is real here.
//   • `loadIntoWorld`  — the editor's Play snapshot, in-process.
// and in `loadAsync` it sits OUTSIDE the try/catch, which covers only
// `json::parse`. So a throw here propagates out of scene load: the editor dies
// on File→Open, or `engine_host` dies on boot.
//
// What is asserted:
//   1. restoreParents never throws, whatever the JSON is — wrong-typed ids, a
//      non-array `entities`, entities that are not objects.
//   2. Valid links are actually restored (or the test above is vacuous).
//   3. Hostile hierarchies do not produce a cycle or an over-deep chain.
//      `safeReparent` is the guard; flecs ABORTS past FLECS_DAG_DEPTH_MAX, so
//      "malformed scene" must never become "process gone".
//   4. One bad record does not cost the others their parents — losing the whole
//      hierarchy because one entity has a typo'd id is silent data damage the
//      user sees only as a scene that has quietly come apart.
#include <cstdio>
#include <string>

#include <flecs.h>
#include <nlohmann/json.hpp>

#include "scene/scene_serializer.h"
#include "components/entity_id.h"
#include "components/name.h"
#include "core/entity_id_util.h"

using json = nlohmann::json;

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

static flecs::entity spawn(flecs::world& w, uint64_t id, const char* name) {
    flecs::entity e = w.entity();
    e.set<Name>({ name });
    e.set<EntityId>({ id });
    return e;
}

// Depth of the ancestor chain; -1 means it did not terminate (a cycle).
static int chainDepth(flecs::entity e, int limit = 256) {
    flecs::entity cur = e;
    for (int i = 0; i < limit; ++i) {
        flecs::entity p = cur.target(flecs::ChildOf);
        if (!p || !p.is_alive()) return i;
        cur = p;
    }
    return -1;
}

// Every call goes through this: the property under test is "returns normally",
// so a throw has to be caught here rather than taking the process with it.
static bool restoreSurvives(flecs::world& w, const json& scene) {
    try { SceneSerializer::restoreParents(w, scene); }
    catch (const std::exception&) { return false; }
    catch (...)                   { return false; }
    return true;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("scene_parents_test\n");

    // ── The happy path, so nothing below is vacuous ─────────────────────────
    {
        flecs::world w;
        spawn(w, 100, "Parent");
        flecs::entity child = spawn(w, 200, "Child");

        json scene;
        scene["entities"] = json::array({
            json{ {"id", 100} },
            json{ {"id", 200}, {"parentId", 100} },
        });
        CHECK(restoreSurvives(w, scene), "a well-formed scene restores parents");
        flecs::entity p = child.target(flecs::ChildOf);
        CHECK(p && p.is_alive() && p.get<EntityId>().value == 100,
              "...and the child is actually parented");
    }

    // ── Wrong-typed identity fields ─────────────────────────────────────────
    // `je.value("id", (uint64_t)0)` does NOT fall back to 0 when `id` exists
    // with the wrong type — nlohmann throws type_error. This is the same defect
    // class already fixed inside createEntity; the post-pass reads the same
    // fields from the same file and was never covered.
    {
        struct Case { const char* what; json scene; };
        const Case cases[] = {
            { "id as a string",
              json{ {"entities", json::array({ json{ {"id","abc"}, {"parentId",100} } })} } },
            { "parentId as a string",
              json{ {"entities", json::array({ json{ {"id",200}, {"parentId","abc"} } })} } },
            { "id as an array",
              json{ {"entities", json::array({ json{ {"id",json::array({1,2})}, {"parentId",100} } })} } },
            { "parentId as an object",
              json{ {"entities", json::array({ json{ {"id",200}, {"parentId",json::object()} } })} } },
            { "id as null",
              json{ {"entities", json::array({ json{ {"id",nullptr}, {"parentId",100} } })} } },
            { "id as a negative number",
              json{ {"entities", json::array({ json{ {"id",-5}, {"parentId",100} } })} } },
            { "id as a float",
              json{ {"entities", json::array({ json{ {"id",1.5}, {"parentId",2.5} } })} } },
            { "an entity that is not an object",
              json{ {"entities", json::array({ 42, "text", nullptr })} } },
            { "entities is a number",        json{ {"entities", 42} } },
            { "entities is a string",        json{ {"entities", "nope"} } },
            { "entities is an object",       json{ {"entities", json::object()} } },
            { "no entities key at all",      json::object() },
            { "the scene is an array",       json::array({1,2,3}) },
            { "the scene is a number",       json(7) },
            { "the scene is null",           json(nullptr) },
        };
        for (const auto& c : cases) {
            flecs::world w;
            spawn(w, 100, "Parent");
            spawn(w, 200, "Child");
            CHECK(restoreSurvives(w, c.scene),
                  "%s: restoreParents returns instead of throwing", c.what);
        }
    }

    // ── One bad record must not cost the others their parents ───────────────
    // A single typo'd id in a 500-entity scene must lose ONE link, not the
    // hierarchy. If the loop throws on the bad record, everything after it is
    // silently left unparented — the scene opens, looks almost right, and has
    // quietly come apart.
    {
        flecs::world w;
        spawn(w, 100, "Parent");
        flecs::entity good1 = spawn(w, 200, "Good1");
        flecs::entity good2 = spawn(w, 300, "Good2");

        json scene;
        scene["entities"] = json::array({
            json{ {"id", 100} },
            json{ {"id", 200}, {"parentId", 100} },
            json{ {"id", "corrupt"}, {"parentId", 100} },   // the bad record
            json{ {"id", 300}, {"parentId", 100} },         // AFTER the bad one
        });
        CHECK(restoreSurvives(w, scene), "a scene with one corrupt record survives");
        // A null entity must be tested for truth BEFORE is_alive() — flecs
        // asserts on is_alive(0).
        flecs::entity p1 = good1.target(flecs::ChildOf);
        flecs::entity p2 = good2.target(flecs::ChildOf);
        CHECK(p1 && p1.is_alive(),
              "the entity BEFORE the corrupt record keeps its parent");
        CHECK(p2 && p2.is_alive(),
              "and so does the one AFTER it — one typo costs one link, not the "
              "whole hierarchy");
    }

    // ── Hostile hierarchies ─────────────────────────────────────────────────
    // safeReparent is the guard. flecs aborts past FLECS_DAG_DEPTH_MAX, so an
    // over-deep chain from a malformed file is a process kill, not a warning.
    {
        flecs::world w;
        flecs::entity a = spawn(w, 1, "A");
        json scene;
        scene["entities"] = json::array({ json{ {"id",1}, {"parentId",1} } });  // SELF
        CHECK(restoreSurvives(w, scene), "a self-parent is survivable");
        CHECK(chainDepth(a) >= 0, "...and does not create a cycle");
    }
    {
        flecs::world w;
        flecs::entity a = spawn(w, 1, "A");
        flecs::entity b = spawn(w, 2, "B");
        json scene;
        scene["entities"] = json::array({          // mutual: A<-B and B<-A
            json{ {"id",1}, {"parentId",2} },
            json{ {"id",2}, {"parentId",1} },
        });
        CHECK(restoreSurvives(w, scene), "a mutual parent pair is survivable");
        CHECK(chainDepth(a) >= 0 && chainDepth(b) >= 0,
              "...and neither entity ends up in a cycle");
    }
    {
        // A chain far deeper than FLECS_DAG_DEPTH_MAX. Nothing stops a cooked
        // or hand-written scene from declaring one.
        flecs::world w;
        const int kDeep = 400;
        for (int i = 1; i <= kDeep; ++i) spawn(w, (uint64_t)i, "Link");
        json ents = json::array();
        for (int i = 2; i <= kDeep; ++i)
            ents.push_back(json{ {"id", i}, {"parentId", i - 1} });
        json scene; scene["entities"] = ents;

        CHECK(restoreSurvives(w, scene),
              "a %d-deep chain does not abort the process", kDeep);
        int deepest = 0;
        w.query<const EntityId>().each([&](flecs::entity e, const EntityId&) {
            const int d = chainDepth(e);
            if (d > deepest) deepest = d;
        });
        CHECK(deepest >= 0, "no entity ends up in a cycle (deepest chain %d)",
              deepest);
    }

    // ── Dangling and duplicate references ───────────────────────────────────
    {
        flecs::world w;
        flecs::entity child = spawn(w, 200, "Child");
        json scene;
        scene["entities"] = json::array({
            json{ {"id",200}, {"parentId", 999999} },     // parent never existed
        });
        CHECK(restoreSurvives(w, scene), "a dangling parentId is survivable");
        flecs::entity dp = child.target(flecs::ChildOf);
        CHECK(!(dp && dp.is_alive()),
              "...and leaves the child at the root rather than guessing");
    }
    {
        // Two entities claiming one id: the byId map keeps the last. The pass
        // must still terminate and leave a sane hierarchy.
        flecs::world w;
        spawn(w, 100, "Parent");
        spawn(w, 200, "ChildA");
        spawn(w, 200, "ChildB");                          // duplicate id
        json scene;
        scene["entities"] = json::array({
            json{ {"id",100} },
            json{ {"id",200}, {"parentId",100} },
            json{ {"id",200}, {"parentId",100} },
        });
        CHECK(restoreSurvives(w, scene), "duplicate ids are survivable");
    }

    if (g_failures) {
        std::printf("scene_parents_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("scene_parents_test: PASS\n");
    return 0;
}
