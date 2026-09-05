// ── seam_cost_bench — what a swappable renderer's boundary actually costs ────
//
// This is the harness behind docs/rhi/swappability.md §2, whose numbers decide
// whether "swappable renderer" is compatible with "no performance loss". That
// document's own §8 says no abstraction ships without being measured — and it
// cited NEON, the residency sweep and the thread-QoS test as precedent, all
// three of which have committed harnesses anyone can re-run. This one did not,
// until review pointed out that the strongest document in docs/rhi/ rested on a
// measurement that existed only as prose.
//
// WHAT IT MEASURES: dispatch cost at PER-DRAW granularity, which is the
// granularity a renderer ABI must not use. Five variants of the same trivial
// body, so the difference between them is the boundary and nothing else:
//
//   direct            inlinable — the no-abstraction baseline
//   virtual mono      one implementation, as IRenderPipeline is today
//   virtual poly      two implementations, so the vtable is not predictable
//   fn ptr, same bin  a C-style table the optimiser can still see through
//   fn ptr, cross-so  a real ABI: no inlining, no devirtualisation
//
// WHAT IT DOES NOT MEASURE, stated because the number is easy to over-read:
// marshalling, lost inlining of REAL work, and the semantic floor of an
// abstraction (docs/rhi/swappability.md §3) — which is where bgfx actually
// costs us, and which no microbenchmark can see. The body here is four
// instructions. Treat the result as a FLOOR on the boundary cost, not an
// estimate of it.
//
// NOT A GATE. It prints numbers and always exits 0. Dispatch cost is a property
// of the machine and the compiler, not of this repo, so a threshold here would
// fail on somebody's laptop for no defect. It is registered in the `perf` lane
// so it is run deliberately.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>   // atoi
#include <dlfcn.h>
#include <random>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace {

uint64_t g_acc = 0;   // global so nothing can be proven dead

inline void directDraw(uint64_t* a, const uint32_t* d, uint32_t i) {
    *a += (uint64_t)d[i] * 3u + 1u;
}

struct IDraw {
    virtual void draw(uint64_t*, const uint32_t*, uint32_t) = 0;
    virtual ~IDraw() = default;
};
struct ImplA final : IDraw {
    void draw(uint64_t* a, const uint32_t* d, uint32_t i) override {
        *a += (uint64_t)d[i] * 3u + 1u;
    }
};
struct ImplB final : IDraw {
    void draw(uint64_t* a, const uint32_t* d, uint32_t i) override {
        *a += (uint64_t)d[i] * 3u + 2u;
    }
};

using DrawFn = void (*)(uint64_t*, const uint32_t*, uint32_t);
void tableDraw(uint64_t* a, const uint32_t* d, uint32_t i) {
    *a += (uint64_t)d[i] * 3u + 1u;
}

double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // 100 000 draws/frame is the figure docs/architecture/provider-abi.md §1
    // costs a per-draw renderer ABI at. This engine submits 299 for 50 000 real
    // props, so it is a deliberate over-estimate — the point is that even there
    // the number is small.
    constexpr uint32_t kDraws  = 100000;
    const int          kFrames = (argc > 1) ? std::atoi(argv[1]) : 500;

    std::vector<uint32_t> data(kDraws);
    std::mt19937 rng(1234);            // fixed seed: the data must not vary run to run
    for (auto& v : data) v = rng();
    const uint32_t* d = data.data();

    ImplA a; ImplB b;
    IDraw* mono = &a;
    std::vector<IDraw*> poly(kDraws);
    for (uint32_t i = 0; i < kDraws; ++i) poly[i] = (i & 1) ? (IDraw*)&a : (IDraw*)&b;
    DrawFn fp = &tableDraw;

    // The .dylib sits beside this executable; both land in the build root.
    void* h = dlopen(ENGINE_SEAM_PLUGIN_PATH, RTLD_NOW);
    if (!h) { std::printf("seam_cost_bench: dlopen failed: %s\n", dlerror()); return 0; }
    auto sofn = (DrawFn)dlsym(h, "seamDraw");
    if (!sofn) { std::printf("seam_cost_bench: dlsym failed\n"); return 0; }

    double tD = 0, tM = 0, tP = 0, tF = 0, tS = 0;
    // Three passes; keep the last. The first warms caches and the branch
    // predictor, which is what a steady-state frame loop sees.
    for (int rep = 0; rep < 3; ++rep) {
        auto t0 = Clock::now();
        for (int f = 0; f < kFrames; ++f) for (uint32_t i = 0; i < kDraws; ++i) directDraw(&g_acc, d, i);
        auto t1 = Clock::now();
        for (int f = 0; f < kFrames; ++f) for (uint32_t i = 0; i < kDraws; ++i) mono->draw(&g_acc, d, i);
        auto t2 = Clock::now();
        for (int f = 0; f < kFrames; ++f) for (uint32_t i = 0; i < kDraws; ++i) poly[i]->draw(&g_acc, d, i);
        auto t3 = Clock::now();
        for (int f = 0; f < kFrames; ++f) for (uint32_t i = 0; i < kDraws; ++i) fp(&g_acc, d, i);
        auto t4 = Clock::now();
        for (int f = 0; f < kFrames; ++f) for (uint32_t i = 0; i < kDraws; ++i) sofn(&g_acc, d, i);
        auto t5 = Clock::now();
        tD = ms(t0,t1); tM = ms(t1,t2); tP = ms(t2,t3); tF = ms(t3,t4); tS = ms(t4,t5);
    }

    std::printf("seam_cost_bench: %u draws/frame x %d frames (acc=%llu)\n\n",
                kDraws, kFrames, (unsigned long long)g_acc);
    auto row = [&](const char* n, double t) {
        std::printf("  %-34s %8.2f ms  %8.4f ms/frame  %6.2f ns/call\n",
                    n, t, t / kFrames, t * 1e6 / (double)kFrames / kDraws);
    };
    row("direct (inlinable)",              tD);
    row("virtual, monomorphic",            tM);
    row("virtual, polymorphic (2 impls)",  tP);
    row("function pointer, same binary",   tF);
    row("function pointer, across .dylib", tS);

    std::printf("\n  cross-.so penalty vs direct: %+.4f ms/frame at %u draws\n",
                (tS - tD) / kFrames, kDraws);
    std::printf("  virtual  penalty vs direct: %+.4f ms/frame\n", (tM - tD) / kFrames);
    std::printf("\n  Interpretation is in docs/rhi/swappability.md §2-§3.\n"
                "  Short version: this is the cost you AVOID by putting the seam at\n"
                "  frame granularity (bgfx crosses its backend boundary once per\n"
                "  frame, bgfx.cpp:2704). It is not the cost of an abstraction —\n"
                "  that is the semantic floor, and no benchmark here can see it.\n");
    return 0;   // never a gate; see the header comment
}
