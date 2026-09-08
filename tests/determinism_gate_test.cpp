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
// ── NO INPUT IS DRIVEN, and that is deliberate for v1 ───────────────────────
// InputManager::beginTick is called inside the fixed step with hid::nowNs(), a
// wall clock. No tier here reads input — the worlds are driven by Spinner,
// animation and physics — so the clock cannot reach anything hashed, and
// feeding a ReplaySource would add machinery that proves nothing yet. The wall
// clock is reported as a finding below rather than papered over. The moment a
// tier reads input, ReplaySource (input_sources.h, already used in
// input_test.cpp:322) is how to make it deterministic.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "components/animator.h"
#include "components/character_controller.h"
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
struct Tier { const char* name; bool spinner, scripting, physics; };
static const Tier kTiers[] = {
    // A genuinely STATIC world. Nothing moves, so A/B must match — this is the
    // instrument's baseline, and if it ever fails the hash itself is wrong and
    // nothing below it is worth reading.
    { "static",    false, false, false },
    // Spinner is the engine's one demo gameplay system, and it turns out to be
    // frame-rate driven — see the kKnown entry. Separating it from `static` is
    // what makes the baseline mean anything.
    { "spinner",   true,  false, false },
    { "scripting", true,  true,  false },
    { "physics",   true,  true,  true  },
};

// A divergence this tree already knows about, with its cause named. The point
// is that a permanently-red lane does not become wallpaper: a KNOWN entry
// reports, an UNKNOWN one fails, and a known entry that STOPS firing is also
// reported so a fix is noticed instead of quietly turning the entry into a lie.
struct Known { const char* tier; const char* cmp; const char* cause; };
static const Known kKnown[] = {
    // ── CLOSED 2026-09-08: spinner and scripting A/B ────────────────────────
    // Their entries used to live here. The gate found the coupling (Spinner and
    // the animator both ran at FRAME dt in tickSystems, writing hashed
    // components at render rate), the fix moved both clocks into the fixed step
    // at kSimDt, and this table shrank — which is the whole loop working. Both
    // tiers are now in the GATING `unit` lane via --gating, so they cannot
    // regress.
    //
    // What is left is physics, and the two rows are ONE cause: contacts are
    // pushed from Jolt worker threads under a mutex, so their order is a thread
    // race, and that order lands in CollisionEvents — a component scripts
    // iterate. A/B sees it too because A/B runs the same physics.
    { "physics",   "A/A'", "collision events are pushed from Jolt worker threads "
                           "in thread-race order into CollisionEvents, a component "
                           "scripts iterate. JoltPlugin::updateCharacters and "
                           "syncRuntimeBodies also iterate unordered_maps" },
    { "physics",   "A/B",  "same cause as A/A' — the thread race is present at any "
                           "frame cadence. NOT render-rate coupling: the Transform "
                           "divergence this row used to carry is gone" },
};
static const Known* findKnown(const char* tier, const char* cmp) {
    for (const auto& k : kKnown)
        if (!std::strcmp(k.tier, tier) && !std::strcmp(k.cmp, cmp)) return &k;
    return nullptr;
}

// A small world built in CODE, not from a .scene file: no filesystem, no
// directory_iterator, no cooked assets, nothing that could vary for a reason
// unrelated to the simulation.
static void buildWorld(flecs::world& w, const Tier& t) {
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
        w.entity().set<Transform>({ {x, 2.0f, 0.0f}, {0,0,0,1}, {1,1,1} })
                  .set<Name>({ "char_" + std::to_string(i) })
                  .set<CharacterController>(cc);
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

static void runOnce(const Tier& t, int framesPerTick, int nTicks,
                    RunResult& out, int detailAtTick,
                    simhash::HashReport* detail) {
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
    engine.attachPlugins();

    flecs::world& w = engine.simWorld();
    buildWorld(w, t);

    simhash::auditCoverage(w, out.unclassifiedStart);

    // InPlace, not Snapshot: Snapshot drags the whole SceneSerializer JSON and
    // entity-id-remap path into every run, which is a different thing to test.
    engine.startSimulation(EngineRuntime::SimMode::InPlace);

    // Same total simulated time either way: framesPerTick frames of
    // (kSimDt / framesPerTick) each. Frame cadence changes; sim time does not.
    const float dt = (1.0f / 60.0f) / (float)framesPerTick;
    out.perTick.reserve((size_t)nTicks);
    for (int tick = 0; tick < nTicks; ++tick) {
        for (int f = 0; f < framesPerTick; ++f) engine.tick(dt);
        const bool want = (detail && tick == detailAtTick);
        out.perTick.push_back(simhash::hashWorld(w, want ? detail : nullptr));
    }

    // End-of-run audit: a component type instantiated mid-run (a script
    // spawning something) is absent at the start and present here.
    simhash::auditCoverage(w, out.unclassifiedEnd);

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

    std::printf("\n-- findings this gate does not fix --\n");
    std::printf("  * hid::nowNs() is read INSIDE the fixed step "
                "(runtime_sim.cpp, m_input.beginTick) — the tick boundary is "
                "wall-clock. Inert here because no tier reads input.\n");
    std::printf("  * m_ecs.progress() at tickSystems() takes NO delta_time, and "
                "flecs documents 0 as \"automatically measure the time passed "
                "since the last frame\" — the ECS pipeline advances on wall "
                "time. Inert today (no systems are registered) and live the "
                "moment a kit registers one.\n");

    std::printf("\n%s: %d unexpected divergence(s), %d possibly-fixed, "
                "%d assertion failure(s)\n",
                "determinism_gate_test", g_unexpected, g_fixed, g_failures);
    return g_failures ? 1 : 0;
}
