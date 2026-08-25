// ── input_test — InputManager gauntlet (no hardware needed) ─────────────────
// ReplaySource drives scripted event streams through the full policy layer:
//   * endpoint ELECTION: duplicate HID collections (same physId) must not
//     double-count clicks or motion;
//   * SNAPSHOT semantics: tick boundaries respected (events after tickEnd
//     stay staged), held state persists, deltas reset per tick;
//   * DETERMINISM: the same stream replayed twice yields bit-identical
//     snapshots — the multiplayer contract;
//   * ACTIONS: digital edges (pressed/released), axis2 from keys and motion,
//     context stack resolution with blockLower;
//   * LATE-LATCH: consumeLook drains everything pumped, including events
//     newer than the last tick, then reads zero.
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "runtime/input/input_manager.h"
#include "test_watchdog.h"

static int g_failures = 0;
#define CHECK(cond, msg) do {                                        \
    if (!(cond)) { std::printf("  FAIL  %s\n", msg); ++g_failures; } \
    else         { std::printf("  ok    %s\n", msg); }               \
} while (0)

using namespace input;

static const char* kTestConfig = R"({
  "contexts": [
    { "name": "Gameplay",
      "actions": [
        { "name": "Fire", "type": "digital", "bindings": ["mouse:left"] },
        { "name": "Jump", "type": "digital", "bindings": ["key:Space"] },
        { "name": "Move", "type": "axis2",
          "bindings": ["key:W:+y","key:S:-y","key:D:+x","key:A:-x"] },
        { "name": "Look", "type": "axis2", "bindings": ["mouse:motion"] }
      ] },
    { "name": "Menu", "blockLower": true,
      "actions": [
        { "name": "Confirm", "type": "digital", "bindings": ["key:Enter"] }
      ] }
  ]
})";

// Two mouse endpoints sharing physId 77 (split HID collections of one
// physical mouse) + one keyboard.
static std::unique_ptr<ReplaySource> makeSource(bool duplicateEndpoints) {
    auto src = std::make_unique<ReplaySource>();
    src->addDevice({1, hid::DeviceClass::Mouse,    0x25a7, 0xfaa0, 77, "toad A"});
    if (duplicateEndpoints)
        src->addDevice({2, hid::DeviceClass::Mouse, 0x25a7, 0xfaa0, 77, "toad B"});
    src->addDevice({3, hid::DeviceClass::Keyboard, 0, 0, 110, "kbd"});

    auto ev = [&](uint64_t tMs, hid::DeviceId d, hid::EventType ty,
                  uint16_t code, int32_t v, int32_t v2 = 0) {
        src->addEvent({tMs * 1000000ull, d, ty, 0, code, v, v2});
    };
    // One physical click reported by BOTH endpoints (identical timestamps),
    // motion on both too — election must keep exactly one of each.
    ev(1, 1, hid::EventType::Button, 0, 1);
    if (duplicateEndpoints) ev(1, 2, hid::EventType::Button, 0, 1);
    ev(2, 1, hid::EventType::MouseMotion, 0, 10, -4);
    if (duplicateEndpoints) ev(2, 2, hid::EventType::MouseMotion, 0, 10, -4);
    ev(3, 3, hid::EventType::Key, 0x1A /*W*/, 1);
    ev(4, 1, hid::EventType::Button, 0, 0);
    if (duplicateEndpoints) ev(4, 2, hid::EventType::Button, 0, 0);
    // tick boundary at 5ms — these land in tick 2:
    ev(6, 3, hid::EventType::Key, 0x1A, 0);
    ev(7, 3, hid::EventType::Key, 0x2C /*Space*/, 1);
    ev(8, 1, hid::EventType::MouseMotion, 0, 3, 3);
    return src;
}

static void runStream(InputManager& m, InputSnapshot out[2]) {
    m.pump();
    m.beginTick(5 * 1000000ull);    // tick 1: events <= 5ms
    out[0] = m.snapshot();
    m.beginTick(10 * 1000000ull);   // tick 2: the rest
    out[1] = m.snapshot();
}

int main() {
    // Unbuffered + a watchdog under ctest's 120 s. This test TIMES OUT on
    // Windows having printed NOTHING AT ALL — not even the banner one line below,
    // which is the tell: stdout is block-buffered when ctest redirects it, and
    // the timeout kill discards the buffer. The phase markers name the last
    // section that started.
    testwd::begin("input_test: InputManager gauntlet", 60);

    testwd::phase("election + snapshot semantics");
    // ── Election + snapshot semantics ───────────────────────────────────────
    {
        InputManager m;
        m.initWithSource(makeSource(true));
        m.loadConfigText(kTestConfig);

        InputSnapshot s[2];
        runStream(m, s);

        CHECK(s[0].mouseDx == 10 && s[0].mouseDy == -4,
              "duplicate-endpoint motion counted ONCE (election)");
        CHECK(s[0].keyDown(0x1A), "W held in tick 1");
        CHECK(!s[0].buttonDown(0), "click released within tick 1");
        CHECK(s[0].mouseDx == 10, "post-boundary events NOT in tick 1");
        CHECK(!s[1].keyDown(0x1A) && s[1].keyDown(0x2C),
              "tick 2: W released, Space held (state persists per tick)");
        CHECK(s[1].mouseDx == 3 && s[1].mouseDy == 3,
              "tick 2 deltas reset and re-accumulate");

        float lx, ly;
        m.consumeLook(&lx, &ly);
        CHECK(lx == 13.0f && ly == -1.0f,
              "late-latch look = ALL pumped motion (both ticks), once");
        m.consumeLook(&lx, &ly);
        CHECK(lx == 0.0f && ly == 0.0f, "look accumulator drains on read");
    }

    testwd::phase("reliable hotplug");
    // ── Reliable hotplug: a DROPPED DeviceRemoved must not wedge election ───
    // A device elected for a type, then unplugged with its DeviceRemoved event
    // lost (ring overflow), must be evicted so a replacement with the same
    // physId can take over. Without the generation reconcile the stale election
    // rejects the replacement's motion forever.
    {
        auto src = std::make_unique<ReplaySource>();
        ReplaySource* raw = src.get();
        raw->addDevice({1, hid::DeviceClass::Mouse, 0x1, 0x1, 77, "mouse"});
        raw->addEvent({1'000'000ull, 1, hid::EventType::MouseMotion, 0, 0, 5, 0});

        InputManager m;
        m.initWithSource(std::move(src));
        m.loadConfigText(kTestConfig);
        m.pump(); m.beginTick(1'000'000ull);
        CHECK(m.snapshot().mouseDx == 5, "device 1 elected for motion");

        // Unplug device 1 (gen++) and connect a replacement sharing physId 77
        // (gen++) — but deliver NO DeviceRemoved event, as if the ring dropped
        // it. Then the replacement moves.
        raw->removeDevice(1);
        raw->addDevice({4, hid::DeviceClass::Mouse, 0x1, 0x1, 77, "mouse'"});
        raw->addEvent({2'000'000ull, 4, hid::EventType::MouseMotion, 0, 0, 7, 0});
        m.pump(); m.beginTick(2'000'000ull);
        CHECK(m.snapshot().mouseDx == 7,
              "replacement (same physId) elected after a dropped DeviceRemoved");
    }

    testwd::phase("determinism: same stream twice");
    // ── Determinism: same stream twice -> bit-identical snapshots ───────────
    {
        InputSnapshot a[2], b[2];
        for (InputSnapshot* out : {a, b}) {
            InputManager m;
            m.initWithSource(makeSource(true));
            m.loadConfigText(kTestConfig);
            runStream(m, out);
        }
        CHECK(std::memcmp(a, b, sizeof(a)) == 0,
              "replayed stream -> bit-identical snapshots (netcode contract)");
    }

    testwd::phase("actions: edges, axes, contexts");
    // ── Actions: edges, axes, contexts ──────────────────────────────────────
    {
        InputManager m;
        m.initWithSource(makeSource(false));
        m.loadConfigText(kTestConfig);

        m.pump();
        m.beginTick(3 * 1000000ull);   // W down, click down+held, motion in
        CHECK(m.actionDown("Fire"), "Fire down while button held");
        CHECK(m.actionPressed("Fire"), "Fire pressed edge on first tick");
        float mx, my;
        m.axis2("Move", &mx, &my);
        CHECK(mx == 0.0f && my == 1.0f, "Move axis2 = +y while W held");
        m.axis2("Look", &mx, &my);
        CHECK(mx == 10.0f && my == -4.0f, "Look axis2 from tick motion counts");

        m.beginTick(5 * 1000000ull);   // button release lands here
        CHECK(m.actionReleased("Fire"), "Fire released edge");

        // Menu context blocks gameplay below it.
        m.pushContext("Menu");
        m.beginTick(10 * 1000000ull);  // Space press lands here
        CHECK(!m.actionDown("Jump"), "blockLower context hides gameplay actions");
        m.popContext();
        CHECK(m.actionDown("Jump"), "pop restores gameplay resolution");
    }

    testwd::phase("sub-tick tap");
    // ── Sub-tick tap: press+release inside ONE tick still fires edges ───────
    {
        InputManager m;
        auto src = std::make_unique<ReplaySource>();
        src->addDevice({3, hid::DeviceClass::Keyboard, 0, 0, 110, "kbd"});
        src->addDevice({1, hid::DeviceClass::Mouse,    0, 0, 77,  "mouse"});
        src->addEvent({1000000, 3, hid::EventType::Key,    0, 0x2C, 1, 0});
        src->addEvent({2000000, 3, hid::EventType::Key,    0, 0x2C, 0, 0});
        src->addEvent({1000000, 1, hid::EventType::Button, 0, 0,    1, 0});
        src->addEvent({2000000, 1, hid::EventType::Button, 0, 0,    0, 0});
        src->addEvent({3000000, 1, hid::EventType::Scroll, 0, 0,    2, -5});
        m.initWithSource(std::move(src));
        m.loadConfigText(R"({"contexts":[{"name":"G","actions":[
            {"name":"Jump","type":"digital","bindings":["key:Space"]},
            {"name":"Fire","type":"digital","bindings":["mouse:left"]},
            {"name":"Pan","type":"axis2","bindings":["scroll"]}]}]})");
        m.pump();
        m.beginTick(10 * 1000000ull);   // everything inside one tick
        CHECK(!m.snapshot().keyDown(0x2C), "tap: held state correctly clear");
        CHECK(m.actionPressed("Jump"), "sub-tick key tap fires pressed edge");
        CHECK(m.actionReleased("Jump"), "sub-tick key tap fires released edge");
        CHECK(m.actionPressed("Fire") && m.actionReleased("Fire"),
              "sub-tick click fires both button edges");
        float px, py;
        m.axis2("Pan", &px, &py);
        CHECK(px == 2.0f && py == -5.0f,
              "axis2 scroll binding reaches BOTH components (wheel not lost)");
    }

    testwd::phase("focus gate");
    // ── Focus gate: unfocused drops presses, keeps releases ─────────────────
    {
        InputManager m;
        auto src = std::make_unique<ReplaySource>();
        src->addDevice({3, hid::DeviceClass::Keyboard, 0, 0, 110, "kbd"});
        src->addEvent({1000000, 3, hid::EventType::Key, 0, 0x1A, 1, 0});
        ReplaySource* raw = src.get();
        m.initWithSource(std::move(src));
        m.pump();
        m.beginTick(2 * 1000000ull);
        // now lose focus; release must still land (no stuck keys)
        m.setFocused(false);
        raw->addEvent({3000000, 3, hid::EventType::Key, 0, 0x1A, 0, 0});
        raw->addEvent({3000001, 3, hid::EventType::Key, 0, 0x2C, 1, 0});
        m.pump();
        m.beginTick(4 * 1000000ull);
        CHECK(!m.snapshot().keyDown(0x1A), "release applies while unfocused");
        CHECK(!m.snapshot().keyDown(0x2C), "press dropped while unfocused");
    }

    testwd::phase("sub-tick edges");
    // ── Sub-tick edges: exact in-tick timing of presses (CS2-style) ─────────
    {
        InputManager m;
        auto src = std::make_unique<ReplaySource>();
        src->addDevice({3, hid::DeviceClass::Keyboard, 0, 0, 110, "kbd"});
        src->addDevice({1, hid::DeviceClass::Mouse,    0, 0, 77,  "mouse"});
        src->addEvent({ 3200000, 3, hid::EventType::Key,    0, 0x2C, 1, 0}); // 3.2ms
        src->addEvent({ 7800000, 1, hid::EventType::Button, 0, 0,    1, 0}); // 7.8ms
        src->addEvent({ 9100000, 1, hid::EventType::Button, 0, 0,    0, 0}); // 9.1ms
        m.initWithSource(std::move(src));
        m.loadConfigText(R"({"contexts":[{"name":"G","actions":[
            {"name":"Jump","type":"digital","bindings":["key:Space"]},
            {"name":"Fire","type":"digital","bindings":["mouse:left"]}]}]})");
        m.pump();
        m.beginTick(16 * 1000000ull);   // tick spans 0..16ms
        const auto& s = m.snapshot();
        CHECK(s.edgeCount == 3, "three sub-tick edges recorded");
        CHECK(s.edges[0].offsetUs == 3200 && s.edges[0].down == 1,
              "Space press at exactly 3200us into the tick");
        CHECK(s.edges[1].offsetUs == 7800 && s.edges[1].kind == 1,
              "Fire press at exactly 7800us");
        CHECK(s.edges[2].offsetUs == 9100 && s.edges[2].down == 0,
              "Fire release at exactly 9100us");
        CHECK(m.actionPressedOffsetUs("Jump") == 3200,
              "actionPressedOffsetUs resolves Jump to 3200us");
        CHECK(m.actionPressedOffsetUs("Fire") == 7800,
              "actionPressedOffsetUs resolves Fire to 7800us");
    }

    testwd::phase("recorder round-trip");
    // ── Recorder round-trip: record -> replay -> bit-identical snapshots ────
    {
        // fs::temp_directory_path(), not "/tmp". The hardcoded POSIX path is why
        // this test HUNG on Windows for 120 s: there is no /tmp, fopen returned
        // nullptr, and fread on a null FILE* trips the debug CRT's invalid-
        // parameter handler — which defaults to _CRTDBG_MODE_WNDW, a MODAL
        // DIALOG, on a CI runner with nobody to click it. A wrong path became an
        // unkillable wait rather than a failed assertion.
        const std::string recPathStr =
            (std::filesystem::temp_directory_path() / "input_test_session.irec").string();
        const char* recPath = recPathStr.c_str();
        InputSnapshot recorded[2];
        {
            InputManager m;
            m.initWithSource(makeSource(true));
            m.loadConfigText(kTestConfig);
            m.startRecording(recPath);
            runStream(m, recorded);
            CHECK(m.recording(), "recording active through the session");
            const auto lat = m.takeLatencyStats();
            CHECK(lat.events > 0, "latency tracker counted accepted events");
            m.stopRecording();
        }
        // Read the .irec: header, events, tick markers (type None, code 0x7C4).
        std::vector<hid::Event> evs;
        std::vector<uint64_t>   ticks;
        {
            FILE* f = fopen(recPath, "rb");
            CHECK(f != nullptr, "recording file exists");
            // BAIL, do not continue. Every fread below takes `f`, and fread on a
            // null FILE* is undefined — on MSVC's debug CRT it is an invalid-
            // parameter assertion, not a quiet return. A CHECK that records a
            // failure and then walks into UB reports the wrong problem, if it
            // manages to report anything at all.
            if (!f) { testwd::end(); std::printf("input_test: 1 FAILURE(S)\n"); return 1; }
            uint32_t magic = 0, ver = 0;
            fread(&magic, 4, 1, f); fread(&ver, 4, 1, f);
            CHECK(magic == 0x43455249 && ver == 1, "IREC header valid");
            hid::Event e;
            while (fread(&e, sizeof(e), 1, f) == 1) {
                if (e.type == hid::EventType::None && e.code == 0x7C4)
                    ticks.push_back(e.timeNs);
                else evs.push_back(e);
            }
            fclose(f);
        }
        CHECK(ticks.size() == 2, "two tick boundaries recorded");
        // Replay through a FRESH manager: same filters, same order.
        {
            auto src = std::make_unique<ReplaySource>();
            // Devices must exist for election parity with the live session.
            src->addDevice({1, hid::DeviceClass::Mouse,    0x25a7, 0xfaa0, 77, "toad A"});
            src->addDevice({2, hid::DeviceClass::Mouse,    0x25a7, 0xfaa0, 77, "toad B"});
            src->addDevice({3, hid::DeviceClass::Keyboard, 0, 0, 110, "kbd"});
            for (const auto& e : evs) src->addEvent(e);
            InputManager m;
            m.initWithSource(std::move(src));
            m.loadConfigText(kTestConfig);
            m.pump();
            InputSnapshot replayed[2];
            m.beginTick(ticks[0]); replayed[0] = m.snapshot();
            m.beginTick(ticks[1]); replayed[1] = m.snapshot();
            CHECK(std::memcmp(recorded, replayed, sizeof(recorded)) == 0,
                  "REPLAY -> bit-identical snapshots (recorder harness works)");
        }
        std::remove(recPath);
    }

    testwd::end();

    if (g_failures) { std::printf("input_test: FAIL (%d)\n", g_failures); return 1; }
    std::printf("input_test: PASS — election, snapshots, determinism, actions\n");
    return 0;
}
