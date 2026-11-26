// ── scene_schema_test — the scene format, held byte for byte (WO-071) ─────────
//
// The scene serializer was a hand-written table, one save and one load function
// per component. WO-071 replaces it with a schema registry and one generic
// serializer, and the contract of that change is that NOTHING ON DISK MOVES: a
// scene saved before it loads and saves back to the same bytes.
//
// So the fixtures under tests/fixtures/scenes/ were written by the hand-written
// table, before the registry existed (`scene_schema_test --write-fixtures`, at
// the commit that added this file), and they are never regenerated to make this
// test pass. A change that needs them regenerated is a change to the format,
// which is a revision bump with a migration (DR-0034), not an edit here.
//
//   every_component.disk.json    every saved component, as a .scene holds it
//   every_component.memory.json  the same through the in-memory snapshot (Play,
//                                undo), which carries live handle ids
//
// Entities are saved one by one in file order, not through a world query, so
// the comparison is of the serializer and not of flecs' table order.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <flecs.h>
#include <nlohmann/json.hpp>

#include "components/meta_registry.h"
#include "scene/entity_serializer.h"
#include "scene/extensions.h"

#include <climits>
#include <cstdint>
#include <set>
#include <vector>

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

using json = nlohmann::json;
using namespace EntitySerde;

namespace {

const std::string kFixtures = std::string(ENGINE_SOURCE_DIR) + "/tests/fixtures/scenes/";

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
// Never over an existing fixture: they record what an EARLIER serializer wrote,
// and regenerating one with the code under test would make the test agree with
// itself. Delete a fixture by hand only when its format change is a migration.
void writeFile(const std::string& path, const std::string& text) {
    if (std::ifstream(path).good()) { std::printf("kept     %s\n", path.c_str()); return; }
    std::ofstream f(path, std::ios::binary);
    f << text;
    std::printf("wrote    %s\n", path.c_str());
}

// A kit's component: reflected through flecs meta, saved under "reflected".
struct Health { float current = 100.0f; float max = 100.0f; };

flecs::world makeWorld() {
    flecs::world w;
    MetaRegistry::registerAll(w);
    w.component<Health>("combat::Health")
        .member("current", &Health::current)
        .member("max", &Health::max);
    return w;
}

// ── The world the fixtures describe ─────────────────────────────────────────
// Every component the serializer saves, each with values that are NOT its
// defaults, so a field that is dropped or defaulted shows as a difference.
void populate(flecs::world& w, SerdeMode mode) {
    flecs::entity a = w.entity();
    a.set<EntityId>({1001});
    a.set<Name>({"Everything"});
    Transform t;
    t.position = {1.5f, -2.25f, 3.0f};
    t.rotation = {0.0f, 0.70710677f, 0.0f, 0.70710677f};
    t.scale    = {2.0f, 2.0f, 0.5f};
    a.set<Transform>(t);
    Camera c;
    c.isPrimary = false; c.projection = ProjectionType::Orthographic;
    c.fov = 72.5f; c.orthoSize = 4.0f; c.nearPlane = 0.25f; c.farPlane = 250.0f;
    c.clearColor[0] = 0.2f; c.clearColor[1] = 0.3f; c.clearColor[2] = 0.4f; c.clearColor[3] = 0.9f;
    a.set<Camera>(c);
    ColourGrading g;
    g.exposureMode = 1; g.toneMapper = 2; g.exposureEV = -1.5f;
    g.aperture = 2.8f; g.shutterSeconds = 0.004f; g.iso = 400.0f;
    g.lutPath = "assets/luts/warm.cube";
    a.set<ColourGrading>(g);
    RigidBody rb;
    rb.bodyType = PhysicsBodyType::Kinematic; rb.shape = PhysicsShape::Capsule;
    rb.mass = 12.0f; rb.restitution = 0.1f; rb.friction = 0.9f; rb.useGravity = false;
    rb.halfExtent = {0.25f, 1.0f, 0.75f}; rb.radius = 0.4f; rb.halfHeight = 0.8f;
    a.set<RigidBody>(rb);
    ScriptComponent sc; sc.scriptPath = "scripts/door.lua";
    a.set<ScriptComponent>(sc);
    CharacterController cc;
    cc.radius = 0.35f; cc.height = 1.75f; cc.maxSlopeDeg = 40.0f;
    cc.stepHeight = 0.25f; cc.mass = 80.0f; cc.gravityScale = 1.5f;
    a.set<CharacterController>(cc);
    Light l;
    l.type = LightType::Spot; l.color = {1.0f, 0.5f, 0.25f}; l.intensity = 7.0f;
    l.range = 30.0f; l.spotInner = 10.0f; l.spotOuter = 20.0f;
    l.castShadows = true; l.useTemperature = true; l.temperatureK = 3200.0f;
    a.set<Light>(l);
    a.set<Health>({40.0f, 120.0f});

    // A child, so parentId is in the fixture too.
    flecs::entity b = w.entity();
    b.set<EntityId>({1002});
    b.set<Name>({"Child"});
    b.set<Transform>(Transform{});
    b.add(flecs::ChildOf, a);
    b.set<SkinnedMesh>(SkinnedMesh{});
    Animator an;
    an.clipIndex = 3; an.fade = 0.5f; an.time = 1.25f; an.speed = 0.75f;
    an.playing = true; an.looping = false;
    if (mode == SerdeMode::Memory) {
        an.clip.id = 7; an.clipPath = "/abs/clips/walk.cclip";
        SkinnedMesh sm; sm.skeleton.id = 5;
        b.set<SkinnedMesh>(sm);
        MeshHandle mh; mh.id = 9;
        MeshRenderer mr{mh}; mr.materialOverride.id = 4;
        b.set<MeshRenderer>(mr);
        LodMesh lm; lm.count = 2;
        lm.mesh[0].id = 10; lm.mesh[1].id = 11;
        lm.coarsenBelow[0] = 0.4f; lm.coarsenBelow[1] = 0.1f;
        b.set<LodMesh>(lm);
    }
    b.set<Animator>(an);
}

std::string saveAll(flecs::world& w, const SerdeContext& ctx) {
    json scene;
    scene["version"]  = 1;
    scene["entities"] = json::array();
    for (uint64_t id : {1001ull, 1002ull})
        scene["entities"].push_back(saveEntity(findById(w, id), ctx));
    return scene.dump(2) + "\n";
}

// Load every entity of `text` into a fresh world, save them back in file order.
std::string roundTrip(const std::string& text, SerdeMode mode,
                      const SchemaRegistry* schemas = nullptr) {
    flecs::world w = makeWorld();
    SerdeContext ctx; ctx.mode = mode; ctx.schemas = schemas;
    const json scene = json::parse(text);
    json out;
    out["version"]  = scene["version"];
    out["entities"] = json::array();
    std::vector<flecs::entity> made;
    std::vector<SerdeContext::EntityRef> refs;   // as the scene loader does
    ctx.entityRefs = &refs;
    for (const auto& je : scene["entities"])
        made.push_back(createEntity(w, je, ctx, IdPolicy::Preserve));
    resolveEntityRefs(w, ctx);
    // Parents after every entity exists, as the scene loader does.
    for (std::size_t i = 0; i < made.size(); ++i) {
        const auto& je = scene["entities"][i];
        if (je.contains("parentId"))
            made[i].add(flecs::ChildOf, findById(w, je["parentId"].get<uint64_t>()));
    }
    for (flecs::entity e : made) out["entities"].push_back(saveEntity(e, ctx));
    return out.dump(2) + "\n";
}

// The disk fixture's mesh: authored by hand, because a Disk save writes the
// mesh through the asset database. With no loader in this process it stays
// unresolved, and an unresolved mesh must be written back exactly as authored.
const json kAuthoredMesh = {
    {"asset", "6f1c2a8e-0b44-4c55-9d3e-2a1b7c9d0e11"},
    {"path", "assets/models/crate.glb"},
    {"cookedPath", "meshs/6f1c2a8e-0b44-4c55-9d3e-2a1b7c9d0e11.cooked"},
    {"material", "Crate_Wood"},
};

std::string writeDiskFixture() {
    flecs::world w = makeWorld();
    populate(w, SerdeMode::Disk);
    SerdeContext ctx; ctx.mode = SerdeMode::Disk;
    // Put the authored mesh on the child the way a load with no loader leaves it.
    loadMesh(findById(w, 1002), kAuthoredMesh, ctx);
    return saveAll(w, ctx);
}

std::string writeMemoryFixture() {
    flecs::world w = makeWorld();
    populate(w, SerdeMode::Memory);
    SerdeContext ctx; ctx.mode = SerdeMode::Memory;
    return saveAll(w, ctx);
}


// ── A component whose meaning changed: the migration test (DR-0034) ─────────
// Revision 0 saved thrust in newtons as "thrust"; revision 1 saves kilonewtons
// as "thrustKN". The rev-0 fixture was written by the rev-0 schema below, and is
// what a scene saved before the bump looks like.
struct ThrusterV0 { float thrust = 0.0f; };
struct ThrusterV1 { float thrustKN = 0.0f; };

SchemaRegistry registryWithThruster(uint32_t revision, bool withMigration = true) {
    SchemaRegistry r;
    registerBuiltinSchemas(r);
    if (revision == 0) {
        r.add(SchemaBuilder<ThrusterV0>("test.Thruster", "thruster")
                  .f32("thrust", &ThrusterV0::thrust).build());
    } else {
        SchemaBuilder<ThrusterV1> b("test.Thruster", "thruster");
        b.revision(revision).f32("thrustKN", &ThrusterV1::thrustKN);
        if (withMigration)
            b.migration(0, [](json& j) {
                if (j.contains("thrust") && j["thrust"].is_number())
                    j["thrustKN"] = j["thrust"].get<double>() / 1000.0;
                j.erase("thrust");
            });
        r.add(b.build());
    }
    return r;
}

std::string writeThrusterFixture() {
    flecs::world w = makeWorld();
    const SchemaRegistry r = registryWithThruster(0);
    SerdeContext ctx; ctx.mode = SerdeMode::Disk; ctx.schemas = &r;
    flecs::entity e = w.entity();
    e.set<EntityId>({2001});
    e.set<Name>({"Engine bell"});
    e.set<Transform>(Transform{});
    e.set<ThrusterV0>({2500.0f});
    json scene;
    scene["version"]  = 1;
    scene["entities"] = json::array({ saveEntity(e, ctx) });
    return scene.dump(2) + "\n";
}

// One entity with a component nothing registered, and one from the future.
const char* kUnknownScene = R"({
  "entities": [
    {
      "camera": {
        "$rev": 9,
        "fov": 10.0,
        "lens": "anamorphic"
      },
      "id": 3001,
      "name": "From elsewhere",
      "sprite": {
        "atlas": "ui/icons.png",
        "frame": 3,
        "tint": [
          1.0,
          0.5,
          0.5,
          1.0
        ]
      },
      "transform": {
        "position": [
          0.0,
          1.0,
          0.0
        ],
        "rotation": [
          0.0,
          0.0,
          0.0,
          1.0
        ],
        "scale": [
          1.0,
          1.0,
          1.0
        ]
      }
    }
  ],
  "version": 1
}
)";

// ── Every field type (the engine API draft's EngFieldType) ──────────────────
struct Probe {
    bool          on    = true;
    int8_t        i8    = -3;
    uint8_t       u8    = 200;
    int16_t       i16   = -30000;
    uint16_t      u16   = 60000;
    int32_t       i32   = -7;
    uint32_t      u32   = 4000000000u;
    int64_t       i64   = -9000000000ll;
    uint64_t      u64   = 18000000000000000000ull;
    float         f     = 0.5f;
    double        d     = 0.1;
    float         v2[2] = {1, 2};
    float         v3[3] = {1, 2, 3};
    float         v4[4] = {1, 2, 3, 4};
    float         q[4]  = {0, 0, 0, 1};
    float         m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::string   s     = "hello";
    flecs::entity_t target = 0;
    AssetRef      a;
};

SchemaRegistry registryWithProbe() {
    SchemaRegistry r;
    registerBuiltinSchemas(r);
    r.add(SchemaBuilder<Probe>("test.Probe", "probe")
        .boolean("on", &Probe::on)
        .integer("i8", &Probe::i8).integer("u8", &Probe::u8)
        .integer("i16", &Probe::i16).integer("u16", &Probe::u16)
        .integer("i32", &Probe::i32).integer("u32", &Probe::u32)
        .integer("i64", &Probe::i64).integer("u64", &Probe::u64)
        .f32("f", &Probe::f).f64("d", &Probe::d)
        .vec2("v2", &Probe::v2).vec3("v3", &Probe::v3).vec4("v4", &Probe::v4)
        .quat("q", &Probe::q).mat4("m", &Probe::m)
        .string("s", &Probe::s)
        .entity("target", &Probe::target)
        .asset("a", &Probe::a)
        .build());
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::strcmp(argv[1], "--write-fixtures") == 0) {
        writeFile(kFixtures + "every_component.disk.json",   writeDiskFixture());
        writeFile(kFixtures + "every_component.memory.json", writeMemoryFixture());
        writeFile(kFixtures + "thruster_rev0.json",          writeThrusterFixture());
        std::printf("wrote the fixtures to %s\n", kFixtures.c_str());
        return 0;
    }
    // A real scene through the serializer, printed: how a change to it is checked
    // against scenes that cannot be fixtures (a game's own, outside the repo).
    if (argc > 2 && std::strcmp(argv[1], "--round-trip") == 0) {
        std::string out = roundTrip(readFile(argv[2]), SerdeMode::Disk);
        out.pop_back();   // a .scene is written without a final newline
        std::fputs(out.c_str(), stdout);
        return 0;
    }
    std::printf("scene_schema_test: the scene format, byte for byte\n");

    const std::string disk   = readFile(kFixtures + "every_component.disk.json");
    const std::string memory = readFile(kFixtures + "every_component.memory.json");
    CHECK(!disk.empty() && !memory.empty(), "both fixtures are present");

    std::printf("\n-- what the serializer writes today equals what it wrote then --\n");
    CHECK(writeDiskFixture() == disk, "a disk save of every component is the fixture");
    CHECK(writeMemoryFixture() == memory, "a memory snapshot of every component is the fixture");

    std::printf("\n-- a saved scene loads and saves back unchanged --\n");
    CHECK(roundTrip(disk, SerdeMode::Disk) == disk, "disk: load then save is byte-identical");
    CHECK(roundTrip(memory, SerdeMode::Memory) == memory, "memory: load then save is byte-identical");


    std::printf("\n-- no GPU or session handle is ever saved to disk --\n");
    for (const char* k : {"handleId", "matOverrideId", "skeletonId", "clipId", "placeholderId"}) {
        const std::string q = std::string("\"") + k + "\"";
        CHECK(disk.find(q) == std::string::npos, "the disk fixture has no %s", k);
    }
    CHECK(memory.find("\"handleId\"") != std::string::npos,
          "while the memory snapshot does (so the check above can see them)");

    std::printf("\n-- a component this engine cannot read is kept, never dropped --\n");
    {
        CHECK(roundTrip(kUnknownScene, SerdeMode::Disk) == kUnknownScene,
              "an unregistered component and a newer revision save back byte-identical (disk)");
        CHECK(roundTrip(kUnknownScene, SerdeMode::Memory) == kUnknownScene,
              "and through the in-memory snapshot (Play, undo)");
        flecs::world w = makeWorld();
        SerdeContext ctx; ctx.mode = SerdeMode::Disk;
        flecs::entity e = createEntity(w, json::parse(kUnknownScene)["entities"][0], ctx,
                                       IdPolicy::Preserve);
        CHECK(!e.has<Camera>(), "a camera saved at a revision this engine lacks is not "
                                "loaded as a default camera");
        CHECK(e.has<Transform>() && e.has<Name>(), "the entity's other components still load");
    }

    std::printf("\n-- old data migrates (DR-0034) --\n");
    {
        const std::string rev0 = readFile(kFixtures + "thruster_rev0.json");
        CHECK(rev0.find("\"thrust\": 2500.0") != std::string::npos
              && rev0.find("$rev") == std::string::npos,
              "the fixture is the revision-0 shape: \"thrust\" in newtons, no revision marker");
        const SchemaRegistry r1 = registryWithThruster(1);
        flecs::world w = makeWorld();
        SerdeContext ctx; ctx.mode = SerdeMode::Disk; ctx.schemas = &r1;
        flecs::entity e = createEntity(w, json::parse(rev0)["entities"][0], ctx,
                                       IdPolicy::Preserve);
        const ThrusterV1* t = e.try_get<ThrusterV1>();
        CHECK(t && t->thrustKN == 2.5f, "revision 0 loads into revision 1 through its "
              "migration: 2500 N is 2.5 kN (%g)", t ? (double)t->thrustKN : -1.0);
        const json saved = saveEntity(e, ctx);
        CHECK(saved["thruster"].value(kRevisionKey, 0u) == 1u
              && !saved["thruster"].contains("thrust"),
              "and saves back as revision 1, with the revision marked");
        CHECK(roundTrip(roundTrip(rev0, SerdeMode::Disk, &r1), SerdeMode::Disk, &r1)
              == roundTrip(rev0, SerdeMode::Disk, &r1),
              "revision 1 then round-trips unchanged");

        const SchemaRegistry r2 = registryWithThruster(2);   // 1->2 never written
        flecs::world w2 = makeWorld();
        SerdeContext c2; c2.mode = SerdeMode::Disk; c2.schemas = &r2;
        flecs::entity e2 = createEntity(w2, json::parse(rev0)["entities"][0], c2,
                                        IdPolicy::Preserve);
        CHECK(!e2.has<ThrusterV1>(), "a missing migration does not load the component");
        CHECK(roundTrip(rev0, SerdeMode::Disk, &r2) == rev0,
              "it is kept and saved back exactly as it was");
    }

    std::printf("\n-- the registry refuses a collision by name --\n");
    {
        SchemaRegistry r;
        registerBuiltinSchemas(r);
        const std::size_t n = r.all().size();
        CHECK(r.add(SchemaBuilder<Camera>("core.Camera", "camera").build()),
              "registering the same schema again is a no-op");
        CHECK(!r.add(SchemaBuilder<ThrusterV0>("test.Other", "camera").build()),
              "another component under a taken key is refused");
        CHECK(!r.add(SchemaBuilder<Camera>("core.Camera", "camera").revision(3).build()),
              "the same component at another revision is refused");
        CHECK(r.all().size() == n, "and nothing was added (%zu)", r.all().size());
        CHECK(r.byName("core.Transform") && r.byKey("transform") == r.byName("core.Transform"),
              "components are found by namespaced name and by key");
    }


    std::printf("\n-- every field type, with defaults and types stated by the schema --\n");
    {
        const SchemaRegistry r = registryWithProbe();
        const ComponentSchema* ps = r.byName("test.Probe");
        CHECK(ps && ps->fields.size() == 19, "the schema lists its 19 fields (%zu)",
              ps ? ps->fields.size() : 0);
        std::string types;
        if (ps) for (const FieldInfo& f : ps->fields) types += std::string(fieldTypeName(f.type)) + " ";
        CHECK(types == "bool i8 u8 i16 u16 i32 u32 i64 u64 f32 f64 vec2 vec3 vec4 quat mat4 "
                       "string entity asset ", "each with its type: %s", types.c_str());
        CHECK(ps && ps->defaults.value("u64", 0ull) == 18000000000000000000ull
              && ps->defaults.value("s", std::string{}) == "hello"
              && ps->defaults["m"].size() == 16 && ps->defaults.value("target", 1ull) == 0ull,
              "and its defaults are the saved form of a default component");
        const ComponentSchema* cam = schemas().byName("core.Camera");
        CHECK(cam && cam->defaults.value("fov", 0.0) == 60.0 && cam->fields.size() == 7,
              "the engine's own schemas state theirs too (camera: fov 60, 7 fields)");

        // Two entities, the reference pointing FORWARD in file order, values not
        // the defaults, through disk and memory.
        flecs::world w = makeWorld();
        SerdeContext ctx; ctx.mode = SerdeMode::Disk; ctx.schemas = &r;
        flecs::entity a = w.entity(), b = w.entity();
        a.set<EntityId>({4001}); a.set<Name>({"A"}); a.set<Transform>(Transform{});
        b.set<EntityId>({4002}); b.set<Name>({"B"}); b.set<Transform>(Transform{});
        Probe p;
        p.on = false; p.i8 = -128; p.u8 = 255; p.i16 = 32767; p.u16 = 1; p.i32 = INT32_MIN;
        p.u32 = 7; p.i64 = INT64_MIN; p.u64 = UINT64_MAX; p.f = -2.5f; p.d = 1e300;
        p.v2[1] = 9; p.v3[2] = 9; p.v4[3] = 9; p.q[0] = 1; p.m[15] = 2;
        p.s = "world"; p.target = b.id(); p.a = {"0b6f-uuid", "assets/sfx/door.wav"};
        a.set<Probe>(p);
        json scene;
        scene["version"]  = 1;
        scene["entities"] = json::array({ saveEntity(a, ctx), saveEntity(b, ctx) });
        const std::string text = scene.dump(2) + "\n";
        CHECK(scene["entities"][0]["probe"].value("target", 0ull) == 4002ull,
              "an entity field is saved as the stable id of the entity it names");
        for (SerdeMode mode : {SerdeMode::Disk, SerdeMode::Memory}) {
            CHECK(roundTrip(text, mode, &r) == text, "%s: every field type round-trips "
                  "byte-identically", mode == SerdeMode::Disk ? "disk" : "memory");
        }
        // Loaded through the scene loader's own path: a list resolved at the end.
        flecs::world w2 = makeWorld();
        SerdeContext c2; c2.mode = SerdeMode::Disk; c2.schemas = &r;
        std::vector<SerdeContext::EntityRef> refs; c2.entityRefs = &refs;
        flecs::entity la = createEntity(w2, scene["entities"][0], c2, IdPolicy::Preserve);
        flecs::entity lb = createEntity(w2, scene["entities"][1], c2, IdPolicy::Preserve);
        resolveEntityRefs(w2, c2);
        const Probe* lp = la.try_get<Probe>();
        CHECK(lp && lp->target == lb.id(), "a reference to an entity later in the file "
              "resolves to that entity in the new world");
        CHECK(lp && lp->i8 == -128 && lp->u8 == 255 && lp->i16 == 32767 && lp->i32 == INT32_MIN
              && lp->i64 == INT64_MIN && lp->u64 == UINT64_MAX && lp->d == 1e300
              && lp->m[15] == 2 && lp->s == "world" && lp->a.path == "assets/sfx/door.wav"
              && lp->a.uuid == "0b6f-uuid" && !lp->on,
              "and every value, at each type's limits, reads back exactly");

        // Out of range or the wrong type keeps the default; nothing throws.
        json bad = scene["entities"][0];
        bad["probe"]["i8"] = 300; bad["probe"]["u16"] = -1; bad["probe"]["u64"] = -5;
        bad["probe"]["d"] = "x"; bad["probe"]["a"] = 7; bad["probe"]["target"] = 999999;
        bad["id"] = 4009;
        flecs::world w3 = makeWorld();
        SerdeContext c3; c3.mode = SerdeMode::Disk; c3.schemas = &r;
        flecs::entity e3 = createEntity(w3, bad, c3, IdPolicy::Preserve);
        const Probe* bp = e3.try_get<Probe>();
        const Probe def;
        CHECK(bp && bp->i8 == def.i8 && bp->u16 == def.u16 && bp->u64 == def.u64
              && bp->d == def.d && bp->a.empty() && bp->target == 0 && bp->u8 == 255,
              "an out-of-range, wrong-typed or dangling field keeps its default, and the "
              "rest of the component loads");
    }


    std::printf("\n-- what a component with nothing saved reads as --\n");
    {
        // The format's defaults are a promise to every file already written. A
        // rigid body with no "bodyType" has always been KINEMATIC (the old table
        // read 1, the cooker wrote 1, scene-format.md says so); RigidBody's own
        // default is dynamic, and the first registry took that. Pinned here.
        flecs::world w = makeWorld();
        SerdeContext ctx; ctx.mode = SerdeMode::Disk;
        const json je = json::parse(R"({"id": 5001, "name": "Body", "rigidBody": {},
                                        "camera": {}, "light": {}, "characterController": {}})");
        flecs::entity e = createEntity(w, je, ctx, IdPolicy::Preserve);
        const RigidBody* rb = e.try_get<RigidBody>();
        CHECK(rb && rb->bodyType == PhysicsBodyType::Kinematic,
              "\"rigidBody\": {} is kinematic, as it has always been");
        CHECK(schemas().byKey("rigidBody")->defaults.value("bodyType", -1) == 1,
              "and the schema's stated default says so (bodyType 1)");
        const Camera* c = e.try_get<Camera>(); const Light* l = e.try_get<Light>();
        const CharacterController* cc = e.try_get<CharacterController>();
        CHECK(c && c->fov == 60.0f && c->isPrimary && c->farPlane == 1000.0f
              && l && l->intensity == 3.0f && l->range == 15.0f && !l->castShadows
              && cc && cc->height == 1.8f && cc->mass == 70.0f,
              "camera, light and character controller read their documented defaults");
    }


    std::printf("\n-- a cooked skinned mesh gets its skeleton and its clip (WO-091) --\n");
    {
        // Before WO-091 the cooked path asked for no skin, so a skinned mesh
        // loaded from a cooked file kept an invalid skeleton and never animated
        // in the editor. A fake host: mesh 3, skeleton 5, embedded clips 7 and 8,
        // and a standalone clip that binds as 42.
        SceneAssets fake;
        fake.loadCookedMesh = [](const char*, std::vector<MeshHandle>*, StreamedMesh* skin) {
            MeshHandle m; m.id = 3;
            if (skin) {
                skin->skeleton.id = 5;
                AnimClipHandle c7, c8; c7.id = 7; c8.id = 8;
                skin->clips = {c7, c8};
            }
            return m;
        };
        fake.bindClip = [](const std::string& path, SkeletonHandle sk) {
            AnimClipHandle h; h.id = (path == "clips/walk.anim" && sk.id == 5) ? 42 : 0;
            return h;
        };
        flecs::world w = makeWorld();
        SerdeContext ctx; ctx.mode = SerdeMode::Disk; ctx.sceneAssets = &fake;
        const json authored = json::parse(R"({"id": 6001, "name": "Authored",
            "meshRenderer": {"cookedPath": "meshs/hero.cooked"}, "skinnedMesh": {},
            "animator": {"clipIndex": 1, "playing": false, "speed": 0.5}})");
        const json bare = json::parse(R"({"id": 6002, "name": "Bare",
            "meshRenderer": {"cookedPath": "meshs/hero.cooked"}})");
        const json standalone = json::parse(R"({"id": 6003, "name": "Standalone",
            "meshRenderer": {"cookedPath": "meshs/hero.cooked"},
            "animator": {"path": "clips/walk.anim", "playing": true}})");
        flecs::entity e1 = createEntity(w, authored, ctx, IdPolicy::Preserve);
        flecs::entity e2 = createEntity(w, bare, ctx, IdPolicy::Preserve);
        flecs::entity e3 = createEntity(w, standalone, ctx, IdPolicy::Preserve);
        const SkinnedMesh* sm = e1.try_get<SkinnedMesh>();
        const Animator* a1 = e1.try_get<Animator>();
        CHECK(sm && sm->skeleton.id == 5, "the skeleton is wired, after the saved "
              "\"skinnedMesh\" marker that would otherwise leave it invalid");
        CHECK(a1 && a1->clip.id == 8 && !a1->playing && a1->speed == 0.5f,
              "the authored Animator keeps its fields and gets embedded clip 1 (id %u)",
              a1 ? a1->clip.id : 0);
        const Animator* a2 = e2.try_get<Animator>();
        CHECK(a2 && a2->clip.id == 7 && a2->playing,
              "a skinned mesh with no Animator plays its first clip, as the editor shows");
        const Animator* a3 = e3.try_get<Animator>();
        CHECK(a3 && a3->clip.id == 42 && a3->clipPath == "clips/walk.anim",
              "a standalone clip binds by its path, kept as authored though it does not "
              "resolve here");
        CHECK(!e1.has<PendingSkin>() && !e2.has<PendingSkin>(), "nothing is left pending");
    }

    std::printf("\n-- extensions: the namespaces a world has --\n");
    {
        flecs::world w = makeWorld();
        const std::set<std::string> have = extensions::available(w, schemas());
        CHECK(have.count("core") && have.count("combat") && !have.count("flecs")
              && !have.count("bx"),
              "core (the registry) and combat (a kit's flecs component), not the engine's "
              "own bookkeeping (flecs, bx)");
        const extensions::Missing m = extensions::check(w, schemas(), {"combat", "apollo"},
                                                        {"voxel", "apollo"});
        CHECK(m.required.size() == 1 && m.required[0] == "apollo",
              "a required namespace nothing provides is missing (apollo)");
        CHECK(m.used.size() == 1 && m.used[0] == "voxel",
              "a used one is reported apart, and one also required is not reported twice");
        CHECK(extensions::namespaceOf("apollo.Thruster") == "apollo"
              && extensions::namespaceOf("combat::Health") == "combat"
              && extensions::namespaceOf("Transform").empty(),
              "both spellings of a namespace are read, and none is none");
    }

    if (g_failures) {
        std::printf("\nscene_schema_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nscene_schema_test: all checks passed\n");
    return 0;
}
