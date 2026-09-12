// ── sim_intent_test — what was ASKED, sampled once per tick ─────────────────
//
// Stage 5 of the command architecture: the intent layer stage 1 named without
// building, so `MoveContribution` would not be mistaken for it.
//
// Two records, two questions:
//   Intent      what was ASKED    re-derivable   rollback, prediction
//   SimCommand  what was DECIDED  replayable     replay, divergence reports
//
// ── THE PROPERTY THIS FILE EXISTS FOR ───────────────────────────────────────
// **The same device motion produces the same intent stream regardless of how
// many frames each tick was split into.** That is the coupling stage 4 left
// open and said so: `CameraLook` stopped the render-rate write reaching hashed
// state, and the SIMULATION still read a value accumulated at frame rate,
// because a controller latches the mouse in `onFrame` and feeds the resulting
// yaw into movement and raycasts. Sampling once per tick, inside the fixed
// step, is what closes it — and this drives a real engine at two cadences to
// show that it does, rather than asserting it about the code.
//
// The rest is the POD and ordering discipline sim_command.h already pays for,
// checked here too because an intent stream goes to the same three places: a
// hash, a replay file, and eventually a wire.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <flecs.h>

#include "components/entity_id.h"
#include "components/name.h"
#include "core/transform.h"
#include "runtime/input/input_manager.h"
#include "runtime/input/input_sources.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"
#include "runtime/sim_intent.h"

using namespace input;
using simintent::Intent;

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

static const char* kConfig = R"({
  "contexts": [
    { "name": "Gameplay",
      "actions": [
        { "name": "Fire", "type": "digital", "bindings": ["mouse:left"] },
        { "name": "Jump", "type": "digital", "bindings": ["key:Space"] },
        { "name": "Move", "type": "axis2",
          "bindings": ["key:W:+y","key:S:-y","key:D:+x","key:A:-x"] }
      ] }
  ]
})";

static const std::filesystem::path& hermeticRoot() {
    static const std::filesystem::path p = [] {
        std::filesystem::path d = std::filesystem::temp_directory_path()
                                / "engine_sim_intent_root";
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return p;
}

static constexpr uint64_t kPlayer = 0xC0FFEE01ull;
static constexpr float    kSimDt  = 1.0f / 60.0f;

// ── The harness: N ticks of the SAME mouse motion, at a chosen cadence ──────
// Each tick moves the mouse `perTick` counts, delivered in `framesPerTick`
// equal pieces. At 1 frame/tick that is one event of 12; at 2 frames/tick it is
// two of 6, pumped on separate frames. The physical motion is identical and the
// frame cadence is not, which is the whole question.
static std::vector<Intent> runCadence(int framesPerTick, int nTicks,
                                      int perTick) {
    EngineConfig cfg;
    cfg.openAssetDatabase = false;
    cfg.autoDetectProject = false;
    cfg.defaultScene      = false;
    cfg.projectRoot       = hermeticRoot();

    EngineRuntime engine;
    std::vector<Intent> out;
    if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) return out;

    // A replay source, so the "device" is a script and the test is hermetic.
    auto src   = std::make_unique<ReplaySource>();
    auto* raw  = src.get();
    raw->addDevice({1, hid::DeviceClass::Mouse,    0x1234, 0x5678, 42, "mouse"});
    raw->addDevice({2, hid::DeviceClass::Keyboard, 0, 0, 43, "kbd"});
    engine.inputManager().initWithSource(std::move(src));
    engine.inputManager().loadConfigText(kConfig);

    engine.attachPlugins();
    flecs::world& w = engine.simWorld();
    w.entity().set<Transform>({{0,0,0},{0,0,0,1},{1,1,1}})
              .set<Name>({"player"}).set<EntityId>({kPlayer});

    engine.actionSet().declare("Fire");
    engine.actionSet().declare("Jump");
    engine.setLocalController(kPlayer);
    engine.setCommandRecording(true, 256);

    engine.startSimulation(EngineRuntime::SimMode::InPlace);

    const float dt    = kSimDt / (float)framesPerTick;
    const int   piece = perTick / framesPerTick;
    uint64_t    stamp = 1;
    for (int t = 0; t < nTicks; ++t) {
        for (int f = 0; f < framesPerTick; ++f) {
            // {timeNs, deviceId, type, flags, code, value, value2}
            raw->addEvent({ stamp++, 1, hid::EventType::MouseMotion, 0, 0,
                            piece, -piece });
            engine.tick(dt);
        }
    }

    // Newest first out of the ring; reverse so index 0 is the FIRST tick.
    for (int t = nTicks - 1; t >= 0; --t) {
        const auto& rec = engine.recordedTick((size_t)t);
        for (const Intent& i : rec.intents) out.push_back(i);
    }

    engine.stopSimulation();
    engine.shutdown();
    return out;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("sim_intent_test — what was asked, sampled once per tick\n");

    // ── 1. The POD contract ────────────────────────────────────────────────
    // Same three destinations as SimCommand — a hash, a replay file, a wire —
    // so the same discipline. An indeterminate padding byte is a false
    // divergence at best.
    {
        std::printf("\n-- 1. the record is a POD --\n");
        CHECK(sizeof(Intent) == 48, "48 bytes (%zu)", sizeof(Intent));
        CHECK(std::is_trivially_copyable<Intent>::value, "trivially copyable");

        alignas(8) unsigned char bufA[sizeof(Intent)];
        alignas(8) unsigned char bufB[sizeof(Intent)];
        std::memset(bufA, 0xAA, sizeof bufA);
        std::memset(bufB, 0x55, sizeof bufB);
        Intent* a = new (bufA) Intent{};
        Intent* b = new (bufB) Intent{};
        CHECK(std::memcmp(a, b, sizeof(Intent)) == 0,
              "two default-constructed intents built in DIFFERENT dirty memory "
              "are byte-identical — no padding leaks into a hash");
    }

    // ── 2. The submission window ───────────────────────────────────────────
    {
        std::printf("\n-- 2. the submission window --\n");
        simintent::Buffer b;
        Intent i{}; i.entity = 7;
        b.setSubmissionOpen(false);
        CHECK(!b.submit(i), "an intent submitted outside the tick is REFUSED");
        CHECK(b.refusedOutOfPhase() == 1, "and counted");
        b.setSubmissionOpen(true);
        CHECK(b.submit(i), "and accepted inside it");
        Intent zero{};
        CHECK(!b.submit(zero), "entity 0 is refused — it names nothing on replay");
        b.clear();
        CHECK(!b.submissionOpen(), "clear() leaves the window shut");
    }

    // ── 3. The action set is an ORDERED declaration ────────────────────────
    // The order is the bit index, so it is the meaning. Two lists with the same
    // names in a different order must hash differently, or a stream recorded
    // against one and replayed against the other acts on the wrong actions and
    // is debugged as a logic bug.
    {
        std::printf("\n-- 3. the action set --\n");
        simintent::ActionSet s1, s2, s3;
        CHECK(s1.declare("Fire") == 0 && s1.declare("Jump") == 1,
              "declaration order is the bit index");
        CHECK(s1.declare("Fire") == 0,
              "re-declaring returns the existing index rather than a new bit");

        s2.declare("Fire"); s2.declare("Jump");
        CHECK(s1.declarationHash() == s2.declarationHash(),
              "the same list hashes the same");

        s3.declare("Jump"); s3.declare("Fire");
        CHECK(s1.declarationHash() != s3.declarationHash(),
              "the SAME NAMES in a different order hash DIFFERENTLY — the order "
              "assigns the bits, so this is a different meaning, not a "
              "different spelling");
    }

    // ── 4. THE PROPERTY: the intent stream is a function of the TICK ───────
    {
        std::printf("\n-- 4. frame-rate invariance --\n");
        const int nTicks = 8, perTick = 12;
        std::vector<Intent> one = runCadence(1, nTicks, perTick);
        std::vector<Intent> two = runCadence(2, nTicks, perTick);

        CHECK(one.size() == (size_t)nTicks,
              "one intent per tick at 1 frame/tick (%zu)", one.size());
        CHECK(two.size() == (size_t)nTicks,
              "one intent per tick at 2 frames/tick too (%zu) — NOT one per "
              "frame, which is what reading the device in onFrame gives",
              two.size());

        // Skip tick 0: the first tick's delta depends on what the cursor was
        // re-based to at session start, which is a startup detail rather than
        // the property. Ticks 1..N-1 are the steady state.
        int mismatches = 0;
        float sumOne = 0.0f, sumTwo = 0.0f;
        for (size_t i = 1; i < one.size() && i < two.size(); ++i) {
            sumOne += one[i].lookDx;
            sumTwo += two[i].lookDx;
            if (one[i].lookDx != two[i].lookDx) ++mismatches;
            if (one[i].lookDy != two[i].lookDy) ++mismatches;
        }
        CHECK(mismatches == 0,
              "the per-tick look delta is IDENTICAL at 1 and 2 frames per tick "
              "(%d mismatches) — the same physical motion, two cadences, one "
              "simulation", mismatches);
        CHECK(std::fabs(sumOne - sumTwo) < 1e-4f && sumOne > 0.0f,
              "and it is not identically zero (%.1f vs %.1f) — a sampler that "
              "never read the device would pass the line above",
              (double)sumOne, (double)sumTwo);
        CHECK(std::fabs(one[1].lookDx - (float)perTick) < 1e-4f,
              "each tick carries the WHOLE tick's motion (%.1f of %d), so two "
              "frames' worth is summed rather than one of them dropped",
              (double)one[1].lookDx, perTick);

        // Whole-record equality, not just the field the property is about: the
        // entity, the source and the seq must line up too, or the streams are
        // not comparable and the check above is weaker than it reads.
        bool identical = one.size() == two.size();
        for (size_t i = 1; identical && i < one.size(); ++i)
            identical = std::memcmp(&one[i], &two[i], sizeof(Intent)) == 0;
        CHECK(identical,
              "and the recorded intents are BYTE-identical, not merely equal "
              "in the field under test");
    }

    // ── 5. The action list TRAVELS WITH THE RECORD ─────────────────────────
    // Intent::held/pressed/released are bit indices into ActionSet's
    // declaration order, so a recorded stream is uninterpretable without the
    // list that assigned them — and worse than uninterpretable if replayed
    // against a DIFFERENT list, because the bits are silently re-read as other
    // actions and the result presents as a logic bug.
    //
    // sim_intent.h states in three places that declarationHash travels with the
    // stream so that mismatch is DETECTED. Section 3 above proves the hash is
    // sensitive to the list; it did not prove the hash reaches a record,
    // because nothing stored it. A mechanism built, tested in isolation and not
    // wired is the shape this tree keeps finding — so this asserts the wiring
    // rather than the hash.
    {
        std::printf("\n-- 5. the action list travels with the record --\n");
        EngineConfig cfg;
        cfg.openAssetDatabase = false;
        cfg.autoDetectProject = false;
        cfg.defaultScene      = false;
        cfg.projectRoot       = hermeticRoot();

        EngineRuntime engine;
        if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) {
            CHECK(false, "engine init");
        } else {
            auto  src = std::make_unique<ReplaySource>();
            auto* raw = src.get();
            raw->addDevice({1, hid::DeviceClass::Mouse, 0x1234, 0x5678, 42, "mouse"});
            engine.inputManager().initWithSource(std::move(src));
            engine.inputManager().loadConfigText(kConfig);
            engine.attachPlugins();
            engine.simWorld().entity()
                .set<Transform>({{0,0,0},{0,0,0,1},{1,1,1}})
                .set<Name>({"player"}).set<EntityId>({kPlayer});
            engine.actionSet().declare("Fire");
            engine.actionSet().declare("Jump");
            engine.setLocalController(kPlayer);
            engine.setCommandRecording(true, 64);
            engine.startSimulation(EngineRuntime::SimMode::InPlace);

            const uint64_t live = engine.actionSet().declarationHash();
            uint64_t stamp = 1;
            for (int t = 0; t < 8; ++t) {
                raw->addEvent({ stamp++, 1, hid::EventType::MouseMotion,
                                0, 0, 5, -3 });
                engine.tick(kSimDt);
            }

            CHECK(live != 0, "the declared list hashes to something (%llu)",
                  (unsigned long long)live);
            int carried = 0, ticksSeen = 0;
            for (int t = 0; t < 8; ++t) {
                const auto& rec = engine.recordedTick((size_t)t);
                if (rec.tick == 0) continue;      // empty slot
                ++ticksSeen;
                if (rec.actionHash == live) ++carried;
            }
            CHECK(ticksSeen > 0, "the ring holds recorded ticks (%d)", ticksSeen);
            CHECK(carried == ticksSeen,
                  "and every one carries the action list's hash (%d of %d) — "
                  "without this the bits in held/pressed/released name nothing",
                  carried, ticksSeen);

            // ...and a different list would be DETECTED, which is the point of
            // carrying it. Compared against a set built the same way, so this
            // fails if declarationHash ever stops depending on the list.
            simintent::ActionSet other;
            other.declare("Fire");
            other.declare("Crouch");          // Jump -> Crouch: bit 1 changes meaning
            CHECK(other.declarationHash() != live,
                  "a stream recorded against a different list would not match "
                  "(%llu vs %llu)",
                  (unsigned long long)other.declarationHash(),
                  (unsigned long long)live);

            engine.stopSimulation();
            engine.shutdown();
        }
    }

    if (g_failures) {
        std::printf("\nsim_intent_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nsim_intent_test: ALL PASS\n");
    return 0;
}
