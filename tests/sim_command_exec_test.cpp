// ── sim_command_exec_test — the physics verbs, executed from the record ─────
//
// Item #8 of the 2026-09-12 audit. Five of the seven command kinds had no
// executor: SetKinematicTarget, Impulse, SetVelocity, Jump, SetBodyType. A kind
// nothing executes is a claim, and once a take has been recorded the claim is
// in the wire format permanently. So three are now executed and two are
// RESERVED — refused by Buffer::submit (pinned in sim_command_test §8).
//
// And the three that had no executor had a second problem: the script surface
// called the backend DIRECTLY, so a jump or an impulse never reached the tick's
// command record at all. A replay's per-tick command digest could not see them.
//
// What this file pins:
//   1. The script surface SUBMITS: applyImpulse / setVelocity / charJump on an
//      entity with a stable id become commands; without one, the old direct
//      path is kept.
//   2. Each verb's executor does what the direct call did.
//   3. PHASE ORDER, which the canonical sort does not give. Impulse is kind 2
//      and SetVelocity kind 3, so sort order alone would let a SetVelocity
//      erase an impulse from the same tick. Submitted impulse-FIRST on purpose.
//   4. The recorded ring holds the verbs at the tick they ran.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include <flecs.h>

#include "components/character_controller.h"
#include "components/entity_id.h"
#include "components/name.h"
#include "components/rigid_body.h"
#include "core/transform.h"
#include "plugins/jolt_plugin.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"
#include "runtime/scripting/script_host.h"
#include "runtime/sim_command.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

using simcmd::Cmd;
using simcmd::Source;
using simcmd::SimCommand;

static constexpr float    kDt   = 1.0f / 60.0f;
static constexpr uint64_t kBox  = 0xB0C5000001ull;
static constexpr uint64_t kHero = 0xB0C5000002ull;

// Submits a fixed schedule of commands from onUpdate — the only phase the
// buffer accepts them in.
class VerbDriver final : public IEnginePlugin {
public:
    const char* name()    const override { return "VerbDriver"; }
    const char* version() const override { return "1.0.0"; }
    void onAttach(RuntimeContext&) override {}
    void onDetach() override {}
    void onSimulationStart(flecs::world&) override {}
    void onSimulationStop() override {}

    void onUpdate(flecs::world&, float) override {
        ++m_tick;
        for (const auto& [tick, cmd] : m_schedule)
            if (tick == m_tick && m_commands) m_commands->submit(cmd);
    }

    void at(uint64_t tick, const SimCommand& c) { m_schedule.push_back({tick, c}); }
    void bind(simcmd::Buffer* b) { m_commands = b; }

private:
    std::vector<std::pair<uint64_t, SimCommand>> m_schedule;
    simcmd::Buffer* m_commands = nullptr;
    uint64_t        m_tick     = 0;
};

static const std::filesystem::path& hermeticRoot() {
    static const std::filesystem::path p = [] {
        std::filesystem::path d = std::filesystem::temp_directory_path()
                                / "engine_sim_command_exec_root";
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return p;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("sim_command_exec_test — the physics verbs, from the record\n");

    // ── 1. The script surface submits ──────────────────────────────────────
    {
        std::printf("\n-- 1. the script surface submits --\n");
        flecs::world w;
        flecs::entity withId = w.entity().set<Transform>({}).set<EntityId>({77});
        flecs::entity noId   = w.entity().set<Transform>({});
        ScriptHost host(&w);
        simcmd::Buffer buf;               // standalone: open, no phase to be in
        host.setCommandBuffer(&buf);

        host.applyImpulse(withId, 1.0f, 2.0f, 3.0f);
        host.setVelocity (withId, 4.0f, 5.0f, 6.0f);
        host.charJump    (withId, 7.0f);
        CHECK(buf.size() == 3,
              "three verbs on an entity with a stable id are three commands "
              "(%zu) — they used to go straight to the backend and never reach "
              "the record", buf.size());

        bool imp = false, vel = false, jmp = false;
        for (const SimCommand& c : buf.commands()) {
            if (c.entity != 77) continue;
            if (c.kind == Cmd::Impulse &&
                c.a[0] == 1.0f && c.a[1] == 2.0f && c.a[2] == 3.0f) imp = true;
            if (c.kind == Cmd::SetVelocity &&
                c.a[0] == 4.0f && c.a[1] == 5.0f && c.a[2] == 6.0f) vel = true;
            if (c.kind == Cmd::Jump &&
                c.a[simcmd::phys::kSlotSpeed] == 7.0f) jmp = true;
        }
        CHECK(imp && vel && jmp,
              "each with its kind and payload intact (impulse %d, velocity %d, "
              "jump %d)", (int)imp, (int)vel, (int)jmp);

        host.applyImpulse(noId, 1.0f, 0.0f, 0.0f);
        CHECK(buf.size() == 3,
              "an entity with no stable id takes the direct path instead (%zu)",
              buf.size());
    }

    // ── 2–4. Executors, phase order, and the record ────────────────────────
    {
        std::printf("\n-- 2. executors, 3. phase order, 4. the record --\n");
        EngineConfig cfg;
        cfg.openAssetDatabase = false;
        cfg.autoDetectProject = false;
        cfg.defaultScene      = false;
        cfg.projectRoot       = hermeticRoot();

        EngineRuntime engine;
        if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) {
            CHECK(false, "engine init");
            return 1;
        }
        auto jolt   = std::make_shared<JoltPlugin>();
        auto driver = std::make_shared<VerbDriver>();
        engine.plugins().add(jolt);
        driver->bind(&engine.commands());
        engine.plugins().add(driver);
        engine.attachPlugins();

        flecs::world& w = engine.simWorld();
        RigidBody floor{}; floor.bodyType = PhysicsBodyType::Static;
        floor.halfExtent = { 50.0f, 0.5f, 50.0f };
        w.entity().set<Transform>({{0.f,-0.5f,0.f},{0,0,0,1},{1,1,1}})
                  .set<Name>({"floor"}).set<RigidBody>(floor);

        // High enough to stay airborne for the whole run: contact and friction
        // would otherwise reach into the velocities this measures.
        RigidBody dyn{}; dyn.bodyType = PhysicsBodyType::Dynamic;
        dyn.halfExtent = { 0.5f, 0.5f, 0.5f }; dyn.mass = 1.0f;
        flecs::entity box =
            w.entity().set<Transform>({{0.f,60.f,0.f},{0,0,0,1},{1,1,1}})
                      .set<Name>({"box"}).set<RigidBody>(dyn)
                      .set<EntityId>({kBox});

        CharacterController cc{};
        flecs::entity hero =
            w.entity().set<Transform>({{6.f,1.f,0.f},{0,0,0,1},{1,1,1}})
                      .set<Name>({"hero"}).set<CharacterController>(cc)
                      .set<EntityId>({kHero});

        // Tick 10: the impulse is submitted FIRST, the velocity second — the
        // order that sort order alone would get wrong.
        driver->at(10, simcmd::phys::impulse (kBox,  Source::Gameplay, 2.0f, 0.0f, 0.0f));
        driver->at(10, simcmd::phys::velocity(kBox,  Source::Gameplay, 3.0f, 0.0f, 0.0f));
        driver->at(20, simcmd::phys::impulse (kBox,  Source::Gameplay, 0.0f, 0.0f, 4.0f));
        driver->at(60, simcmd::phys::jump    (kHero, Source::Gameplay, 5.0f));

        engine.setCommandRecording(true, 256);
        engine.startSimulation(EngineRuntime::SimMode::InPlace);

        float vx10 = 0, vy10 = 0, vz10 = 0, vz19 = 0, vz20 = 0, tmp = 0;
        float heroY0 = 0, heroMax = -1e9f;
        for (int t = 1; t <= 80; ++t) {
            engine.tick(kDt);
            if (t == 10) jolt->getVelocity(w, box.id(), vx10, vy10, vz10);
            if (t == 19) jolt->getVelocity(w, box.id(), tmp, tmp, vz19);
            if (t == 20) jolt->getVelocity(w, box.id(), tmp, tmp, vz20);
            if (t == 59) heroY0 = hero.get<Transform>().position.y;
            if (t >= 60) heroMax = std::max(heroMax, hero.get<Transform>().position.y);
        }

        CHECK(std::fabs(vx10 - 5.0f) < 0.05f,
              "SetVelocity then Impulse: vx = 3 + 2 = %.3f, whichever was "
              "submitted first. Sort order alone (Impulse is kind 2, "
              "SetVelocity 3) would erase the impulse and give 3", vx10);
        CHECK(std::fabs((vz20 - vz19) - 4.0f) < 0.05f,
              "an Impulse alone adds impulse / mass (dvz %.3f, want 4)",
              vz20 - vz19);
        CHECK(heroMax - heroY0 > 0.3f,
              "a Jump command lifts a grounded character (rose %.3f m)",
              heroMax - heroY0);
        CHECK(engine.physicsCommandsDispatched() == 4,
              "exactly the four scheduled verbs were dispatched (%llu)",
              (unsigned long long)engine.physicsCommandsDispatched());

        // Tick 80 is ticksAgo 0, so tick 10 is 70 back and tick 60 is 20 back.
        auto has = [](const EngineRuntime::RecordedTick& r, Cmd k) {
            for (const SimCommand& c : r.cmds)
                if (c.kind == k) return true;
            return false;
        };
        const auto& r10 = engine.recordedTick(70);
        const auto& r60 = engine.recordedTick(20);
        CHECK(r10.tick == 10 && has(r10, Cmd::Impulse) && has(r10, Cmd::SetVelocity),
              "the record holds tick 10's impulse and velocity (slot tick %llu)",
              (unsigned long long)r10.tick);
        CHECK(r60.tick == 60 && has(r60, Cmd::Jump),
              "and tick 60's jump (slot tick %llu)", (unsigned long long)r60.tick);

        engine.stopSimulation();
        engine.shutdown();
    }

    if (g_failures) {
        std::printf("\nsim_command_exec_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nsim_command_exec_test: ALL PASS\n");
    return 0;
}
