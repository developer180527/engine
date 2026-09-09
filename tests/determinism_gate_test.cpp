// ── determinism_gate_test — is this simulation reproducible, and where not ───
//
// **THIS TEST IS EXPECTED TO REPORT DIVERGENCES.** It is an instrument, not a
// pass/fail assertion about a finished property. It runs in a NON-GATING ctest
// lane (`determinism`) precisely so it can tell the truth on day one; tiers are
// promoted into the gating `unit` lane as their causes are closed.
//
// ── WHY IT EXISTS ───────────────────────────────────────────────────────────
// A simulation that is not reproducible cannot have BEHAVIOURAL regression
// tests, only structural ones. Today the sim is covered by "does the query
// follow the world" and by nothing that asserts "these inputs produce this
// outcome". docs/plans/automated-testing-soak-fuzz-plan.md 6.3 specified this
// lane and observed that "right now nothing in the engine would tell you it is
// broken". This is that lane.
//
// ── WHAT IT MEASURES: ECS-OBSERVABLE DETERMINISM ────────────────────────────
// It hashes classified ECS component state (runtime/sim_hash.h). It does NOT
// hash JPH::PhysicsSystem, lua_State, AnimatorSystem::m_contexts, or kit
// members. **A green tier means "nothing that reaches a component diverged" —
// it does NOT mean the simulation is bit-identical.** Divergence confined to
// external state is invisible here. That window is real and is stated rather
// than discovered later.
//
// ── HOW IT LOCALISES ────────────────────────────────────────────────────────
// Plugins are HOST-registered, not runtime-registered (src/plugins/
// stock_plugins.h is called by hosts; tests/stress_physics.cpp constructs
// JoltPlugin directly). So this boots a real EngineRuntime with ZERO plugins
// and adds them one tier at a time — a divergence is attributed to a subsystem
// instead of reported as "the world differs".
//
// ── TWO COMPARISONS ─────────────────────────────────────────────────────────
//   A vs A'  two sequential runs, same process. CHEAP SAME-PROCESS
//            reproducibility. It does NOT cover process-level variation: ASLR
//            context, allocator history and global state are shared between
//            the two runs. A cross-process variant is future work.
//   A vs B   1 frame/tick vs 2 frames/tick, same total simulated time.
//            Catches RENDER-RATE COUPLING. The more valuable of the two: A/A'
//            finds implementation accidents, A/B finds architectural coupling
//            that constrains every gameplay system written from here on.
//
// ── DRIVING THE ENGINE, and this cost a review pass to get right ────────────
// The harness calls engine.tick(dt) and NOT tickSimulation(dt). In InPlace
// mode the animator advances at runtime_sim.cpp tickSystems() —
// `if (!m_gameWorld) m_animatorSystem.tick(dt);` — which tickSimulation never
// reaches. An earlier draft drove tickSimulation directly and would have made
// the animator tier pass for the wrong reason.
//
// It also does NOT copy the existing pattern in sim_world_test.cpp:66 and
// soak_engine.cpp:108, which call `engine.tick(dt); engine.tickSimulation(dt);`
// — tick(float) at runtime_frame.cpp:164 ALREADY calls tickSimulation, so those
// loops advance the accumulator by 2x dt per iteration.
//
// ── HERMETIC BY CONSTRUCTION, and it was not at first ───────────────────────
// The world is built in code and cfg.projectRoot points at an empty temp
// directory. Both matter. Leaving projectRoot EMPTY is not neutral:
// LuaScriptPlugin resolves `m_projectRoot / "scripts" / "autorun"`, which with
// an empty root is a RELATIVE path against the working directory — so run from
// the repo root the scripting tier picked up ./scripts/autorun/*.lua and
// executed them, in `fs::directory_iterator` order. A determinism gate whose
// result depends on where it was launched from is not one.
//
// ── INPUT IS DRIVEN, as of stage 5 (2026-09-09) ─────────────────────────────
// This comment used to say no tier reads input, that it was deliberate for v1,
// and that "the moment a tier reads input, ReplaySource is how to make it
// deterministic". The `input` tier is that tier and ReplaySource is how.
//
// It was not possible earlier, and the reason is worth keeping: a controller
// latched the mouse in onFrame, at RENDER rate, and fed the resulting heading
// into movement — so the A/B comparison (1 vs 2 frames per tick) would have
// diverged by construction, on the input rather than on anything this gate was
// built to find. Stage 5 samples intent once per TICK inside the fixed step
// (runtime/sim_intent.h), which makes the A/B row for this tier read as **the
// same mouse motion at two frame rates must simulate identically** — measured
// through the intent sample, the contribution fold, EntityId resolution, the
// character controller and Jolt.
//
// InputManager::beginTick is still called with hid::nowNs(), a wall clock, and
// that is still a finding reported below: the events this tier feeds carry
// small monotonic stamps, so they always fold on the tick they arrive in and
// the clock cannot decide anything. A stream whose stamps straddled a real
// tick boundary would be a different matter.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>

#include "animation/clip_registry.h"
#include "animation/ozz_bridge.h"
#include "animation/skeleton_registry.h"
#include "components/animator.h"
#include "components/skinned_mesh.h"
#include "components/character_controller.h"
#include "components/entity_id.h"
#include "components/name.h"
#include "components/rigid_body.h"
#include "components/spinner.h"
#include "core/transform.h"
#include "plugins/jolt_plugin.h"
#include "plugins/lua_script_plugin.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"
#include "components/meta_registry.h"
#include "runtime/sim_classification.h"
#include "runtime/sim_hash.h"
#include "runtime/sim_intent.h"
#include "runtime/input/input_manager.h"
#include "runtime/input/input_sources.h"

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// ════════════════════════════════════════════════════════════════════════════
// Section 1 — the hashing primitives, tested on their own
// ════════════════════════════════════════════════════════════════════════════
// A gate is only as good as its digest, and a digest nobody tests is the
// softest part of it. These pin the scalar policy the rest of the file assumes.
static void testDigestPrimitives() {
    std::printf("\n-- 1. digest primitives --\n");
    auto hf = [](float v) { simhash::Digest d; d.f32(v); return d.h; };
    auto hs = [](std::string_view s) { simhash::Digest d; d.str(s); return d.h; };

    CHECK(hf(0.0f) != hf(-0.0f),
          "+0.0 and -0.0 hash DIFFERENTLY — a sign difference is a real "
          "difference in the path that produced it");
    CHECK(hf(INFINITY) != hf(-INFINITY), "+inf and -inf are distinct");
    CHECK(hf(INFINITY) != hf(3.4e38f),   "inf is distinct from a large finite");

    // Two different NaN payloads. libm is free to produce either, and two runs
    // that both produced "not a number" have not diverged.
    uint32_t a = 0x7fc00001u, b = 0x7fe00000u;
    float na, nb; std::memcpy(&na, &a, 4); std::memcpy(&nb, &b, 4);
    CHECK(hf(na) == hf(nb), "two NaN payloads hash EQUAL (canonicalised)");
    CHECK(hf(na) != hf(1.0f), "...but a NaN is still distinct from a number");

    // THE ONE THAT MATTERS. A text-based hash (ecs_ptr_to_json) would print
    // both of these the same way and report "deterministic" while the
    // simulation diverged.
    const float one = 1.0f, ulp = std::nextafterf(one, INFINITY);
    CHECK(hf(one) != hf(ulp),
          "ONE ULP apart hashes differently (%.9g vs %.9g) — the exact class "
          "of divergence a text serializer hides", (double)one, (double)ulp);

    CHECK(hs("ab") != hs("ba"),  "string content is hashed, not just length");
    { // "ab"+"c" must not collide with "a"+"bc" — hence the length prefix
        simhash::Digest x; x.str("ab"); x.str("c");
        simhash::Digest y; y.str("a");  y.str("bc");
        CHECK(x.h != y.h, "length-prefixed: \"ab\"+\"c\" != \"a\"+\"bc\"");
    }
    { std::string s1 = "hello", s2(5, '\0');
      std::memcpy(s2.data(), "hello", 5);
      CHECK(hs(s1) == hs(s2),
            "equal CONTENT in different storage hashes equal (never the pointer)"); }
}

// ════════════════════════════════════════════════════════════════════════════
// Section 2 — hashWorld's sensitivity, on a bare world
// ════════════════════════════════════════════════════════════════════════════
// A hash that cannot fail is the failure mode this repo has already shipped
// twice (BUG-0049, BUG-0016). These prove hashWorld actually looks — at values,
// at STRUCTURE, and that auditCoverage notices an unclassified component. All
// on a bare flecs world, so nothing here depends on the runtime booting.
struct Unclassified { int x = 0; };

static void testHashSensitivity() {
    std::printf("\n-- 2. hashWorld sensitivity --\n");

    auto fresh = [](flecs::world& w) {
        MetaRegistry::registerAll(w);
        simhash::registerClassification(w);
    };

    // ── value sensitivity, at ONE ULP ──────────────────────────────────────
    // The whole point of hashing bits rather than text: a text serializer
    // prints both of these "1" and reports the world unchanged.
    {
        flecs::world w; fresh(w);
        Transform t{}; t.position = {1.0f, 2.0f, 3.0f};
        t.rotation = {0,0,0,1}; t.scale = {1,1,1};
        flecs::entity e = w.entity().set<Transform>(t).set<Name>({"a"});
        const uint64_t before = simhash::hashWorld(w);

        Transform m = t;
        m.position.x = std::nextafterf(t.position.x, INFINITY);
        e.set<Transform>(m);
        CHECK(simhash::hashWorld(w) != before,
              "ONE ULP on a live component changes the world hash");
    }

    // ── structural sensitivity ─────────────────────────────────────────────
    // A traversal that hashes values correctly but misses structure would pass
    // every value test above and be useless.
    {
        flecs::world w; fresh(w);
        Transform t{}; t.rotation = {0,0,0,1}; t.scale = {1,1,1};
        flecs::entity a = w.entity().set<Transform>(t).set<Name>({"a"});
        const uint64_t base = simhash::hashWorld(w);

        flecs::entity b = w.entity().set<Transform>(t).set<Name>({"b"});
        const uint64_t added = simhash::hashWorld(w);
        CHECK(added != base, "ADDING an entity changes the hash");

        b.destruct();
        CHECK(simhash::hashWorld(w) == base, "...and removing it restores it");


        a.set<Spinner>({1.0f, 0.0f});
        a.set<Spinner>({1.0f, 0.0f});
        const uint64_t withComp = simhash::hashWorld(w);
        CHECK(withComp != base, "ADDING a component changes the hash");
        a.remove<Spinner>();
        CHECK(simhash::hashWorld(w) == base, "...and removing it restores it");

        // Two entities with identical component VALUES must not collapse: the
        // raw entity id is mixed in, so creation order is observable.
        flecs::world w2; fresh(w2);
        w2.entity().set<Transform>(t).set<Name>({"a"});
        w2.entity().set<Transform>(t).set<Name>({"a"});
        flecs::world w3; fresh(w3);
        w3.entity().set<Transform>(t).set<Name>({"a"});
        CHECK(simhash::hashWorld(w2) != simhash::hashWorld(w3),
              "two identical entities do not collapse into one");
    }

    // ── the ChildOf target ─────────────────────────────────────────────────
    // A parent link decides the entity's WORLD pose, which is what physics
    // spawns against and what the renderer draws — while `Transform` itself is
    // BIT-IDENTICAL through a reparent. Until this was hashed, the whole class
    // of "the same local pose under a different ancestor" was invisible here.
    {
        flecs::world w; fresh(w);
        Transform t{}; t.rotation = {0,0,0,1}; t.scale = {1,1,1};
        flecs::entity p1 = w.entity().set<Transform>(t).set<Name>({"p1"});
        flecs::entity p2 = w.entity().set<Transform>(t).set<Name>({"p2"});
        flecs::entity c  = w.entity().set<Transform>(t).set<Name>({"c"});

        const uint64_t rootless = simhash::hashWorld(w);
        c.child_of(p1);
        const uint64_t under1 = simhash::hashWorld(w);
        CHECK(under1 != rootless,
              "PARENTING an entity changes the hash, with its Transform "
              "untouched — the local pose is identical either way");

        c.child_of(p2);
        CHECK(simhash::hashWorld(w) != under1,
              "and moving it to a DIFFERENT parent changes it again");

        c.remove(flecs::ChildOf, flecs::Wildcard);
        CHECK(simhash::hashWorld(w) == rootless,
              "...and unparenting restores it — 0 for 'no parent', so removal "
              "is a change rather than silence");
    }

    // ── auditCoverage actually looks ───────────────────────────────────────
    // Proves the audit is WIRED, not merely that removing a declare<T> happens
    // to trip it. An unclassified component on an entity with no classified
    // components at all is the case a registry-only walk would miss entirely.
    {
        flecs::world w; fresh(w);
        std::vector<std::string> un;
        CHECK(simhash::auditCoverage(w, un),
              "a freshly classified world audits clean (%zu unclassified)",
              un.size());

        w.entity().set<Unclassified>({7});
        const bool ok = simhash::auditCoverage(w, un);
        CHECK(!ok && !un.empty(),
              "an UNCLASSIFIED component fails the audit — the gate says "
              "'classify me', it does not silently stop looking");
        if (!un.empty())
            CHECK(un[0].find("Unclassified") != std::string::npos,
                  "...and names it: %s", un[0].c_str());
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Tiers and run shapes
// ════════════════════════════════════════════════════════════════════════════
// ── The contribution driver (the `moves` tier) ──────────────────────────────
// Submits MoveContributions for every character, from THREE sources with three
// modes, so the composition rule in runtime/move_compose.h is actually
// exercised rather than merely linked — the BUG-0049 shape this file already
// warns about one tier down.
//
// What this adds over move_composition_test.cpp is not the rule (200 shuffles
// prove that, and prove it better — the gate compares two runs of the SAME
// content, so it cannot vary arrival order between them). It is the PATH: the
// per-tick fold, EntityId -> entity resolution through a hashed cache, and the
// dispatch order into JoltPlugin, all of which are new in stage 2 and all of
// which sit between gameplay and a hashed component.
// Namespace-scope counters, because the plugin is constructed inside runOnce and
// the assertion that this tier is NOT A NO-OP has to be made from main. A test
// tier that silently submitted nothing would report "reproducible" for the same
// reason an empty physics world once did, one tier down in this file.
static long g_contribAccepted  = 0;
static long g_contribRefused   = 0;
static long g_movesDispatched  = 0;
static long g_movesUnresolved  = 0;

class ContributionPlugin final : public IEnginePlugin {
public:
    const char* name()    const override { return "GateContributions"; }
    const char* version() const override { return "1.0.0"; }
    void onAttach(RuntimeContext&) override {}
    void onDetach() override {}
    void onSimulationStart(flecs::world&) override {}
    void onSimulationStop() override {}

    void onUpdate(flecs::world& w, float) override {
        ++m_tick;
        simcmd::Buffer* buf = m_commands;
        if (!buf) return;
        // Sources deliberately do NOT ascend with the loop: Ability is
        // submitted before Gameplay for every character, so a fold that
        // resolved by arrival order rather than by `source` would produce a
        // different answer than the rule specifies.
        w.query_builder<const EntityId, const CharacterController>().build()
            .each([&](const EntityId& id, const CharacterController&) {
                const float p = (float)(id.value % 7) * 0.11f;
                const float t = (float)m_tick * 0.017f;
                auto put = [&](simcmd::Source src, float hx, float hz,
                               simcmd::move::Mode m) {
                    if (buf->submit(simcmd::move::contribution(
                            id.value, src, hx, hz, 0.0f, m)))
                        ++g_contribAccepted;
                    else
                        ++g_contribRefused;
                };
                put(simcmd::Source::Ability,  std::sin(t + p) * 0.5f, 0.0f,
                    simcmd::move::Mode::Override);
                put(simcmd::Source::Gameplay, 0.0f, std::cos(t + p) * 0.5f,
                    simcmd::move::Mode::Additive);
                // Above the Override, so it survives the floor and is summed.
                put(simcmd::Source::Cutscene, 0.0f, p * 0.25f,
                    simcmd::move::Mode::Additive);
            });
    }

    void bind(simcmd::Buffer* b) { m_commands = b; }

private:
    simcmd::Buffer* m_commands = nullptr;
    uint64_t        m_tick     = 0;
};

// ── The intent driver (the `input` tier) ────────────────────────────────────
// This file has said since it was written that NO TIER READS INPUT, and that
// the moment one did, ReplaySource would be how to make it deterministic. This
// is that tier, and it closes the instrument's largest stated blind spot.
//
// It is also the strongest available form of stage 5's property. The unit test
// compares two recorded INTENT streams at 1 and 2 frames per tick; this
// compares two WORLD HASHES over 240 ticks, through the intent sample, the
// contribution fold, EntityId resolution, the character controller and Jolt.
// The A/B comparison is literally "the same mouse motion at two frame rates
// must simulate identically" — which, before the intent layer, it did not,
// because a controller latched the device in onFrame and fed the result into
// movement.
static long g_intentTicks = 0;

class IntentPlugin final : public IEnginePlugin {
public:
    const char* name()    const override { return "GateIntent"; }
    const char* version() const override { return "1.0.0"; }
    void onAttach(RuntimeContext&) override {}
    void onDetach() override {}
    void onSimulationStart(flecs::world&) override {}
    void onSimulationStop() override {}

    void onUpdate(flecs::world&, float) override {
        if (!m_intents || !m_commands) return;
        const simintent::Intent* in = m_intents->find(m_player);
        if (!in) return;
        ++g_intentTicks;
        // A miniature controller: turn the look delta into a heading and steer
        // along it. Deliberately the shape the FPS kit has — yaw accumulated
        // from mouse counts, feeding the MOVEMENT direction — because that is
        // the coupling being measured, not the camera.
        m_yaw += in->lookDx * 0.0025f;
        const float fx = -std::sin(m_yaw), fz = -std::cos(m_yaw);
        m_commands->submit(simcmd::move::contribution(
            m_player, simcmd::Source::Gameplay,
            fx * 1.5f + in->moveX, fz * 1.5f + in->moveY, 0.0f,
            simcmd::move::Mode::Additive));
    }

    void bind(simintent::Buffer* i, simcmd::Buffer* c, uint64_t player) {
        m_intents = i; m_commands = c; m_player = player;
    }

private:
    simintent::Buffer* m_intents  = nullptr;
    simcmd::Buffer*    m_commands = nullptr;
    uint64_t           m_player   = 0;
    float              m_yaw      = 0.0f;
};

// The player's stable id in the input tier's world. Fixed, for the reason the
// `moves` tier's ids are fixed: generateEntityId() is random_device-seeded.
static constexpr uint64_t kGatePlayer = 0x5EED10C0DEull;

static const char* kGateInputConfig = R"({
  "contexts": [
    { "name": "Gameplay",
      "actions": [
        { "name": "Fire", "type": "digital", "bindings": ["mouse:left"] },
        { "name": "Move", "type": "axis2",
          "bindings": ["key:W:+y","key:S:-y","key:D:+x","key:A:-x"] }
      ] }
  ]
})";

struct Tier { const char* name; bool spinner, animator, scripting, physics, moves, input; };
static const Tier kTiers[] = {
    // A genuinely STATIC world. Nothing moves, so A/B must match — this is the
    // instrument's baseline, and if it ever fails the hash itself is wrong and
    // nothing below it is worth reading.
    { "static",    false, false, false, false, false, false },
    // Spinner is the engine's one demo gameplay system, and it turns out to be
    // frame-rate driven — see the kKnown entry. Separating it from `static` is
    // what makes the baseline mean anything.
    { "spinner",   true,  false, false, false, false, false },
    // The animator tier the plan specified and the first build of this file
    // never created. Its absence mattered: three comments in the frame-rate fix
    // claimed "the gate caught it" about Animator::time when no entity in any
    // tier had ever carried an Animator. Skeleton and clip are built through
    // ozz's offline builders, so this needs no Assimp and no asset files — the
    // same fixture animator_system_test.cpp uses.
    { "animator",  false, true,  false, false, false, false },
    { "scripting", true,  true,  true,  false, false, false },
    { "physics",   true,  true,  true,  true,  false, false },
    // Stage 2's dispatch path: contributions composed per tick, resolved from
    // EntityId, and driven into the character controller. A superset of
    // `physics`, so a divergence here that `physics` does not show is
    // attributable to composition or resolution rather than to Jolt.
    { "moves",     true,  true,  true,  true,  true,  false },
    // A tier DRIVEN BY A RECORDED INPUT STREAM, which this file could not have
    // before stage 5: the device is sampled once per TICK, so the same events
    // produce the same simulation at either frame cadence. Read the A/B row
    // for this tier as "the same mouse motion at 1 and 2 frames per tick".
    //
    // moves=FALSE, and that is load-bearing rather than tidiness. With the
    // contribution driver also running, its Ability OVERRIDE outranked the
    // intent driver's Gameplay ADDITIVE and discarded it — the composition
    // rule working exactly as designed, and silently neutralising the thing
    // under test. The tier stayed GREEN under a mutation that samples intent
    // per frame instead of per tick, because the player's movement was never
    // coming from the intent at all. Measured before it was fixed: the player
    // ended at the identical position at both cadences.
    { "input",     true,  true,  true,  true,  false, true  },
};

// A divergence this tree already knows about, with its cause named. The point
// is that a permanently-red lane does not become wallpaper: a KNOWN entry
// reports, an UNKNOWN one fails, and a known entry that STOPS firing is also
// reported so a fix is noticed instead of quietly turning the entry into a lie.
struct Known { const char* tier; const char* cmp; const char* cause; };
// A std::vector and not a C array, because this list is EMPTY and `Known k[] =
// {}` declares a zero-size array — a GNU extension, ill-formed in ISO C++, so
// the table being empty (the good outcome) is the case that would break a
// pedantic build. A vector is legal at any size and needs no count maintained
// beside it.
static const std::vector<Known> kKnown = {
    // ── EMPTY, AS OF 2026-09-08, AND THAT IS THE HEADLINE ───────────────────
    // Every tier and both comparisons are now in the GATING lane (--gating runs
    // exactly the pairs with no entry here). Three causes were found by this
    // gate and closed:
    //
    //   Spinner   ran in tickSystems() at FRAME dt, writing Transform.
    //   Animator  same function, two lines below, writing Animator::time.
    //             Both clocks moved into the fixed step at kSimDt
    //             (BUG-0053, runtime_sim.cpp).
    //   Contacts  arrived from Jolt worker threads in thread-race order and
    //             became the order of CollisionEvents::entered/exited, which
    //             scripts iterate. Sorted at the source, and the maps whose
    //             iteration drives body destruction and character stepping
    //             became ordered (BUG-0054, jolt_plugin.h).
    //
    // ── ADDING A ROW HERE SILENTLY REMOVES A PAIR FROM THE GATING LANE ──────
    // Promotion is automatic and so is DEMOTION, which is the asymmetry to
    // watch: a row added to shut the gate up looks identical, in the full
    // lane's output, to a divergence that was always known. So kExpectGating
    // below pins the count, and shrinking the gating lane fails the test until
    // someone updates that number deliberately.
};
// How many (tier, comparison) pairs the gating lane is expected to cover. It is
// kTiers x 2 minus kKnown's size, and it is written down rather than computed
// so that SHRINKING the gating lane is a deliberate act. Without it, adding a
// kKnown row would quietly demote a pair and every lane would still be green.
static constexpr int kExpectGating = 14;   // 7 tiers x 2 comparisons, 0 known

static const Known* findKnown(const char* tier, const char* cmp) {
    for (const auto& k : kKnown)
        if (!std::strcmp(k.tier, tier) && !std::strcmp(k.cmp, cmp)) return &k;
    return nullptr;
}

// A small world built in CODE, not from a .scene file: no filesystem, no
// directory_iterator, no cooked assets, nothing that could vary for a reason
// unrelated to the simulation.
// A flat identity skeleton and an empty-track clip, both through ozz's offline
// builders: no Assimp, no files, no cook. Lifted from animator_system_test.cpp,
// which is where this shape is already proven.
static Skeleton makeSkeleton(int boneCount) {
    Skeleton s;
    s.bones.resize((size_t)boneCount);
    for (int i = 0; i < boneCount; ++i) {
        s.bones[(size_t)i].name        = "b" + std::to_string(i);
        s.bones[(size_t)i].parentIndex = (i == 0) ? -1 : 0;
    }
    s.buildBoneMap();
    anim::buildOzzSkeleton(s);
    return s;
}
static AnimClip makeClip(const Skeleton& skel, float duration) {
    AnimClip clip;
    if (!skel.ozz) return clip;
    ozz::animation::offline::RawAnimation raw;
    raw.duration = duration;
    raw.tracks.resize((size_t)skel.ozz->num_joints());
    ozz::animation::offline::AnimationBuilder builder;
    ozz::unique_ptr<ozz::animation::Animation> built = builder(raw);
    if (!built) return clip;
    clip.name     = "gate_clip";
    clip.duration = duration;
    clip.ozz      = std::shared_ptr<const ozz::animation::Animation>(
        built.release(), ozz::Deleter<ozz::animation::Animation>());
    return clip;
}

static void buildWorld(flecs::world& w, const Tier& t, EngineRuntime& engine) {
    for (int i = 0; i < 24; ++i) {
        Transform tr{};
        tr.position = { (float)i * 0.37f, 4.0f + (float)i * 0.11f, (float)(i % 5) };
        tr.rotation = { 0, 0, 0, 1 };
        tr.scale    = { 1, 1, 1 };
        flecs::entity e = w.entity()
            .set<Transform>(tr)
            .set<Name>({ "prop_" + std::to_string(i) });
        if (t.spinner) e.set<Spinner>({ 0.9f + (float)i * 0.01f, 0.4f });
    }

    if (t.animator) {
        // Registered ONCE and shared: the handles are what the components
        // carry, and Animator::clip is deliberately not hashed (a session-local
        // id) while clipPath and clipIndex are.
        const Skeleton skel = makeSkeleton(4);
        const AnimClip clip = makeClip(skel, 2.0f);
        for (int i = 0; i < 8; ++i) {
            SkinnedMesh sm{};
            sm.skeleton = engine.skeletons().add(Skeleton(skel));
            Animator a{};
            a.clip     = engine.clips().add(AnimClip(clip));
            a.clipPath = "gate_clip";
            a.playing  = true;
            a.looping  = true;
            a.speed    = 0.7f + (float)i * 0.05f;   // distinct phases per entity
            a.time     = (float)i * 0.13f;
            Transform tr{};
            tr.position = { (float)i, 0.0f, 0.0f };
            tr.rotation = { 0, 0, 0, 1 };
            tr.scale    = { 1, 1, 1 };
            w.entity().set<Transform>(tr)
                      .set<Name>({ "skinned_" + std::to_string(i) })
                      .set<SkinnedMesh>(sm)
                      .set<Animator>(a);
        }
    }

    // ── Parented entities ──────────────────────────────────────────────────
    // Under a rotated, UNIFORMLY scaled parent, and the uniformity is
    // deliberate: a rotated NON-uniform parent introduces shear, which SRT
    // cannot represent, so a divergence there would be an inherent limitation
    // of the transform representation rather than a bug this gate should
    // report. The non-uniform case is pinned separately, in section 6.
    //
    // These carry Spinner rather than a RigidBody because a physics body on a
    // child entity is now REFUSED (stage 3b) — while parented, its world pose
    // would be decided by the parent and by physics at once. So what this
    // exercises is getWorldMatrix through a rotated ancestor and the ChildOf
    // target now in hashWorld, which is where the gap was.
    {
        Transform pt{};
        pt.position = { 3.0f, 2.0f, -1.0f };
        pt.rotation = { 0.0f, 0.3826834f, 0.0f, 0.9238795f };   // 45 deg about Y
        pt.scale    = { 2.0f, 2.0f, 2.0f };                      // UNIFORM
        flecs::entity parent = w.entity()
            .set<Transform>(pt).set<Name>({ "parent_rot" });
        if (t.spinner) parent.set<Spinner>({ 0.35f, 0.2f });
        for (int i = 0; i < 4; ++i) {
            Transform ct{};
            ct.position = { (float)i * 0.6f, 0.5f, 0.0f };
            ct.rotation = { 0, 0, 0, 1 };
            ct.scale    = { 1, 1, 1 };
            flecs::entity c = w.entity()
                .set<Transform>(ct)
                .set<Name>({ "child_" + std::to_string(i) });
            if (t.spinner) c.set<Spinner>({ 0.7f + (float)i * 0.05f, 0.3f });
            c.child_of(parent);
        }
    }

    if (!t.physics) return;

    // The physics tier must actually EXERCISE the hazards it claims to look
    // for, or a pass means only that nothing ran. An earlier version of this
    // file gave a few entities a RigidBody with no floor and no characters:
    // nothing collided, updateCharacters iterated an empty map, and the gate
    // duly reported physics as reproducible. That is the BUG-0049 shape — a
    // check that cannot fail — so the world below is built to collide.
    RigidBody floor{};
    floor.bodyType   = PhysicsBodyType::Static;
    floor.halfExtent = { 50.0f, 0.5f, 50.0f };
    w.entity().set<Transform>({ {0.f, -0.5f, 0.f}, {0,0,0,1}, {1,1,1} })
              .set<Name>({ "floor" })
              .set<RigidBody>(floor);

    // Dynamic boxes dropped in a tight cluster so they land ON EACH OTHER —
    // contact events are what arrive from Jolt's worker threads in thread-race
    // order and land in CollisionEvents, a hashed component.
    for (int i = 0; i < 40; ++i) {
        RigidBody rb{};
        rb.bodyType   = PhysicsBodyType::Dynamic;
        rb.halfExtent = { 0.5f, 0.5f, 0.5f };
        rb.mass       = 1.0f;
        const float x = (float)(i % 5) * 0.9f - 1.8f;
        const float z = (float)((i / 5) % 5) * 0.9f - 1.8f;
        const float y = 1.0f + (float)(i / 25) * 1.1f;
        w.entity().set<Transform>({ {x, y, z}, {0,0,0,1}, {1,1,1} })
                  .set<Name>({ "box_" + std::to_string(i) })
                  .set<RigidBody>(rb);
    }

    // Characters, so JoltPlugin::updateCharacters has an unordered_map with
    // more than one entry to iterate — the ordering hazard is only observable
    // when characters can interact.
    for (int i = 0; i < 6; ++i) {
        CharacterController cc{};
        const float x = (float)i * 0.55f - 1.4f;
        flecs::entity e =
            w.entity().set<Transform>({ {x, 2.0f, 0.0f}, {0,0,0,1}, {1,1,1} })
                      .set<Name>({ "char_" + std::to_string(i) })
                      .set<CharacterController>(cc);
        // FIXED ids, and only on the tier that needs them. generateEntityId()
        // is seeded from std::random_device, so minting ids here would put a
        // different value into a HASHED component on every run and redden this
        // gate — which is also exactly why ScriptHost::charMove refuses to
        // assign one inside the simulation and falls back instead.
        //
        // Only on the `moves` tier, so the five tiers that were already green
        // keep hashing precisely the content they were green on: a divergence
        // appearing in `moves` is then attributable to stage 2 and not to a
        // component this file started adding everywhere.
        if (t.moves) e.set<EntityId>({ 0x9E3779B97F4A7C15ull + (uint64_t)i });
    }

    // The player the input tier steers — its own character, so the six above
    // keep behaving exactly as they do in the `moves` tier and a divergence
    // here is attributable to the intent path rather than to a changed world.
    if (t.input) {
        CharacterController pc{};
        w.entity().set<Transform>({ {0.0f, 2.0f, 3.0f}, {0,0,0,1}, {1,1,1} })
                  .set<Name>({ "player" })
                  .set<CharacterController>(pc)
                  .set<EntityId>({ kGatePlayer });
    }
}

// A directory with no project.json, no scripts and no assets — created once and
// reused, so every run of every tier sees the same (empty) content.
static const std::filesystem::path& hermeticRoot() {
    static const std::filesystem::path p = [] {
        std::filesystem::path d = std::filesystem::temp_directory_path()
                                / "engine_determinism_gate_root";
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return p;
}

struct RunResult {
    std::vector<uint64_t> perTick;
    std::vector<std::string> unclassifiedStart, unclassifiedEnd;
    bool ok = false;
};

// A one-ULP nudge applied to one component of one entity at a chosen tick. This
// is how the END-TO-END path gets tested: not just that the digest is
// value-sensitive (section 2 does that on a bare world), but that runOnce ->
// firstDiff -> explain() reports the RIGHT tick and names the RIGHT component
// type. The plan called this "the one that matters" and the first build of this
// file skipped it.
struct Perturb {
    int  atTick = -1;
    bool applied = false;
};

static void runOnce(const Tier& t, int framesPerTick, int nTicks,
                    RunResult& out, int detailAtTick,
                    simhash::HashReport* detail,
                    Perturb* perturb = nullptr) {
    EngineConfig cfg;
    cfg.openAssetDatabase = false;
    cfg.autoDetectProject = false;
    cfg.defaultScene      = false;
    // AN EMPTY PROJECT ROOT, AND IT MUST BE SET ─────────────────────────────
    // Leaving it empty is not neutral: LuaScriptPlugin::collectAutorunScripts
    // resolves `m_projectRoot / "scripts" / "autorun"` and, with an empty root,
    // that is a RELATIVE path against the current working directory. Run from
    // the repo root the scripting tier picked up ./scripts/autorun/*.lua and
    // executed them — so the tier's behaviour depended on where the binary was
    // launched from, and a `directory_iterator` (unordered) chose the order.
    // A determinism gate whose result depends on its CWD is not one.
    cfg.projectRoot = hermeticRoot();

    EngineRuntime engine;
    if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) return;

    if (t.physics)   engine.plugins().add(std::make_shared<JoltPlugin>());
    if (t.scripting) engine.plugins().add(std::make_shared<LuaScriptPlugin>());
    // ── A SCRIPTED DEVICE ───────────────────────────────────────────────────
    // Installed BEFORE attachPlugins, so the manager is bound before anything
    // reads it. The events are added per tick in the loop below rather than
    // preloaded, so the stream is spread across frames the way a real one is —
    // at 2 frames/tick the tick's motion arrives on one frame and is sampled
    // on the next boundary, which is exactly the case that used to diverge.
    input::ReplaySource* device = nullptr;
    if (t.input) {
        auto src = std::make_unique<input::ReplaySource>();
        device = src.get();
        device->addDevice({1, hid::DeviceClass::Mouse,    0x1234, 0x5678, 42, "mouse"});
        device->addDevice({2, hid::DeviceClass::Keyboard, 0, 0, 43, "kbd"});
        engine.inputManager().initWithSource(std::move(src));
        engine.inputManager().loadConfigText(kGateInputConfig);
        engine.actionSet().declare("Fire");
        engine.actionSet().declare("Move");
        engine.setLocalController(kGatePlayer);
    }

    std::shared_ptr<IntentPlugin> intentDriver;
    if (t.input) {
        intentDriver = std::make_shared<IntentPlugin>();
        intentDriver->bind(&engine.intents(), &engine.commands(), kGatePlayer);
        engine.plugins().add(intentDriver);
    }

    std::shared_ptr<ContributionPlugin> contrib;
    if (t.moves) {
        contrib = std::make_shared<ContributionPlugin>();
        // Bound to the runtime's buffer, not given one: submissions must land
        // in the tick that is open, and the phase guard refuses anything else.
        contrib->bind(&engine.commands());
        engine.plugins().add(contrib);
    }
    engine.attachPlugins();

    flecs::world& w = engine.simWorld();
    buildWorld(w, t, engine);

    simhash::auditCoverage(w, out.unclassifiedStart);

    // InPlace, not Snapshot: Snapshot drags the whole SceneSerializer JSON and
    // entity-id-remap path into every run, which is a different thing to test.
    engine.startSimulation(EngineRuntime::SimMode::InPlace);

    // Same total simulated time either way: framesPerTick frames of
    // (kSimDt / framesPerTick) each. Frame cadence changes; sim time does not.
    const float dt = (1.0f / 60.0f) / (float)framesPerTick;
    out.perTick.reserve((size_t)nTicks);
    uint64_t evStamp = 1;
    for (int tick = 0; tick < nTicks; ++tick) {
        // A deterministic "hand": a slow sweep with a reversal, so the heading
        // the driver integrates is not monotonic and a sign error would show.
        // Integers, so no float accumulates in the DEVICE — the arithmetic
        // under test belongs to the engine, not the fixture.
        const int dx = ((tick / 20) % 2) ? -8 : 10;
        const int dy = ((tick / 13) % 2) ?  4 : -2;

        for (int f = 0; f < framesPerTick; ++f) {
            // ── THE MOTION IS SPLIT ACROSS THE TICK'S FRAMES ────────────────
            // The same total per tick, delivered in framesPerTick pieces, so
            // the two runs differ ONLY in cadence. This matters more than it
            // looks: the first version of this fixture emitted one event per
            // TICK, and a mutation that samples intent per FRAME instead of
            // per tick left the tier GREEN — because the tick's whole motion
            // arrived on the first frame, so the first sample took all of it
            // either way. A tier that cannot fail under the defect it exists
            // to catch is the BUG-0049 shape this file warns about; splitting
            // the motion is what makes the A/B row mean what it says.
            if (device)
                device->addEvent({ evStamp++, 1, hid::EventType::MouseMotion,
                                   0, 0, dx / framesPerTick,
                                   dy / framesPerTick });
            engine.tick(dt);
        }

        // ── DESPAWN, so the body-DESTRUCTION order is actually exercised ────
        // syncRuntimeBodies walks m_entityToBody and destroys the bodies whose
        // entity has died, and Jolt's Architecture.md requires bodies to be
        // added and removed in a consistent order for determinism because
        // BodyID recycling feeds its contact sort. Nothing here ever destroyed
        // an entity, so that loop only ever ran over survivors — the hazard was
        // named in the ordered-map comment and never reached by the test.
        //
        // HONEST LABEL: this exercises the destroy path but does NOT
        // discriminate the ordered map. Measured — reverting m_entityToBody to
        // an unordered_map leaves this green, with or without these despawns,
        // because libc++'s std::hash<uint64_t> is unseeded identity and two
        // runs with the same insertion sequence get the same bucket order. The
        // reason those maps are ordered is a robustness argument, not something
        // this gate can see; jolt_plugin.h says so at the declaration. What
        // these despawns DO buy is coverage of syncRuntimeBodies' removal path
        // at all, which had none.
        //
        // Deterministic by construction: a fixed tick, and a name predicate
        // rather than an iteration-order pick.
        if (t.physics && tick == 90) {
            std::vector<flecs::entity> doomed;
            w.query_builder<const Name, const RigidBody>().build()
                .each([&](flecs::entity e, const Name& n, const RigidBody&) {
                    for (const char* v : { "box_7", "box_12", "box_23", "box_31" })
                        if (n.value == v) doomed.push_back(e);
                });
            for (flecs::entity e : doomed) e.destruct();
        }

        if (perturb && tick == perturb->atTick && !perturb->applied) {
            // The smallest change the engine can represent, on a live
            // component, mid-run. A text-based digest prints this identically.
            w.query_builder<Transform, const Name>().build()
                .each([&](flecs::entity, Transform& tr, const Name& n) {
                    if (perturb->applied || n.value != "prop_3") return;
                    tr.position.x = std::nextafterf(tr.position.x, INFINITY);
                    perturb->applied = true;
                });
        }
        const bool want = (detail && tick == detailAtTick);
        out.perTick.push_back(simhash::hashWorld(w, want ? detail : nullptr));
    }

    // End-of-run audit: a component type instantiated mid-run (a script
    // spawning something) is absent at the start and present here.
    simhash::auditCoverage(w, out.unclassifiedEnd);

    if (t.moves) {
        // Accepted-by-the-buffer is not delivered-to-physics: resolving an
        // EntityId back to a live entity is new in stage 2 and can fail
        // silently. Read before stopSimulation(), which resets the counters.
        g_movesDispatched += (long)engine.movesDispatched();
        g_movesUnresolved += (long)engine.movesUnresolved();
    }
    engine.stopSimulation();
    engine.shutdown();
    out.ok = true;
}

// Index of the first differing tick, or -1.
static int firstDiff(const std::vector<uint64_t>& a,
                     const std::vector<uint64_t>& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return (int)i;
    return a.size() == b.size() ? -1 : (int)n;
}

// Re-runs both shapes to the divergent tick with per-type/per-entity detail on.
// Costs the extra work ONLY on failure.
static void explain(const Tier& t, int fptA, int fptB, int tick) {
    simhash::HashReport ra, rb;
    RunResult a, b;
    runOnce(t, fptA, tick + 1, a, tick, &ra);
    runOnce(t, fptB, tick + 1, b, tick, &rb);

    std::printf("       differing component types:\n");
    for (const auto& [name, ha] : ra.perType) {
        auto it = std::find_if(rb.perType.begin(), rb.perType.end(),
                               [&](const auto& p) { return p.first == name; });
        if (it != rb.perType.end() && it->second != ha)
            std::printf("         %-22s A %016llx  B %016llx\n", name.c_str(),
                        (unsigned long long)ha, (unsigned long long)it->second);
    }
    for (const auto& [ent, ha] : ra.perEntity) {
        auto it = std::find_if(rb.perEntity.begin(), rb.perEntity.end(),
                               [&](const auto& p) { return p.first == ent; });
        if (it != rb.perEntity.end() && it->second != ha) {
            std::printf("       first differing entity: %llu  A %016llx  B %016llx\n",
                        (unsigned long long)ent, (unsigned long long)ha,
                        (unsigned long long)it->second);
            break;
        }
    }
}

static int g_unexpected = 0, g_fixed = 0;

static void compare(const Tier& t, const char* cmp, int fptA, int fptB,
                    int nTicks) {
    RunResult a, b;
    runOnce(t, fptA, nTicks, a, -1, nullptr);
    runOnce(t, fptB, nTicks, b, -1, nullptr);
    if (!a.ok || !b.ok) {
        std::printf("  FAIL  tier=%-9s %-4s could not run\n", t.name, cmp);
        ++g_failures;
        return;
    }
    if (!a.unclassifiedStart.empty() || !a.unclassifiedEnd.empty()) {
        const auto& u = a.unclassifiedEnd.empty() ? a.unclassifiedStart
                                                  : a.unclassifiedEnd;
        std::printf("  FAIL  tier=%-9s UNCLASSIFIED component(s) — the gate is "
                    "not looking at them:\n", t.name);
        for (const auto& s : u) std::printf("           %s\n", s.c_str());
        ++g_failures;
    }

    const int d = firstDiff(a.perTick, b.perTick);
    const Known* k = findKnown(t.name, cmp);

    if (d < 0) {
        if (k) {
            std::printf("  FIXED?    tier=%-9s %-4s now MATCHES across %d ticks.\n"
                        "            Known cause was: %s\n"
                        "            If that was fixed deliberately, remove the "
                        "kKnown entry and promote this tier to the `unit` lane.\n",
                        t.name, cmp, nTicks, k->cause);
            ++g_fixed;
        } else {
            std::printf("  ok    tier=%-9s %-4s reproducible across %d ticks\n",
                        t.name, cmp, nTicks);
        }
        return;
    }

    if (k) {
        std::printf("  EXPECTED  tier=%-9s %-4s diverges at tick %d\n"
                    "            A %016llx  B %016llx\n"
                    "            known: %s\n",
                    t.name, cmp, d,
                    (unsigned long long)a.perTick[d],
                    (unsigned long long)b.perTick[d], k->cause);
    } else {
        std::printf("  UNEXPECTED tier=%-9s %-4s diverges at tick %d\n"
                    "            A %016llx  B %016llx\n",
                    t.name, cmp, d,
                    (unsigned long long)a.perTick[d],
                    (unsigned long long)b.perTick[d]);
        ++g_unexpected;
        ++g_failures;
    }
    explain(t, fptA, fptB, d);
}

// ── --gating ────────────────────────────────────────────────────────────────
// Runs ONLY the (tier, comparison) pairs with no kKnown entry, and any
// divergence in those is a hard failure. The `unit` lane runs this; the full
// diagnostic run stays in the non-gating `determinism` lane.
//
// PROMOTION IS AUTOMATIC, and that is the point: delete a kKnown entry because
// you fixed its cause, and that pair enters the gating lane on the next build
// with no CMake edit and no second list to keep in sync. A promotion someone
// has to remember is a promotion that does not happen.
static bool gatingOnly(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--gating")) return true;
    return false;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool gating = gatingOnly(argc, argv);
    std::printf("determinism_gate_test: ECS-observable determinism%s\n",
                gating ? " [--gating: only pairs with no known divergence]" : "");

    testDigestPrimitives();
    testHashSensitivity();

    // ── 2b. END TO END: the gate reports the right tick and the right type ──
    {
        std::printf("\n-- 2b. a one-ULP perturbation, through the whole gate --\n");
        const Tier& t = kTiers[0];                    // static: nothing else moves
        const int   kAt = 37, kN = 60;

        RunResult a, b;
        Perturb   p{ kAt, false };
        runOnce(t, 1, kN, a, -1, nullptr);            // clean
        runOnce(t, 1, kN, b, -1, nullptr, &p);        // nudged at tick kAt

        CHECK(p.applied, "the perturbation was applied");
        const int d = firstDiff(a.perTick, b.perTick);
        CHECK(d == kAt,
              "the gate reports the perturbation at tick %d (expected %d) — "
              "not just that the digest changed, but that the run loop and "
              "firstDiff locate it", d, kAt);

        // ...and that explain() names Transform, not merely "something differs".
        simhash::HashReport ra, rb;
        RunResult da, db;
        Perturb   p2{ kAt, false };
        runOnce(t, 1, kAt + 1, da, kAt, &ra);
        runOnce(t, 1, kAt + 1, db, kAt, &rb, &p2);
        std::string named;
        for (const auto& [name, ha] : ra.perType) {
            auto it = std::find_if(rb.perType.begin(), rb.perType.end(),
                                   [&](const auto& q) { return q.first == name; });
            if (it != rb.perType.end() && it->second != ha) named += name + " ";
        }
        CHECK(named == "Transform ",
              "and names exactly the component that changed: \"%s\"",
              named.c_str());
    }

    const int kTicks = 240;
    std::printf("\n-- 3. A vs A' (same process, two sequential runs) --\n");
    for (const auto& t : kTiers) {
        if (gating && findKnown(t.name, "A/A'")) continue;
        compare(t, "A/A'", 1, 1, kTicks);
    }

    std::printf("\n-- 4. A vs B (1 frame/tick vs 2 — render-rate coupling) --\n");
    for (const auto& t : kTiers) {
        if (gating && findKnown(t.name, "A/B")) continue;
        compare(t, "A/B", 1, 2, kTicks);
    }

    // ── The gating lane must not have shrunk ────────────────────────────────
    {
        int covered = 0;
        for (const auto& t : kTiers)
            for (const char* c : { "A/A'", "A/B" })
                if (!findKnown(t.name, c)) ++covered;
        CHECK(covered == kExpectGating,
              "the gating lane covers %d of %d tier/comparison pairs "
              "(expected %d) — if this dropped, a kKnown row was added and "
              "quietly DEMOTED a pair that used to be gated. That is allowed, "
              "but it has to be a deliberate act: update kExpectGating in the "
              "same change and say why",
              covered, (int)(sizeof(kTiers) / sizeof(kTiers[0])) * 2,
              kExpectGating);
    }

    // ── The `moves` tier must not be a no-op ────────────────────────────────
    // A tier that submitted nothing would report "reproducible" for exactly the
    // reason an earlier physics tier did — nothing ran. So the contributions
    // are counted as they are ACCEPTED by the buffer, not as they are offered.
    {
        CHECK(g_contribAccepted > 0,
              "the moves tier actually submitted contributions (%ld accepted) "
              "— without this, a green tier would only mean nothing ran",
              g_contribAccepted);
        // Every submission happens inside a plugin's onUpdate, which is exactly
        // the window Buffer opens. A refusal here would mean the phase guard
        // added in stage 1.5 is shut during the one phase it must be open in.
        CHECK(g_movesDispatched > 0 && g_movesUnresolved == 0,
              "and every one reached the character controller (%ld dispatched, "
              "%ld unresolved) — EntityId -> entity resolution is new in stage "
              "2, and a command that resolves to nothing moves nothing while "
              "still hashing as reproducible",
              g_movesDispatched, g_movesUnresolved);
        CHECK(g_intentTicks > 0,
              "and the input tier's driver saw an intent on %ld ticks — a tier "
              "whose controller never ran would report 'reproducible' for the "
              "reason an empty physics world once did", g_intentTicks);
        CHECK(g_contribRefused == 0,
              "and none were refused out of phase (%ld) — the submission "
              "window is open across broadcastUpdate, end to end",
              g_contribRefused);
    }

    std::printf("\n-- findings this gate does not fix --\n");
    std::printf("  * hid::nowNs() is read INSIDE the fixed step "
                "(runtime_sim.cpp, m_input.beginTick) — the tick boundary is "
                "wall-clock. Inert here because no tier reads input.\n");
    std::printf("  * (CLOSED 2026-09-08) m_ecs.progress() used to take NO "
                "delta_time — flecs documents 0 as \"automatically measure the "
                "time passed since the last frame\", i.e. a wall clock in the "
                "ECS pipeline. It now takes an explicit dt, and the SIM world's "
                "progress moved into the fixed step.\n");

    std::printf("\n%s: %d unexpected divergence(s), %d possibly-fixed, "
                "%d assertion failure(s)\n",
                "determinism_gate_test", g_unexpected, g_fixed, g_failures);

    return g_failures ? 1 : 0;
}
