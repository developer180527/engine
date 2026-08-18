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

    // ── A reader running WHILE writers write ────────────────────────────────
    // The section above joins every writer before reading, so it never exercised
    // the path the stamp protocol exists for: a console draining the ring while
    // the engine logs into it. That is the case the in-progress poison and its
    // release fence are for, and it has to be in the test or those fixes are
    // asserted by comment only.
    //
    // What is checked is the PROTOCOL's guarantee, which is not "every line is
    // readable" — a fast writer legitimately laps a reader — but "anything read
    // is a whole record from one writer". A splice would show up as a message
    // that does not parse.
    {
        constexpr int kThreads = 6, kEach = 20000;
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> readOk{0}, readSkipped{0}, spliced{0};

        std::vector<std::thread> writers;
        for (int k = 0; k < kThreads; ++k) writers.emplace_back([k] {
            for (int i = 0; i < kEach; ++i) LOG_INFO("TestRace", "w%d %d", k, i);
        });
        std::thread reader([&] {
            elog::Entry e;
            while (!stop.load(std::memory_order_relaxed)) {
                const uint64_t total = elog::written();
                const uint64_t from  = total > 64 ? total - 64 : 0;
                for (uint64_t s2 = from; s2 < total; ++s2) {
                    if (!elog::read(s2, e)) { readSkipped.fetch_add(1); continue; }
                    readOk.fetch_add(1);
                    if (e.cat && std::strcmp(e.cat, "TestRace") == 0) {
                        int tid = -1, line = -1;
                        if (std::sscanf(e.msg, "w%d %d", &tid, &line) != 2 ||
                            tid < 0 || tid >= kThreads || line < 0 || line >= kEach)
                            spliced.fetch_add(1);
                    }
                }
            }
        });
        for (auto& t : writers) t.join();
        stop.store(true, std::memory_order_relaxed);
        reader.join();

        CHECK(readOk.load() > 0,
              "the concurrent reader actually read records (%llu ok, %llu skipped)",
              (unsigned long long)readOk.load(),
              (unsigned long long)readSkipped.load());
        // Skipping is PERMITTED, not required — asserting it happens would pin an
        // implementation artifact rather than the contract. An earlier draft did
        // assert it and started failing the moment the writer got faster: the
        // sequence is now reserved AFTER formatting, so the window in which a
        // reader can catch a slot mid-write is a memcpy rather than a vsnprintf,
        // and the reader stopped losing races it used to lose. The refusal logic
        // itself is covered deterministically by the ring-wrap section above.
        std::printf("  note  reader skipped %llu of %llu (0 is fine — it means the "
                    "write window is narrow)\n",
                    (unsigned long long)readSkipped.load(),
                    (unsigned long long)(readOk.load() + readSkipped.load()));
        CHECK(spliced.load() == 0,
              "and not one record it accepted was a splice of two writers (%llu)",
              (unsigned long long)spliced.load());
    }

    // ── AUDIENCE: two consoles, one ring ────────────────────────────────────
    // The game console shows game-facing categories plus warnings and errors
    // from EVERYWHERE; the internal console shows the machinery. The asymmetry is
    // the safety property: a category nobody remembered to mark can cost noise or
    // silence at info level, and can NEVER hide a failure from the person whose
    // build is broken.
    {
        elog::Category* eng  = elog::category("TestEngineSide");
        elog::Category* game = elog::category("TestGameSide");
        elog::setAudience(*game, elog::Audience::Game);

        CHECK(elog::audienceOf(*eng) == elog::Audience::Engine,
              "a category defaults to Engine");
        CHECK(elog::audienceOf(*game) == elog::Audience::Game,
              "and can be marked game-facing");

        // Info: game-facing only.
        CHECK(elog::visibleToGame(game, elog::Level::Info),
              "the game console sees a game-facing INFO line");
        CHECK(!elog::visibleToGame(eng, elog::Level::Info),
              "and does NOT see engine INFO chatter");
        CHECK(!elog::visibleToGame(eng, elog::Level::Success),
              "nor engine success lines");
        CHECK(!elog::visibleToGame(eng, elog::Level::Debug),
              "nor engine debug");

        // THE RULE THAT MAKES THE SPLIT SAFE.
        CHECK(elog::visibleToGame(eng, elog::Level::Warning),
              "a WARNING from an engine category still reaches the game console");
        CHECK(elog::visibleToGame(eng, elog::Level::Error),
              "and so does an ERROR — a failure is never filed as an internal");

        // An unregistered/unknown category must also not swallow failures.
        CHECK(elog::visibleToGame(nullptr, elog::Level::Error),
              "even an unresolved category cannot hide an error");
        CHECK(!elog::visibleToGame(nullptr, elog::Level::Info),
              "while unresolved info stays out of the game console");

        // The tags the engine ships as game-facing must actually be marked —
        // "Script" is the one every kit and Lua script logs under, so if it is
        // wrong, a game developer cannot see their own script errors at info
        // level at all.
        elog::markGameFacingDefaults();
        CHECK(elog::audienceOf(*elog::category("Script")) == elog::Audience::Game,
              "'Script' — every kit and Lua log — is game-facing");
        CHECK(elog::audienceOf(*elog::category("Scene")) == elog::Audience::Game,
              "'Scene' is game-facing");
        CHECK(elog::audienceOf(*elog::category("Renderer")) == elog::Audience::Engine,
              "'Renderer' is NOT — it is machinery");
    }

    // ── The shipping posture silences ENGINE chatter, not the GAME's ─────────
    // A released build should not print engine internals into the player's log,
    // but a game using our logger as its logger must keep working. That
    // distinction is the payoff for Audience existing at all.
    {
        elog::Category* eng  = elog::category("TestShipEngine");
        elog::Category* game = elog::category("TestShipGame");
        elog::setAudience(*game, elog::Audience::Game);
        elog::quietForShipping();

        CHECK(!elog::enabled(eng, elog::Level::Info),
              "an ENGINE category stops recording info");
        CHECK(!elog::enabled(eng, elog::Level::Success),
              "...and success");
        CHECK(elog::enabled(eng, elog::Level::Warning) &&
              elog::enabled(eng, elog::Level::Error),
              "but keeps warnings and errors — a shipped log's whole purpose");
        CHECK(elog::enabled(game, elog::Level::Info),
              "a GAME category is untouched: silencing it would break anyone "
              "using the engine's logger as their game's");

        // Order-independent: a category created AFTER the call gets the posture
        // too, so a host may call this before or after boot logging starts.
        elog::Category* late = elog::category("TestShipLate");
        CHECK(!elog::enabled(late, elog::Level::Info) &&
              elog::enabled(late, elog::Level::Error),
              "a category created afterwards inherits the shipping default");

        elog::watchAll();
        CHECK(elog::enabled(eng, elog::Level::Info),
              "and watchAll() puts the engine categories back for a dev host");
        // watchAll must clear the sticky DEFAULT as well, or a subsystem that
        // registers after it is still silent while the UI says otherwise. This
        // assertion exists because the first version did not, and the omission
        // surfaced as a failure in the unrelated overflow case below.
        CHECK(elog::enabled(elog::category("TestShipAfterWatchAll"),
                            elog::Level::Info),
              "including for a category created after watchAll()");
    }

    // NOTE: this block must come BEFORE the overflow case. The overflow case
    // exhausts the 64-slot registry on purpose, after which `category("...")`
    // returns the shared overflow bucket for every new name — so both categories
    // below aliased the SAME object, marking one of them game-facing marked both,
    // and three assertions failed for a reason that had nothing to do with
    // gating. A test's position in the file is part of its setup.
    // ── Demand gating ────────────────────────────────────────────────────────
    // Engine detail is recorded only while something is watching, because the
    // Internal Console is closed for essentially the whole life of a session and
    // the 117 Info call sites were being formatted for nobody. This runs LAST:
    // it deliberately changes global masks, and putting it earlier would leak a
    // quiet posture into every case after it — which is exactly how the
    // watchAll/defaultMask bug got caught, from the other direction.
    {
        std::printf("\n── demand gating ──\n");
        elog::watchAll();
        elog::Category* eng  = elog::category("GateEngine");
        elog::Category* game = elog::category("GateGame");
        elog::setAudience(*game, elog::Audience::Game);

        CHECK(elog::verboseActive(),
              "before arming, everything records — an unarmed gate must be a "
              "no-op, so a headless tool or a test never loses lines silently");

        // Targeting set BEFORE the panel closes has to survive it, or Solo-ing a
        // subsystem and glancing away would throw the Solo out.
        elog::setLevel(*eng, elog::Level::Success, false);

        elog::armDemandGating();
        CHECK(!elog::verboseActive() && elog::watchers() == 0,
              "arming with nobody watching puts engine detail to sleep");
        CHECK(!elog::enabled(eng, elog::Level::Info),
              "an ENGINE category stops recording Info");
        CHECK(elog::enabled(eng, elog::Level::Warning) &&
              elog::enabled(eng, elog::Level::Error),
              "but NEVER stops recording warnings and errors — a gate that can "
              "hide a failure is a bug generator");
        CHECK(elog::enabled(game, elog::Level::Info),
              "a GAME category is untouched: it feeds the other console, and "
              "silencing a game's own scripts because an engine panel is shut "
              "would be indefensible");

        // A category created while asleep must be born asleep, or every
        // subsystem that registers after the panel closes leaks detail forever.
        elog::Category* late = elog::category("GateLate");
        CHECK(!elog::enabled(late, elog::Level::Info),
              "a category registered WHILE asleep is born asleep");

        elog::acquireWatch();
        CHECK(elog::verboseActive() && elog::enabled(eng, elog::Level::Info),
              "opening the panel wakes engine detail");
        CHECK(!elog::enabled(eng, elog::Level::Success),
              "and restores the per-category targeting that was set before, "
              "rather than resetting to 'watch everything'");

        // Refcount, not a bool: two observers, and the first one leaving must
        // not turn the lights off on the second.
        elog::acquireWatch();
        elog::releaseWatch();
        CHECK(elog::verboseActive(),
              "two watchers, one leaves — still recording (refcount, not a bool)");
        elog::releaseWatch();
        CHECK(!elog::verboseActive() && !elog::enabled(eng, elog::Level::Info),
              "the last one out turns it off");

        // An unbalanced release must clamp at zero rather than wrap negative and
        // make the next acquire fail to wake anything.
        elog::releaseWatch();
        CHECK(elog::watchers() == 0, "an unmatched release clamps at 0, no wrap");
        elog::acquireWatch();
        CHECK(elog::verboseActive() && elog::enabled(eng, elog::Level::Info) &&
              !elog::enabled(eng, elog::Level::Success),
              "so the next acquire still wakes it WITH the targeting intact — "
              "checking the masks and not just the flag, because the flag was "
              "right while the saved masks had already been clobbered");
        elog::releaseWatch();

        // The pin: the escape hatch for needing the lines from BEFORE you knew
        // there was a problem, which is the one thing gating genuinely costs.
        elog::pinVerbose(true);
        CHECK(elog::verboseActive() && elog::enabled(eng, elog::Level::Info),
              "pinning records with the panel closed");
        elog::pinVerbose(false);
        CHECK(!elog::verboseActive(), "unpinning goes back to sleep");

        // Leave the process as we found it, so this case cannot poison a future
        // one the way the sticky defaultMask did.
        elog::pinVerbose(false);
        elog::watchAll();
        CHECK(elog::enabled(eng, elog::Level::Info) &&
              elog::enabled(late, elog::Level::Info),
              "watchAll overrides the gate for existing AND late categories");
    }

    // ── Category OVERFLOW is visible, not a silent merge ────────────────────
    // Past the usable capacity this used to return &cats[0], so the 64th tag
    // inherited slot 0's mask, bumped slot 0's counter and displayed under slot
    // 0's NAME — a kit's lines appearing to come from the renderer, untargetable
    // and unsilenceable. That defeats the one feature this file exists for, and
    // nothing reported it. Kept last in this file because it exhausts the
    // registry for the rest of the process.
    {
        elog::Category* first = &elog::categoryAt(0);
        const char* firstName = first->name.load();
        const uint64_t firstLines = first->written.load();

        char names[80][16];
        elog::Category* cats[80];
        for (int i = 0; i < 80; ++i) {
            std::snprintf(names[i], sizeof(names[i]), "Fill%02d", i);
            cats[i] = elog::category(names[i]);
        }

        CHECK(elog::overflowHits() > 0,
              "overflow is COUNTED (%llu), not silent",
              (unsigned long long)elog::overflowHits());

        // The decisive property: an overflowed tag must not impersonate a real
        // subsystem. It lands in a bucket that says what it is.
        elog::Category* over = cats[79];
        const char* overName = over->name.load();
        CHECK(over != first,
              "an overflowed category is NOT slot 0 — it cannot impersonate the "
              "first subsystem that ever registered ('%s')",
              firstName ? firstName : "?");
        CHECK(overName && std::strstr(overName, "exhausted") != nullptr,
              "it lands in a bucket that names itself ('%s')",
              overName ? overName : "<null>");

        // And slot 0 is untouched: same name, and its line count did not absorb
        // anybody else's writes.
        CHECK(first->name.load() == firstName, "slot 0 kept its own name");
        elog::write(over, elog::Level::Info, "overflowed line");
        CHECK(first->written.load() == firstLines,
              "and slot 0's counter did not absorb the overflowed write "
              "(%llu, was %llu)", (unsigned long long)first->written.load(),
              (unsigned long long)firstLines);

        // Everything up to the usable limit is still independently targetable —
        // that is what the reserved bucket buys.
        elog::Category* mid = cats[10];
        elog::setLevel(*mid, elog::Level::Info, false);
        CHECK(!elog::enabled(mid, elog::Level::Info) &&
              elog::enabled(cats[11], elog::Level::Info),
              "a category below the limit is silenced ALONE");
        elog::setLevel(*mid, elog::Level::Info, true);

        // Every registered name must be non-null: a reader observing the count
        // must never see a reserved-but-unnamed slot as a live category. (The
        // race this guards is a real one, and only a TSan run can prove the
        // ordering — see the header. This at least pins the invariant.)
        bool allNamed = true;
        const int n = elog::categoryCount();
        for (int i = 0; i < n; ++i)
            if (!elog::categoryAt(i).name.load()) allNamed = false;
        CHECK(allNamed, "every slot the count exposes has a published name");
    }

    if (g_failures) {
        std::printf("\nlogger_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("\nlogger_test: PASS\n");
    return 0;
}
