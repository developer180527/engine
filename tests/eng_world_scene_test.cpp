// ── eng_world_scene_test — a C API component in scenes and in the sim hash ───
// WO-073. What eng_world_test (plain C) cannot see from C: a component a kit
// registers through engRegisterComponent is saved and loaded with scenes field
// by field under its name, an entity field is saved as the target's stable id
// and linked again on load, a value it cannot read keeps the default, and the
// determinism gate classifies and hashes it (field by field, never padding).
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

    engDestroyWorld(world);
    engDestroyEngine(engine);
    std::printf(g_failures ? "\nFAILED: %d\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
