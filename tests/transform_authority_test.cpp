// ── transform_authority_test — the backstop, and its blind spots ────────────
//
// Stage 3b. The watcher is deliberately a BACKSTOP, not the mechanism: stages
// 1–3a removed the *reasons* gameplay had to write a physics-owned field, and
// this catches what happens anyway.
//
// A watcher is only worth having if BOTH halves hold, so both are tested and
// the second is the one that decides whether anyone keeps it switched on:
//
//   POSITIVE CONTROL   a plugin writing a dynamic body's Transform in onUpdate
//                      is reported, with the entity, the FIELD and the phase.
//   NO FALSE POSITIVES a legal scale write, a legal character rotation, a
//                      kinematic move and physics' own write-back are all
//                      silent. A watcher that fires on correct code gets muted,
//                      and a muted watcher is worse than none because it looks
//                      like coverage.
//
// It also pins the two SPAWN REFUSALS and the singular-parent clamp, because
// all three are places the engine now says no where it used to half-work.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include <flecs.h>

#include "components/character_controller.h"
#include "components/name.h"
#include "components/rigid_body.h"
#include "core/transform.h"
#include "core/transform_utils.h"
#include "plugins/jolt_plugin.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"
#include "runtime/transform_authority.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

// ── A plugin that misbehaves on request ─────────────────────────────────────
// The positive control. Nothing in the tree writes a physics-owned field any
// more, which is the point of stages 1-3a — so the violation has to be staged,
// or this test would be asserting that a watcher watching nothing reports
// nothing.
class OffenderPlugin final : public IEnginePlugin {
public:
    enum class What { Nothing, DynamicPosition, LegalScale, CharacterRotation,
                      KinematicPosition, TransientPosition };

    const char* name()    const override { return "Offender"; }
    const char* version() const override { return "1.0.0"; }
    void onAttach(RuntimeContext&) override {}
    void onDetach() override {}
    void onSimulationStart(flecs::world&) override {}
    void onSimulationStop() override {}

    void onUpdate(flecs::world& w, float) override {
        if (!target) return;
        flecs::entity e = w.entity(target);
        if (!e.is_alive()) return;
        Transform& t = e.get_mut<Transform>();
        switch (what) {
        case What::Nothing: break;
        case What::DynamicPosition:
        case What::KinematicPosition:
            t.position.x += 1.0f;
            break;
        case What::LegalScale:
            // Scale is never backend-owned: spawnBody sizes shapes from
            // RigidBody's half-extents, not from Transform.scale.
            t.scale.x = 2.0f + (float)(++n) * 0.01f;
            break;
        case What::CharacterRotation:
            // Gameplay-owned even though physics READS it — pushEcsToPhysics
            // pushes it into CharacterVirtual and never reads it back.
            t.rotation = { 0.0f, 0.3826834f, 0.0f, 0.9238795f };
            break;
        case What::TransientPosition: {
            // Written and put back INSIDE one phase. The watcher compares
            // final state, so it cannot see this — a recorded limit, not a
            // bug, and the reason the header refuses to claim such a write
            // "cannot alter the simulation's result".
            const bx::Vec3 was = t.position;
            t.position = { 999.0f, 999.0f, 999.0f };
            t.position = was;
            break;
        }
        }
    }

    flecs::entity_t target = 0;
    What            what   = What::Nothing;
    int             n      = 0;
};

static const std::filesystem::path& hermeticRoot() {
    static const std::filesystem::path p = [] {
        std::filesystem::path d = std::filesystem::temp_directory_path()
                                / "engine_transform_authority_root";
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return p;
}

struct Scene {
    flecs::entity dynamic, kinematic, character;
};

// One session: build a world, run `ticks` fixed steps with the offender doing
// `what` to `pick(scene)`, and report the watcher's verdict.
struct Verdict {
    size_t violations = 0;
    std::string field, phase;
    uint64_t entity = 0;
    float finalX = 0.0f;
};

template <typename Pick>
static Verdict run(OffenderPlugin::What what, Pick pick, int ticks = 6) {
    EngineConfig cfg;
    cfg.openAssetDatabase = false;
    cfg.autoDetectProject = false;
    cfg.defaultScene      = false;
    cfg.projectRoot       = hermeticRoot();

    EngineRuntime engine;
    Verdict v;
    if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) return v;

    engine.plugins().add(std::make_shared<JoltPlugin>());
    auto offender = std::make_shared<OffenderPlugin>();
    engine.plugins().add(offender);
    engine.attachPlugins();

    flecs::world& w = engine.simWorld();
    RigidBody floor{}; floor.bodyType = PhysicsBodyType::Static;
    floor.halfExtent = { 50.0f, 0.5f, 50.0f };
    w.entity().set<Transform>({{0.f,-0.5f,0.f},{0,0,0,1},{1,1,1}})
              .set<Name>({"floor"}).set<RigidBody>(floor);

    Scene s{};
    RigidBody dyn{}; dyn.bodyType = PhysicsBodyType::Dynamic;
    dyn.halfExtent = {0.5f,0.5f,0.5f}; dyn.mass = 1.0f;
    s.dynamic = w.entity().set<Transform>({{0.f,4.f,0.f},{0,0,0,1},{1,1,1}})
                          .set<Name>({"crate"}).set<RigidBody>(dyn);

    RigidBody kin{}; kin.bodyType = PhysicsBodyType::Kinematic;
    kin.halfExtent = {1.f,0.25f,1.f};
    s.kinematic = w.entity().set<Transform>({{6.f,1.f,0.f},{0,0,0,1},{1,1,1}})
                            .set<Name>({"platform"}).set<RigidBody>(kin);

    CharacterController cc{};
    s.character = w.entity().set<Transform>({{-6.f,1.f,0.f},{0,0,0,1},{1,1,1}})
                            .set<Name>({"hero"}).set<CharacterController>(cc);

    offender->target = pick(s).id();
    offender->what   = what;

    engine.startSimulation(EngineRuntime::SimMode::InPlace);
    for (int i = 0; i < ticks; ++i) engine.tick(1.0f / 60.0f);

    const auto& watcher = engine.transformAuthority();
    v.violations = watcher.violations();
    if (!watcher.recent().empty()) {
        v.field  = authority::fieldName(watcher.recent().front().fields);
        v.phase  = watcher.recent().front().phase;
        v.entity = watcher.recent().front().entity;
    }
    flecs::entity t = w.entity(offender->target);
    if (t.is_alive() && t.has<Transform>()) v.finalX = t.get<Transform>().position.x;

    engine.stopSimulation();
    engine.shutdown();
    return v;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("transform_authority_test — the backstop, and its blind spots\n");

    CHECK(authority::Watcher::active,
          "the watcher is compiled in for this build — otherwise every "
          "assertion below would pass by being switched off");

    // ── 1. The rule itself, with no simulation running ─────────────────────
    // The table is the only part worth arguing about; everything else in the
    // watcher is bookkeeping. Checked directly so a change to it fails HERE,
    // where the failure names the row, rather than as a mysterious count.
    {
        std::printf("\n-- 1. the authority table --\n");
        flecs::world w;
        RigidBody rb{};
        rb.bodyType = PhysicsBodyType::Dynamic;
        flecs::entity d = w.entity().set<RigidBody>(rb);
        rb.bodyType = PhysicsBodyType::Static;
        flecs::entity st = w.entity().set<RigidBody>(rb);
        rb.bodyType = PhysicsBodyType::Kinematic;
        flecs::entity k = w.entity().set<RigidBody>(rb);
        flecs::entity ch = w.entity().set<CharacterController>({});
        flecs::entity plain = w.entity().set<Transform>({});

        using namespace authority;
        CHECK(backendOwned(d) == (Position | Rotation), "Dynamic: physics owns pose");
        CHECK(backendOwned(st) == (Position | Rotation),
              "Static: physics owns pose — nothing may move it but a Teleport");
        CHECK(backendOwned(k) == None,
              "Kinematic: NOTHING is backend-owned. The plan's table said "
              "physics owned it; BUG-0058 showed that is backwards — a "
              "kinematic body's pose is by definition gameplay's");
        CHECK(backendOwned(ch) == Position,
              "Character: position only. Rotation is pushed INTO the "
              "controller and never read back, so reading is not owning");
        CHECK(backendOwned(plain) == None, "a plain entity is gameplay's");
    }

    // ── 2. Positive control ────────────────────────────────────────────────
    {
        std::printf("\n-- 2. positive control --\n");
        Verdict v = run(OffenderPlugin::What::DynamicPosition,
                        [](const Scene& s) { return s.dynamic; });
        CHECK(v.violations > 0,
              "a plugin writing a dynamic body's position in onUpdate IS "
              "reported (%zu times)", v.violations);
        CHECK(v.field == "position",
              "and the FIELD is named (%s) — a per-entity digest could not "
              "have said which", v.field.c_str());
        CHECK(v.phase == "onUpdate",
              "and the PHASE is named (%s), so the report points at the writer",
              v.phase.c_str());
        // The write is dropped, which is what makes it worth reporting.
        CHECK(std::fabs(v.finalX) < 3.0f,
              "and the write was in fact discarded (x=%.3f after six ticks of "
              "adding 1.0 per tick)", (double)v.finalX);
    }

    // ── 3. No false positives ──────────────────────────────────────────────
    // The half that decides whether anyone leaves this switched on.
    {
        std::printf("\n-- 3. no false positives --\n");
        Verdict scale = run(OffenderPlugin::What::LegalScale,
                            [](const Scene& s) { return s.dynamic; });
        CHECK(scale.violations == 0,
              "writing SCALE on a dynamic body is legal and silent (%zu) — "
              "spawnBody sizes shapes from RigidBody's half-extents, so scale "
              "is gameplay's, and an earlier per-entity digest would have "
              "reported this correct code",
              scale.violations);

        Verdict rot = run(OffenderPlugin::What::CharacterRotation,
                          [](const Scene& s) { return s.character; });
        CHECK(rot.violations == 0,
              "writing a CHARACTER'S ROTATION is legal and silent (%zu)",
              rot.violations);

        Verdict kin = run(OffenderPlugin::What::KinematicPosition,
                          [](const Scene& s) { return s.kinematic; });
        CHECK(kin.violations == 0,
              "moving a KINEMATIC body is legal and silent (%zu) — it is how "
              "a moving platform is built", kin.violations);
        CHECK(kin.finalX > 8.0f,
              "and it actually moved (x=%.2f), so the silence is not just an "
              "entity nobody touched", (double)kin.finalX);

        Verdict quiet = run(OffenderPlugin::What::Nothing,
                            [](const Scene& s) { return s.dynamic; });
        CHECK(quiet.violations == 0,
              "and a session where physics is the ONLY writer reports nothing "
              "(%zu) — the re-baseline around the physics step works, or "
              "every falling body would be a violation every tick",
              quiet.violations);
    }

    // ── 4. The blind spot, recorded rather than discovered later ───────────
    {
        std::printf("\n-- 4. what it cannot see --\n");
        Verdict t = run(OffenderPlugin::What::TransientPosition,
                        [](const Scene& s) { return s.dynamic; });
        CHECK(t.violations == 0,
              "a write UNDONE within one phase is NOT detected (%zu). This is "
              "a limit, not a pass: another system reading Transform mid-phase "
              "could branch on the transient value. The watcher sees "
              "final-state ownership; the determinism gate sees observable "
              "divergence; NEITHER sees this",
              t.violations);
    }

    // ── 5. Spawns that are refused rather than half-working ────────────────
    {
        std::printf("\n-- 5. refused spawns --\n");
        flecs::world w;
        AssetRegistry assets; TextureRegistry tex; MaterialRegistry mat;
        ProjectContext proj; ImporterRegistry imp;
        RuntimeContext ctx{ w, assets, tex, mat, proj, imp };

        RigidBody floor{}; floor.bodyType = PhysicsBodyType::Static;
        floor.halfExtent = {50.f,0.5f,50.f};
        w.entity().set<Transform>({{0.f,-0.5f,0.f},{0,0,0,1},{1,1,1}})
                  .set<RigidBody>(floor);

        flecs::entity parent =
            w.entity().set<Transform>({{0.f,0.f,0.f},{0,0,0,1},{1,1,1}});
        RigidBody dyn{}; dyn.bodyType = PhysicsBodyType::Dynamic;
        dyn.halfExtent = {0.5f,0.5f,0.5f};
        flecs::entity child =
            w.entity().set<Transform>({{10.f,3.f,0.f},{0,0,0,1},{1,1,1}})
                      .set<RigidBody>(dyn);
        child.child_of(parent);

        // Both components on one entity: the controller must win.
        flecs::entity both =
            w.entity().set<Transform>({{20.f,3.f,0.f},{0,0,0,1},{1,1,1}})
                      .set<RigidBody>(dyn)
                      .set<CharacterController>({});

        JoltPlugin jolt;
        jolt.onAttach(ctx);
        jolt.onSimulationStart(w);
        for (int i = 0; i < 4; ++i) jolt.onPhysicsStep(w, 1.0f/60.0f);

        // A refused body does not fall: physics is not simulating it, and the
        // entity stays exactly where gameplay put it.
        const Transform& ct = child.get<Transform>();
        CHECK(ct.position.y == 3.0f,
              "a RigidBody on a CHILD entity is refused (y=%.3f, unmoved) — "
              "while parented, its world pose would be decided by the parent "
              "and by physics at once, with no way to detect the disagreement",
              (double)ct.position.y);

        float q[4];
        CHECK(jolt.characterRotation(both.id(), q),
              "an entity with BOTH components gets its CharacterController");
        const Transform& bt = both.get<Transform>();
        CHECK(bt.position.y < 2.99f,
              "and it is simulated as a character (y=%.3f) rather than by two "
              "write-backs fighting over one Transform in query order",
              (double)bt.position.y);

        jolt.onSimulationStop();
        jolt.onDetach();
    }

    // ── 6. A singular parent is clamped, not inverted to identity ──────────
    // Four sites hand-rolled this and all four ignored safeInvert's bool. On a
    // singular parent they used IDENTITY as the inverse, which does not mean
    // "no parent" — it means the child's local pose is overwritten with its
    // WORLD one, so it jumps by the parent's entire pose, once, silently.
    {
        std::printf("\n-- 6. the singular parent --\n");
        float parentWorld[16];
        bx::mtxSRT(parentWorld, 0.0f, 1.0f, 1.0f, 0, 0, 0, 100.0f, 0.0f, 0.0f);
        float world[16]; bx::mtxIdentity(world);
        world[12] = 105.0f; world[13] = 0.0f; world[14] = 0.0f;

        const uint64_t before = g_singularParentClamps;
        float local[16];
        const bool clean = worldToLocalMatrix(local, world, parentWorld);
        CHECK(!clean, "a parent with a zero X scale is reported as clamped");
        CHECK(g_singularParentClamps == before + 1, "and counted");
        CHECK(std::fabs(local[12] - 105.0f) > 1.0f,
              "the local X is NOT the world X (%.3f) — identity-as-inverse "
              "would have handed back 105 and moved the child 100 units",
              (double)local[12]);

        // The healthy case must still be exact.
        float healthy[16];
        bx::mtxSRT(healthy, 1.0f, 1.0f, 1.0f, 0, 0, 0, 100.0f, 0.0f, 0.0f);
        CHECK(worldToLocalMatrix(local, world, healthy),
              "an invertible parent reports clean");
        CHECK(std::fabs(local[12] - 5.0f) < 1e-4f,
              "and gives the right local (%.4f, want 5)", (double)local[12]);
    }

    if (g_failures) {
        std::printf("\ntransform_authority_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\ntransform_authority_test: ALL PASS\n");
    return 0;
}
