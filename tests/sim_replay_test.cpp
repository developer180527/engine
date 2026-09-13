// ── sim_replay_test — record a take, replay it, get the same world ──────────
//
// Replay stage R1, and the test the command architecture has named since its
// first stage: "record N ticks of commands + per-tick world hashes, restore the
// initial world, replay, assert every per-tick hash matches". It was blocked on
// a world restore that does not exist. It turned out not to need one — replay
// needs a REPRODUCIBLE START, not a mid-session restore, and Snapshot Play
// already has one: the scene snapshot it loads into a fresh world. The take
// keeps that string, and replay loads the same bytes.
//
// What this pins:
//   1. The take format: round trip, and refusal of anything not wholly a take.
//   2. A session driven by a live device, recorded as a take, REPLAYS WITH NO
//      DEVICE to identical per-tick command digests and world hashes — every
//      tick, through the intent sample, the contribution fold, EntityId
//      resolution, the character controller and Jolt.
//   3. The verdict LOCALISES: one mouse count changed in one recorded intent
//      diverges at exactly that tick, and as a COMMAND divergence — the logic
//      decided differently — not a world one.
//   4. The refusals that keep a replay honest: a different action list, a
//      recording begun mid-session, an InPlace session.
//   5. CROSS-PROCESS: the take is written to a file and replayed by a child
//      process — this binary re-invoked — that never saw the recording, whose
//      edit world is EMPTY, and whose controller reads intents only through the
//      frozen C ABI (engineIntentGet) and declares its actions in
//      onSimulationStart, the way a kit must. In-process replay alone could be
//      passing on state that survives in memory between the runs.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <engine/engine_api.h>
#include <flecs.h>

#include "core/memory/mem.h"
#include "components/character_controller.h"
#include "components/entity_id.h"
#include "components/name.h"
#include "components/rigid_body.h"
#include "core/transform.h"
#include "plugins/jolt_plugin.h"
#include "runtime/input/input_manager.h"
#include "runtime/input/input_sources.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"
#include "runtime/take.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

static constexpr float    kDt     = 1.0f / 60.0f;
static constexpr uint64_t kPlayer = 0x7A4E000000000001ull;
static constexpr int      kTicks  = 150;
static constexpr int      kPerturbAt = 77;

static const char* kConfig = R"({
  "contexts": [
    { "name": "Gameplay",
      "actions": [
        { "name": "Fire", "type": "digital", "bindings": ["mouse:left"] },
        { "name": "Move", "type": "axis2",
          "bindings": ["key:W:+y","key:S:-y","key:D:+x","key:A:-x"] }
      ] }
  ]
})";

// A miniature controller in the FPS kit's shape: yaw accumulated from look
// counts, steering the character. It reads INTENTS only — a replay feeds those
// and nothing else. Two ways in, which must agree to the bit: the engine's
// buffer directly (a plugin holding EngineRuntime&), or the frozen C ABI (what
// a kit has — before EngineApiIntentV1 a kit had no way to read an intent at
// all, so the real game could not record a replayable take).
class Controller final : public IEnginePlugin {
public:
    explicit Controller(bool viaCAbi = false) : m_viaCAbi(viaCAbi) {}
    const char* name()    const override { return "ReplayController"; }
    const char* version() const override { return "1.0.0"; }
    void onAttach(RuntimeContext&) override {}
    void onDetach() override {}
    // ── RESET SESSION STATE HERE, which is a rule and not a detail ──────────
    // m_yaw is state the take cannot see. Left alone, the replay session starts
    // from the yaw the recording ENDED with and diverges from tick one. Every
    // plugin and kit that keeps session state in members owes the same reset.
    void onSimulationStart(flecs::world&) override {
        m_yaw = 0.0f;
        m_lateDone = false;
        if (m_viaCAbi) {
            // A kit declares its actions HERE — so a replay's action-list check
            // has to run after this, which is what the child exercises.
            engineIntentDeclareAction("Fire");
            engineIntentDeclareAction("Move");
            m_player = engineEntityFind("player");
        }
    }
    void onSimulationStop() override {}

    void onUpdate(flecs::world&, float) override {
        if (m_lateDeclare && !m_lateDone) {     // the mistake §5 catches
            engineIntentDeclareAction("Late");
            m_lateDone = true;
        }
        if (!m_commands) return;
        float moveX = 0, moveY = 0, lookDx = 0;
        if (m_viaCAbi) {
            EngineIntent ei{}; ei.structSize = sizeof(EngineIntent);
            if (!engineIntentGet(m_player, &ei)) return;
            moveX = ei.moveX; moveY = ei.moveY; lookDx = ei.lookDx;
            ++m_cAbiReads;
            // An OLDER kit's smaller struct: only its prefix may be written.
            unsigned char small[12]; std::memset(small, 0xCC, sizeof small);
            const uint32_t sz = 8;                     // structSize + moveX
            std::memcpy(small, &sz, sizeof sz);
            engineIntentGet(m_player, reinterpret_cast<EngineIntent*>(small));
            if (small[8] != 0xCC || small[11] != 0xCC) m_overran = true;
        } else {
            if (!m_intents) return;
            const simintent::Intent* in = m_intents->find(kPlayer);
            if (!in) return;
            moveX = in->moveX; moveY = in->moveY; lookDx = in->lookDx;
        }
        m_yaw += lookDx * 0.0025f;
        m_commands->submit(simcmd::move::contribution(
            kPlayer, simcmd::Source::Gameplay,
            -std::sin(m_yaw) * 1.5f + moveX,
            -std::cos(m_yaw) * 1.5f + moveY, 0.0f,
            simcmd::move::Mode::Additive));
    }
    void bind(simintent::Buffer* i, simcmd::Buffer* c) { m_intents = i; m_commands = c; }
    uint64_t cAbiReads() const { return m_cAbiReads; }
    void     setLateDeclare(bool on) { m_lateDeclare = on; }
    bool     overran()   const { return m_overran; }

private:
    bool               m_viaCAbi  = false;
    EngineEntity       m_player   = 0;
    uint64_t           m_cAbiReads = 0;
    bool               m_overran  = false;
    bool               m_lateDeclare = false;
    bool               m_lateDone    = false;
    simintent::Buffer* m_intents  = nullptr;
    simcmd::Buffer*    m_commands = nullptr;
    float              m_yaw      = 0.0f;
};

static const std::filesystem::path& hermeticRoot() {
    static const std::filesystem::path p = [] {
        std::filesystem::path d = std::filesystem::temp_directory_path()
                                / "engine_sim_replay_root";
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return p;
}

// Runs a replay of `t` for its full length and returns the verdict.
static EngineRuntime::ReplayStatus replay(EngineRuntime& engine, const take::Take& t) {
    if (!engine.startReplay(t)) return {};
    for (size_t i = 0; i < t.ticks.size(); ++i) engine.tick(kDt);
    const EngineRuntime::ReplayStatus st = engine.replayStatus();
    engine.stopSimulation();
    return st;
}

// ── The child: a fresh process replays a take file ─────────────────────────
// Nothing in common with the parent but the binary. No device, no declared
// actions, no local controller, an EMPTY edit world — everything the replay
// needs must come from the file, and every intent read goes through the C ABI.
static int childReplay(const char* takePath, const char* outPath) {
    std::ifstream in(takePath, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    take::Take t; std::string err;
    if (!take::decode(bytes.data(), bytes.size(), t, &err)) {
        std::printf("child: decode failed: %s\n", err.c_str());
        return 2;
    }
    EngineConfig cfg;
    cfg.openAssetDatabase = false;
    cfg.autoDetectProject = false;
    cfg.defaultScene      = false;
    cfg.projectRoot       = hermeticRoot();
    EngineRuntime engine;
    if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) return 3;
    auto ctl = std::make_shared<Controller>(/*viaCAbi=*/true);
    ctl->bind(nullptr, &engine.commands());
    engine.plugins().add(std::make_shared<JoltPlugin>());
    engine.plugins().add(ctl);
    engine.attachPlugins();

    const bool started = engine.startReplay(t);
    EngineRuntime::ReplayStatus st{};
    if (started) {
        for (size_t i = 0; i < t.ticks.size(); ++i) engine.tick(kDt);
        st = engine.replayStatus();
        engine.stopSimulation();
    }
    engine.shutdown();
    std::FILE* f = std::fopen(outPath, "w");
    if (!f) return 4;
    std::fprintf(f, "%d %d %llu %llu %d %llu %d\n", started ? 1 : 0,
                 st.complete ? 1 : 0, (unsigned long long)st.ticksCompared,
                 (unsigned long long)st.firstDivergentTick, (int)st.kind,
                 (unsigned long long)ctl->cAbiReads(), ctl->overran() ? 1 : 0);
    std::fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 4 && std::strcmp(argv[1], "--replay-child") == 0)
        return childReplay(argv[2], argv[3]);
    std::printf("sim_replay_test — record a take, replay it, same world\n");

    // ── 1. The take format ─────────────────────────────────────────────────
    {
        std::printf("\n-- 1. the take format --\n");
        take::Take t;
        t.actionHash = 7; t.localController = 9; t.simDt = kDt;
        t.start = R"({"entities":[]})";
        t.ticks.push_back({ 1, 11, 22, 0, 0 });
        simintent::Intent in{}; in.entity = 9; in.lookDx = 3.5f; in.held = 0x5;
        t.intents.push_back(in);
        t.ticks.push_back({ 2, 33, 44, 0, 1 });

        const std::vector<uint8_t> bytes = take::encode(t);
        take::Take back; std::string err;
        CHECK(take::decode(bytes.data(), bytes.size(), back, &err) &&
              back.actionHash == 7 && back.localController == 9 &&
              back.simDt == kDt && back.start == t.start &&
              back.ticks.size() == 2 && back.ticks[1].worldHash == 44 &&
              back.intents.size() == 1 && back.ticks[1].intentCount == 1 &&
              take::intentsOf(back, back.ticks[1])[0].lookDx == 3.5f &&
              take::intentsOf(back, back.ticks[1])[0].held == 0x5 &&
              take::intentsOf(back, back.ticks[0]).empty(),
              "a take round-trips through bytes intact (%s)", err.c_str());

        take::Take untouched; untouched.actionHash = 123;
        CHECK(!take::decode(bytes.data(), bytes.size() - 1, untouched, &err) &&
              untouched.actionHash == 123,
              "a TRUNCATED take is refused, and the output is left alone (%s)",
              err.c_str());
        std::vector<uint8_t> flipped = bytes; flipped[24] ^= 0x01;
        err.clear();   // or a refusal with no message would print the last one
        CHECK(!take::decode(flipped.data(), flipped.size(), back, &err),
              "one flipped bit is refused rather than replayed (%s)", err.c_str());
    }

    // ── The rig: a live device driving a character through intents ─────────
    EngineConfig cfg;
    cfg.openAssetDatabase = false;
    cfg.autoDetectProject = false;
    cfg.defaultScene      = false;
    cfg.projectRoot       = hermeticRoot();
    EngineRuntime engine;
    if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) {
        std::printf("  FAIL  engine init\n");
        return 1;
    }
    auto  src    = std::make_unique<input::ReplaySource>();
    auto* device = src.get();
    device->addDevice({1, hid::DeviceClass::Mouse,    0x1234, 0x5678, 42, "mouse"});
    device->addDevice({2, hid::DeviceClass::Keyboard, 0, 0, 43, "kbd"});
    engine.inputManager().initWithSource(std::move(src));
    engine.inputManager().loadConfigText(kConfig);
    engine.actionSet().declare("Fire");
    engine.actionSet().declare("Move");
    engine.setLocalController(kPlayer);

    auto jolt = std::make_shared<JoltPlugin>();
    auto ctl  = std::make_shared<Controller>();
    ctl->bind(&engine.intents(), &engine.commands());
    engine.plugins().add(jolt);
    engine.plugins().add(ctl);
    engine.attachPlugins();

    // The EDIT world. A floor, a character to steer, and loose boxes it walks
    // into, so the take exercises contacts and not just a free-falling capsule.
    flecs::world& edit = engine.simWorld();
    RigidBody floor{}; floor.bodyType = PhysicsBodyType::Static;
    floor.halfExtent = { 50.0f, 0.5f, 50.0f };
    edit.entity().set<Transform>({{0.f,-0.5f,0.f},{0,0,0,1},{1,1,1}})
                 .set<Name>({"floor"}).set<RigidBody>(floor);
    flecs::entity box0;
    for (int i = 0; i < 6; ++i) {
        RigidBody box{}; box.bodyType = PhysicsBodyType::Dynamic;
        box.halfExtent = { 0.3f, 0.3f, 0.3f }; box.mass = 1.0f;
        flecs::entity b =
            edit.entity().set<Transform>({{(float)(i % 3) * 0.7f - 0.7f, 0.4f,
                                           -1.5f - (float)(i / 3) * 0.7f},
                                          {0,0,0,1},{1,1,1}})
                         .set<Name>({"box_" + std::to_string(i)}).set<RigidBody>(box);
        if (i == 0) box0 = b;
    }
    CharacterController cc{};
    edit.entity().set<Transform>({{0.f,1.f,0.f},{0,0,0,1},{1,1,1}})
                 .set<Name>({"player"}).set<CharacterController>(cc)
                 .set<EntityId>({kPlayer});

    // ── 2. Record a take with a live device ────────────────────────────────
    std::printf("\n-- 2. record --\n");
    // The command ring on too, so its steady state is measured below.
    engine.setCommandRecording(true, 8);
    CHECK(engine.startSimulation(EngineRuntime::SimMode::Snapshot),
          "a Snapshot session starts");
    CHECK(engine.startTakeRecording(), "a take starts recording before tick one");
    constexpr int kWarm = 50;
    mem::TagStats simAt{}, replayAt{};
    uint64_t stamp = 1;
    for (int t = 0; t < kTicks; ++t) {
        if (t == kWarm) {
            simAt    = mem::stats(mem::Tag::Sim);
            replayAt = mem::stats(mem::Tag::Replay);
        }
        const int dx = ((t / 25) % 2) ? -9 : 11;   // a sweep with reversals
        device->addEvent({ stamp++, 1, hid::EventType::MouseMotion, 0, 0, dx, 2 });
        engine.tick(kDt);
    }
    const mem::TagStats simEnd    = mem::stats(mem::Tag::Sim);
    const mem::TagStats replayEnd = mem::stats(mem::Tag::Replay);
    engine.stopSimulation();
    engine.setCommandRecording(false, 0);
    const take::Take rec = engine.stopTakeRecording();

    // ── 2b. Memory: attributed, and flat across the recorded ticks ─────────
    // A vector per recorded tick was one allocation per fixed step for the
    // whole session, filed under Core; the brace-assigned command ring was two
    // more. These pin both halves: the right TAG, and NO growth once warm.
    std::printf("\n-- 2b. memory --\n");
#if defined(ENGINE_MEM_ROUTE) && ENGINE_MEM_ROUTE == 0
    std::printf("  skip  ENGINE_MEM_ROUTE=0: the sanitizer owns allocations\n");
#else
    CHECK(replayEnd.allocCount == replayAt.allocCount,
          "recording made NO Replay allocations across ticks %d..%d (%llu) — the "
          "take was reserved at start, not grown per tick", kWarm, kTicks,
          (unsigned long long)(replayEnd.allocCount - replayAt.allocCount));
    CHECK(simEnd.allocCount == simAt.allocCount,
          "and the tick's command/intent buffers and the command ring made none "
          "either once warm (%llu)",
          (unsigned long long)(simEnd.allocCount - simAt.allocCount));
    CHECK(replayEnd.currentBytes > 0 && simEnd.currentBytes > 0,
          "and both are ATTRIBUTED — Replay %llu bytes, Sim %llu bytes live — "
          "not filed under Core", (unsigned long long)replayEnd.currentBytes,
          (unsigned long long)simEnd.currentBytes);
#endif

    CHECK(!engine.takeActionListChanged(),
          "a take whose actions were all declared up front raises no "
          "late-declaration warning");
    CHECK(rec.ticks.size() == (size_t)kTicks,
          "the take holds every tick (%zu of %d)", rec.ticks.size(), kTicks);
    double look = 0.0;
    for (const simintent::Intent& in : rec.intents) look += std::fabs(in.lookDx);
    CHECK(look > 100.0,
          "and the recorded intents CARRY the device's motion (%.0f counts) — a "
          "dead input path would replay an empty take perfectly", look);
    CHECK(!rec.ticks.empty() && rec.ticks.front().worldHash != rec.ticks.back().worldHash,
          "and the world actually changed across the take — matching hashes of a "
          "world that never moved would prove nothing");

    // ── The edit world moves on after the take ─────────────────────────────
    // A designer keeps working after recording. The replay must start from the
    // TAKE's snapshot, not from the edit world as it is now — and without this
    // change the two are identical, so a replay that ignored the take's start
    // would pass every assertion below.
    box0.get_mut<Transform>().position.x += 3.0f;

    // ── 3. Replay it with NO device ────────────────────────────────────────
    std::printf("\n-- 3. replay --\n");
    const EngineRuntime::ReplayStatus same = replay(engine, rec);
    CHECK(same.complete && same.ticksCompared == (uint64_t)kTicks,
          "the replay compared every recorded tick (%llu of %d)",
          (unsigned long long)same.ticksCompared, kTicks);
    CHECK(same.firstDivergentTick == 0,
          "with IDENTICAL command digests and world hashes on all of them — the "
          "same world, rebuilt from the take's start and its intents alone, "
          "though the edit world has changed since (first divergence: %s)",
          same.firstDivergentTick ? std::to_string(same.firstDivergentTick).c_str()
                                  : "none");

    // The take survives the trip through bytes as a replayable thing, not just
    // as an equal struct.
    {
        const std::vector<uint8_t> bytes = take::encode(rec);
        take::Take fromDisk; std::string err;
        const bool ok = take::decode(bytes.data(), bytes.size(), fromDisk, &err);
        const EngineRuntime::ReplayStatus viaBytes = ok ? replay(engine, fromDisk)
                                                        : EngineRuntime::ReplayStatus{};
        CHECK(ok && viaBytes.complete && viaBytes.firstDivergentTick == 0,
              "and a take read back from its encoded bytes replays identically "
              "too (%zu bytes)", bytes.size());
    }

    // ── 3b. Cross-process ──────────────────────────────────────────────────
    std::printf("\n-- 3b. cross-process --\n");
    {
        const std::filesystem::path dir  = hermeticRoot();
        const std::filesystem::path file = dir / "rec.take";
        const std::filesystem::path out  = dir / "child_result.txt";
        {
            const std::vector<uint8_t> bytes = take::encode(rec);
            std::ofstream o(file, std::ios::binary);
            o.write(reinterpret_cast<const char*>(bytes.data()),
                    (std::streamsize)bytes.size());
        }
        std::error_code ec; std::filesystem::remove(out, ec);
        const std::string cmd = "\"" + std::string(argv[0]) + "\" --replay-child \"" +
                                file.string() + "\" \"" + out.string() + "\"";
        const int rc = std::system(cmd.c_str());
        int started = 0, complete = 0, kind = -1, overran = 1;
        unsigned long long compared = 0, first = ~0ull, reads = 0;
        if (std::FILE* f = std::fopen(out.string().c_str(), "r")) {
            if (std::fscanf(f, "%d %d %llu %llu %d %llu %d", &started, &complete,
                            &compared, &first, &kind, &reads, &overran) != 7)
                started = 0;
            std::fclose(f);
        }
        CHECK(rc == 0 && started == 1,
              "a CHILD process starts a replay from the take file alone — its "
              "actions declared by the controller in onSimulationStart through "
              "the C ABI, after which the action list is checked (rc %d)", rc);
        CHECK(complete == 1 && compared == (unsigned long long)kTicks && first == 0,
              "and reproduces every tick of the parent's recording (%llu of %d, "
              "first divergence %llu)", compared, kTicks, first);
        CHECK(reads >= (unsigned long long)kTicks - 1,
              "reading every intent through engineIntentGet (%llu reads) — a kit "
              "can now drive a replayable session", reads);
        CHECK(overran == 0,
              "and a caller's smaller structSize is honoured: nothing past it is "
              "written");
    }

    // ── 4. The verdict localises ───────────────────────────────────────────
    // One mouse count — the device's real smallest step — in ONE recorded
    // intent. Not one ULP: the controller multiplies the look delta into a yaw,
    // and a one-ULP change can be absorbed by that arithmetic, which would make
    // the test pass for the wrong reason.
    std::printf("\n-- 4. localisation --\n");
    {
        take::Take bad = rec;
        const std::span<simintent::Intent> at =
            take::intentsOf(bad, bad.ticks[kPerturbAt - 1]);
        CHECK(!at.empty(), "tick %d has a recorded intent to change", kPerturbAt);
        if (!at.empty()) at[0].lookDx += 1.0f;
        const EngineRuntime::ReplayStatus d = replay(engine, bad);
        CHECK(d.firstDivergentTick == (uint64_t)kPerturbAt,
              "one mouse count changed at tick %d diverges at EXACTLY tick %llu — "
              "not before (the record was followed until then) and not later "
              "(nothing absorbed it)",
              kPerturbAt, (unsigned long long)d.firstDivergentTick);
        CHECK(d.kind == EngineRuntime::ReplayStatus::Divergence::Commands,
              "and as a COMMAND divergence — the logic decided differently, so "
              "the report points at gameplay, not at physics");
    }

    // ── 5. The refusals that keep a replay honest ──────────────────────────
    std::printf("\n-- 5. refusals --\n");
    {
        take::Take other = rec; other.actionHash ^= 1;
        CHECK(!engine.startReplay(other),
              "a take recorded against a DIFFERENT action list is refused — its "
              "intent bits would be read as other actions");
        CHECK(!engine.simulating(),
              "and the action-list refusal — checked after the session starts, "
              "so a kit's onSimulationStart declarations count — stops that session");

        take::Take gappy = rec; gappy.ticks[10].tick = 99;
        CHECK(!engine.startReplay(gappy) && !engine.simulating(),
              "a MALFORMED take (tick numbers not 1..N) is refused up front, not "
              "reported later as a gameplay divergence");

        take::Take holey = rec; holey.ticks[5].firstIntent += 1;
        CHECK(!engine.startReplay(holey) && !engine.simulating(),
              "and so is one whose intent runs do not tile the intent array — a "
              "tick would otherwise be fed another tick's input");

        // A replay drives the take's controller, then hands the runtime back.
        engine.setLocalController(0x55);
        const EngineRuntime::ReplayStatus after = replay(engine, rec);
        CHECK(after.complete && after.firstDivergentTick == 0,
              "and the refusals left no session behind: the next replay runs clean");
        CHECK(engine.localController() == 0x55,
              "and the local controller is RESTORED when the replay ends (%llx)",
              (unsigned long long)engine.localController());
        engine.setLocalController(kPlayer);

        engine.startSimulation(EngineRuntime::SimMode::Snapshot);
        engine.tick(kDt);
        CHECK(!engine.startTakeRecording(),
              "recording cannot begin after tick one — the take's start would not "
              "be the state its first tick came from");
        engine.stopSimulation();

        engine.startSimulation(EngineRuntime::SimMode::InPlace);
        CHECK(!engine.startTakeRecording(),
              "nor in an InPlace session, which has no start snapshot");
        engine.stopSimulation();

        // An action declared AFTER the take captured its list — in onUpdate,
        // not onSimulationStart — is outside every check a replay makes.
        ctl->setLateDeclare(true);
        engine.startSimulation(EngineRuntime::SimMode::Snapshot);
        engine.startTakeRecording();
        for (int t = 0; t < 3; ++t) engine.tick(kDt);
        engine.stopSimulation();
        (void)engine.stopTakeRecording();
        ctl->setLateDeclare(false);
        CHECK(engine.takeActionListChanged(),
              "an action declared in onUpdate during a recording is WARNED "
              "about — the take's hash cannot cover it");
        CHECK(engine.actionSet().indexOf("Late") == -1,
              "and, declared inside the session, it ends with the session");
    }

    engine.shutdown();
    if (g_failures) {
        std::printf("\nsim_replay_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nsim_replay_test: ALL PASS\n");
    return 0;
}
