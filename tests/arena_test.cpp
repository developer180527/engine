// arena_test — headless correctness check for core/frame_arena.h (engine_core).
#include "core/frame_arena.h"
#include <cstdint>
#include <cstdio>

int main() {
    // Unbuffered: ctest redirects stdout, which makes it block-buffered,
    // and a test killed on timeout loses everything still in the buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    mem::FrameArena a;
    a.init(1024);   // tiny on purpose, to exercise overflow

    bool ok = true;
    auto check = [&](const char* what, bool cond) {
        std::printf("  %-32s %s\n", what, cond ? "PASS" : "FAIL");
        ok = ok && cond;
    };

    // Distinct, advancing pointers.
    auto* a0 = a.alloc<uint32_t>(4);   // 16 B
    auto* a1 = a.alloc<uint32_t>(4);   // 16 B
    check("distinct allocations", a0 != a1);
    check("pointer advanced",     (uint8_t*)a1 > (uint8_t*)a0);
    check("used tracks bytes",    a.used() >= 32);

    // Alignment honoured.
    auto* p = (uint8_t*)a.alloc(1, 1);   // misalign by 1
    auto* q = (uint8_t*)a.alloc(8, 64);  // request 64-byte alignment
    check("64-byte alignment",    ((uintptr_t)q % 64) == 0);
    (void)p;

    // reset() rewinds — next alloc reuses the front of the block.
    a.reset();
    check("reset rewinds used",   a.used() == 0);
    auto* r = a.alloc<uint32_t>(4);
    check("reuses block after reset", (void*)r == (void*)a0);

    // Overflow → graceful heap fallback (1KB arena, ask for 4KB).
    a.reset();
    void* big = a.alloc(4096, 16);
    check("overflow returns valid ptr", big != nullptr);
    check("overflow tracked",           a.overflowBytes() >= 4096);
    a.reset();
    check("reset frees overflow",       a.overflowBytes() == 0);

    // High-water persists across resets (peak usage for profiling).
    check("high-water recorded",  a.highWater() > 0);

    std::printf("arena_test: %s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
