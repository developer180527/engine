#pragma once
// ── Making a hang say where it hung ─────────────────────────────────────────
//
// Two tests time out on Windows — input_test and cook_infra_test — and reported
// NOTHING. input_test prints on the first line of main() and even that was
// missing, which is the tell: stdout is BLOCK-BUFFERED when redirected (ctest
// always redirects), so everything a test printed sits in a 4 KB buffer that the
// timeout kill discards. A 120-second silence is not evidence, it is the absence
// of evidence, and no amount of reading the source substitutes for it.
//
// Two mechanisms, because they fail differently:
//
//   1. UNBUFFERED STDOUT (every test, not just these two). Then whatever ran
//      before the hang survives the kill, and the last line printed brackets the
//      problem. This is the load-bearing half.
//
//   2. THIS WATCHDOG, for when the hang is inside a call that prints nothing.
//      A thread fires BEFORE ctest's own timeout, names the last phase reached,
//      and exits. ctest's kill tells you a test hung; this tells you where.
//
// Exits with _Exit rather than abort(): on MSVC, abort() can raise the CRT error
// dialog or hand off to Windows Error Reporting, and a CI runner then waits on a
// dialog nobody will click — turning a diagnosable hang into a longer one.
//
// Costs nothing when the test passes: one sleeping thread, disarmed at the end.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace testwd {

inline std::atomic<const char*> g_phase{"(before the first phase marker)"};
inline std::atomic<bool>        g_done{false};

// Narrate progress. Printed immediately (stdout is unbuffered by `begin`), so
// the phase trail survives a kill even if the watchdog never fires.
inline void phase(const char* name) {
    g_phase.store(name, std::memory_order_relaxed);
    std::printf("[phase] %s\n", name);
}

// Fire after `seconds` if the test has not finished. Keep this comfortably below
// the ctest TIMEOUT for the test, or ctest kills the process first and the whole
// point is lost.
inline void arm(int seconds) {
    static std::once_flag once;
    std::call_once(once, [seconds] {
        std::thread([seconds] {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            while (std::chrono::steady_clock::now() < deadline) {
                if (g_done.load(std::memory_order_relaxed)) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (g_done.load(std::memory_order_relaxed)) return;
            const char* p = g_phase.load(std::memory_order_relaxed);
            std::fflush(stdout);
            std::fprintf(stderr,
                "\n*** WATCHDOG: still running after %d s.\n"
                "*** Last phase reached: %s\n"
                "*** Everything printed above this line completed; the hang is "
                "after it.\n", seconds, p ? p : "(none)");
            std::fflush(stderr);
            std::_Exit(97);
        }).detach();
    });
}

// Unbuffered stdout + the watchdog + a banner, in the order that matters:
// unbuffering FIRST, so the banner itself cannot be lost.
inline void begin(const char* testName, int watchdogSeconds = 60) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("%s\n", testName);
    arm(watchdogSeconds);
}

// Call before returning from main, so the watchdog cannot fire during teardown.
inline void end() { g_done.store(true, std::memory_order_relaxed); }

} // namespace testwd
