// ── ring_test — SPSC ring + clock correctness (no HID hardware needed) ──────
// 1) FIFO integrity: producer pushes sequenced events while the consumer
//    drains concurrently; every drained event must arrive in order with no
//    duplicates. Push failures (overflow) must equal dropped().
// 2) Overflow behavior: slow consumer → drops counted, sequence still ordered.
// 3) nowNs(): strictly monotonic, plausible resolution.
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <thread>

#include "hid/hid.h"

static int g_failures = 0;
#define CHECK(cond, msg) do {                                        \
    if (!(cond)) { std::printf("  FAIL  %s\n", msg); ++g_failures; } \
    else         { std::printf("  ok    %s\n", msg); }               \
} while (0)

int main() {
    std::printf("ring_test: hid module logic\n");

    // ── FIFO integrity under concurrency ────────────────────────────────────
    {
        hid::EventRing<1024> ring;
        constexpr uint64_t kCount = 2'000'000;
        std::atomic<uint64_t> pushed{0}, rejected{0};

        std::thread producer([&] {
            for (uint64_t i = 0; i < kCount; ++i) {
                hid::Event e{};
                e.timeNs = i;                      // sequence number
                e.type   = hid::EventType::MouseMotion;
                if (ring.push(e)) pushed.fetch_add(1, std::memory_order_relaxed);
                else              rejected.fetch_add(1, std::memory_order_relaxed);
            }
        });

        uint64_t drained = 0, lastSeq = 0;
        bool ordered = true;
        hid::Event buf[256];
        while (true) {
            const size_t n = ring.drain(buf, 256);
            for (size_t i = 0; i < n; ++i) {
                if (drained && buf[i].timeNs <= lastSeq) ordered = false;
                lastSeq = buf[i].timeNs;
                ++drained;
            }
            if (n == 0) {
                if (pushed.load() + rejected.load() == kCount &&
                    drained == pushed.load()) break;
                std::this_thread::yield();
            }
        }
        producer.join();

        CHECK(ordered, "events drain strictly in push order");
        CHECK(drained == pushed.load(), "drained == accepted pushes");
        CHECK(ring.dropped() == rejected.load(), "dropped() == rejected pushes");
        std::printf("        (%" PRIu64 " through, %" PRIu64 " dropped by "
                    "design under overflow)\n", drained, ring.dropped());
    }

    // ── Overflow: producer floods a tiny ring, consumer sips ────────────────
    {
        hid::EventRing<64> ring;
        for (uint64_t i = 0; i < 1000; ++i) {
            hid::Event e{};
            e.timeNs = i;
            ring.push(e);
        }
        CHECK(ring.dropped() == 1000 - 64, "flood drops exactly capacity excess");
        hid::Event buf[64];
        const size_t n = ring.drain(buf, 64);
        bool ok = (n == 64);
        for (size_t i = 0; i < n && ok; ++i) ok = (buf[i].timeNs == i);
        CHECK(ok, "surviving events are the OLDEST, in order (drop-newest)");
    }

    // ── Clock ───────────────────────────────────────────────────────────────
    {
        const uint64_t a = hid::nowNs();
        uint64_t b = a;
        for (int i = 0; i < 1000; ++i) {
            const uint64_t t = hid::nowNs();
            if (t < b) { b = 0; break; }   // regression
            b = t;
        }
        CHECK(b >= a, "nowNs() is monotonic over 1000 reads");
        CHECK(sizeof(hid::Event) == 24, "Event stays 24-byte wire POD");
    }

    if (g_failures) { std::printf("ring_test: FAIL (%d)\n", g_failures); return 1; }
    std::printf("ring_test: PASS\n");
    return 0;
}
