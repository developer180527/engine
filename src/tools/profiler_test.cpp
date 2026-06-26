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

    // Verify durations land within a tolerance band (sleep is a lower bound;
    // scheduling adds slack, so use a generous upper bound).
    double outerMs = -1, innerMs = -1;
    int    outerDepth = -1, innerDepth = -1;
    for (const auto& s : timer.lastFrame()) {
        double ms = (s.end - s.start) / 1e6;
        if (std::string(s.name) == "outer") { outerMs = ms; outerDepth = s.depth; }
        if (std::string(s.name) == "inner") { innerMs = ms; innerDepth = s.depth; }
    }

    bool ok = true;
    auto check = [&](const char* what, bool cond) {
        std::printf("  %-28s %s\n", what, cond ? "PASS" : "FAIL");
        ok = ok && cond;
    };
    check("outer recorded",        outerMs >= 0);
    check("inner recorded",        innerMs >= 0);
    check("outer ~5ms (3+2)",      outerMs >= 4.0 && outerMs <= 25.0);
    check("inner ~2ms",            innerMs >= 1.5 && innerMs <= 20.0);
    check("outer contains inner",  outerMs >= innerMs);
    check("outer depth 0",         outerDepth == 0);
    check("inner depth 1 (nested)", innerDepth == 1);

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

    std::printf("profiler_test: %s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
#endif
}
