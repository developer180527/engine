// ── logger_test — the ring, the filter, and the loss ────────────────────────
//
// The logger was rewritten because the old one cost 2.44 µs per line, 88% of it
// in a `std::vector::erase(begin())` masquerading as a ring buffer. Speed is not
// what this file asserts — a benchmark measures that. What it asserts is the
// three properties the speed depends on, each of which fails SILENTLY:
//
//   FILTERING must stop the work, not just hide the output. If a suppressed line
//   still formats, "target one subsystem" costs the same as logging everything
//   and nothing in the output reveals it.
//   RESERVATION must not lose lines under concurrent writers. A dropped
//   fetch_add is invisible: the log simply has fewer lines than happened, which
//   is indistinguishable from the code not running.
//   LOSS must be counted exactly. A ring drops the oldest by design; a console
//   that cannot say how many is lying about what it shows.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "core/logger.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("logger_test\n");
    elog::setStdout(false);        // the ring is what is under test, not stdio

    // ── Categories are per tag, discovered on use ───────────────────────────
    {
        elog::Category* a = elog::category("TestAlpha");
        elog::Category* b = elog::category("TestAlpha");
        elog::Category* c = elog::category("TestBeta");
        CHECK(a == b, "the same tag resolves to one category");
        CHECK(a != c, "a different tag gets its own");
        // A distinct pointer with equal text must still collapse — two TUs can
        // hold separate copies of the same literal.
        char dyn[] = "TestAlpha";
        CHECK(elog::category(dyn) == a,
              "equal text from a different address is the same category");
    }

    // ── A line is recorded and reads back intact ────────────────────────────
    {
        const uint64_t before = elog::written();
        LOG_INFO("TestAlpha", "hello %d %s", 42, "world");
        CHECK(elog::written() == before + 1, "one line written");
        elog::Entry e;
        CHECK(elog::read(before, e), "and it reads back");
        CHECK(std::strcmp(e.msg, "hello 42 world") == 0,
              "with the formatted text ('%s')", e.msg);
        CHECK(e.level == elog::Level::Info, "and the level");
        CHECK(e.cat && std::strcmp(e.cat, "TestAlpha") == 0, "and the category");
    }

    // ── FILTERING STOPS THE WORK ────────────────────────────────────────────
    // Asserted through written(), not through the output: if a suppressed line
    // still reserved a slot it would advance the counter, which is exactly the
    // "formats then discards" implementation this replaced.
    {
        elog::Category* c = elog::category("TestAlpha");
        elog::setLevel(*c, elog::Level::Info, false);
        const uint64_t before = elog::written();
        for (int i = 0; i < 1000; ++i) LOG_INFO("TestAlpha", "suppressed %d", i);
        CHECK(elog::written() == before,
              "1000 filtered lines reserve NO slots (%llu)",
              (unsigned long long)(elog::written() - before));

        // The argument expressions must not even be evaluated.
        int sideEffects = 0;
        auto bump = [&] { ++sideEffects; return 7; };
        for (int i = 0; i < 100; ++i) LOG_INFO("TestAlpha", "%d", bump());
        CHECK(sideEffects == 0,
              "a filtered line does not evaluate its arguments (%d)", sideEffects);

        // Errors from the same category still get through — the mask is per
        // level, which is what lets you watch one subsystem's info and
        // everything's errors at once.
        const uint64_t b2 = elog::written();
        LOG_ERROR("TestAlpha", "still recorded");
        CHECK(elog::written() == b2 + 1, "a different level on the same tag passes");
        elog::setLevel(*c, elog::Level::Info, true);
    }

    // ── solo(): stream one subsystem, keep everyone's errors ────────────────
    {
        elog::Category* target = elog::category("TestBeta");
        elog::solo(target);
        const uint64_t before = elog::written();
        LOG_INFO("TestBeta",  "watched");
        LOG_INFO("TestAlpha", "not watched");
        LOG_ERROR("TestAlpha", "an error always survives");
        CHECK(elog::written() == before + 2,
              "solo records the target's info and other tags' errors, nothing else "
              "(%llu of 3)", (unsigned long long)(elog::written() - before));
        elog::watchAll();
    }

    // ── Truncation is bounded and counted ───────────────────────────────────
    {
        const uint64_t t0 = elog::truncated();
        char big[elog::kMsgMax * 3];
        std::memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        LOG_INFO("TestAlpha", "%s", big);
        elog::Entry e;
        CHECK(elog::read(elog::written() - 1, e), "an over-long line is recorded");
        CHECK(std::strlen(e.msg) == elog::kMsgMax - 1,
              "clamped to the slot (%zu)", std::strlen(e.msg));
        CHECK(e.truncated, "and flagged as truncated");
        CHECK(elog::truncated() == t0 + 1, "and counted");
    }

    // ── The ring wraps, and the loss is EXACT ───────────────────────────────
    {
        const uint64_t start = elog::written();
        const uint64_t n = elog::kSlots * 2 + 17;      // lap it twice
        for (uint64_t i = 0; i < n; ++i) LOG_INFO("TestAlpha", "wrap %llu",
                                                  (unsigned long long)i);
        const uint64_t total = elog::written();
        CHECK(total == start + n, "every line was reserved a slot (%llu of %llu)",
              (unsigned long long)(total - start), (unsigned long long)n);
        CHECK(elog::evicted() == total - elog::kSlots,
              "evicted() is exact: %llu written, %u slots, %llu gone",
              (unsigned long long)total, elog::kSlots,
              (unsigned long long)elog::evicted());

        // Everything still in the ring must read; everything older must refuse.
        elog::Entry e;
        uint64_t readable = 0;
        for (uint64_t s = elog::oldest(); s < total; ++s) if (elog::read(s, e)) ++readable;
        CHECK(readable == elog::kSlots,
              "the whole ring is readable (%llu of %u)",
              (unsigned long long)readable, elog::kSlots);
        CHECK(elog::oldest() == 0 || !elog::read(elog::oldest() - 1, e),
              "an evicted sequence is refused, not returned as stale text");
        CHECK(!elog::read(total, e), "and so is one that has not happened yet");
        CHECK(!elog::read(total + 10000, e), "...however far ahead");
    }

    // ── Concurrent writers lose nothing ─────────────────────────────────────
    // Eight threads is the shape that matters: job workers log, and the old
    // implementation serialized them all on one mutex behind a 1024-element
    // memmove. Run this under TSan for the ordering to be fully checked; even
    // without it, a lost or double-used reservation shows up as a bad count.
    {
        constexpr int kThreads = 8, kEach = 4000;
        const uint64_t before = elog::written();
        std::atomic<bool> go{false};
        std::vector<std::thread> th;
        for (int k = 0; k < kThreads; ++k) th.emplace_back([&, k] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kEach; ++i) LOG_WARN("TestMT", "t%d %d", k, i);
        });
        go.store(true, std::memory_order_release);
        for (auto& t : th) t.join();
        CHECK(elog::written() == before + (uint64_t)kThreads * kEach,
              "%d threads x %d lines all reserved distinct slots (%llu of %d)",
              kThreads, kEach,
              (unsigned long long)(elog::written() - before), kThreads * kEach);

        elog::Category* mt = elog::category("TestMT");
        CHECK(mt->written.load() == (uint64_t)kThreads * kEach,
              "and the per-category count agrees (%llu)",
              (unsigned long long)mt->written.load());

        // Whatever survives in the ring must be a WHOLE record from one writer,
        // never a splice of two — that is what the sequence stamp is for.
        elog::Entry e;
        bool wellFormed = true;
        for (uint64_t s = elog::oldest(); s < elog::written(); ++s) {
            if (!elog::read(s, e)) continue;
            int tid = -1, line = -1;
            if (e.cat && std::strcmp(e.cat, "TestMT") == 0 &&
                (std::sscanf(e.msg, "t%d %d", &tid, &line) != 2 ||
                 tid < 0 || tid >= kThreads || line < 0 || line >= kEach))
                wellFormed = false;
        }
        CHECK(wellFormed, "no record read back is a splice of two writers");
    }

    if (g_failures) {
        std::printf("\nlogger_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("\nlogger_test: PASS\n");
    return 0;
}
