// profiler_test — headless correctness check for core/profiler.h.
//
// Proves the profiler works with no GPU, no ECS, no editor — i.e. that it
// truly lives in engine_core. A scope wrapping a known sleep must report that
// duration; nested scopes must report the right tree. Exit code 0 = pass.
#include "core/profiler.h"

#include <chrono>
#include <cstdio>
#include <thread>

static void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int main() {
#if !ENGINE_PROFILE
    std::printf("profiler_test: ENGINE_PROFILE disabled — nothing to test\n");
    return 0;
#else
    auto& prof = prof::Profiler::get();
    prof.setEnabled(true);

    prof.beginFrame();
    {
        ENGINE_PROFILE_SCOPE("outer");
        sleepMs(3);
        {
            ENGINE_PROFILE_SCOPE("inner");
            sleepMs(2);
        }
    }
    prof.endFrame();

    auto& timer = prof.timer();
    timer.logLastFrame("Test");

    // Sleep is a LOWER bound only — the OS may delay a thread arbitrarily, so a
    // tight upper bound measures the machine's scheduler, not the profiler. A
    // 25ms ceiling on a 5ms sleep duly went red on a contended CI runner. The
    // upper bounds are kept only loose enough to catch a UNITS error (recording
    // ns or us as ms would be off by 1000x+), which is the one real bug an
    // upper bound can find here.
    double outerMs = -1, innerMs = -1;
    int    outerDepth = -1, innerDepth = -1;
    uint32_t outerIdx = prof::kNoParent, innerParent = prof::kNoParent;
    const auto& frame = timer.lastFrame();
    for (uint32_t i = 0; i < frame.size(); ++i) {
        const auto& s = frame[i];
        double ms = (s.end - s.start) / 1e6;
        if (std::string(s.name) == "outer") { outerMs = ms; outerDepth = s.depth; outerIdx = i; }
        if (std::string(s.name) == "inner") { innerMs = ms; innerDepth = s.depth; innerParent = s.parent; }
    }

    bool ok = true;
    auto check = [&](const char* what, bool cond) {
        std::printf("  %-28s %s\n", what, cond ? "PASS" : "FAIL");
        ok = ok && cond;
    };
    check("outer recorded",        outerMs >= 0);
    check("inner recorded",        innerMs >= 0);
    constexpr double kUnitsSanityMs = 2000.0;   // not a timing bound
    check("outer >= 4ms (3+2 slept)", outerMs >= 4.0 && outerMs <= kUnitsSanityMs);
    check("inner >= 1.5ms",           innerMs >= 1.5 && innerMs <= kUnitsSanityMs);
    check("outer contains inner",  outerMs >= innerMs);
    check("outer depth 0",         outerDepth == 0);
    check("inner depth 1 (nested)", innerDepth == 1);
    check("inner.parent == outer (id)", innerParent == outerIdx);

    // Dormant path: a scope run while disabled must record nothing. (begin/
    // endFrame are zero-cost no-ops when disabled, so the snapshot keeps the
    // last enabled frame — the property under test is that no NEW "ignored"
    // sample was created, not that the snapshot is wiped.)
    prof.setEnabled(false);
    prof.beginFrame();
    { ENGINE_PROFILE_SCOPE("ignored"); sleepMs(1); }
    prof.endFrame();
    bool ignoredFound = false;
    for (const auto& s : timer.lastFrame())
        if (std::string(s.name) == "ignored") ignoredFound = true;
    check("dormant scope records nothing", !ignoredFound);

    // Overflow: blow past the per-thread cap; samples must drop (not grow
    // unboundedly) and the frame must report it.
    prof.setEnabled(true);
    prof.beginFrame();
    for (uint32_t i = 0; i < prof::TimerChannel::kMaxSamplesPerThread + 200; ++i)
        { ENGINE_PROFILE_SCOPE("flood"); }
    prof.endFrame();
    check("overflow capped at budget",
          timer.lastFrame().size() <= prof::TimerChannel::kMaxSamplesPerThread);
    check("overflow reported (dropped>0)", timer.lastFrameDropped() > 0);

    std::printf("profiler_test: %s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
#endif
}
