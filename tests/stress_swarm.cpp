// ── stress_swarm — many-entity ECS + allocator load ─────────────────────────
// Cranks entity count and concurrent allocation to surface scaling cliffs:
//   1. spawn 50k entities — must not runaway-map memory or crash
//   2. run a move system over all of them — reports per-tick cost (frame budget)
//   3. hammer mem:: from every worker at once — reports whether the per-tag
//      heap lock serializes parallel allocation (backlog: sharded arenas)
// Reports numbers (perf cliffs are for humans to read); hard-asserts only on
// crashes / unbounded heap growth (the syscall-minimization invariant at scale).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include <flecs.h>

#include "core/transform.h"
#include "core/memory/mem.h"
#include "runtime/jobs/jobs.h"

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

struct Vel { float x, y, z; };

int main() {
    // Unbuffered: ctest redirects stdout, which makes it block-buffered,
    // and a test killed on timeout loses everything still in the buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("stress_swarm: many-entity ECS + allocator load\n");
    jobs::init();

    // ── 1. Spawn a swarm ────────────────────────────────────────────────────
    constexpr int N = 50000;
    const uint64_t maps0 = mem::mapEventCount();
    flecs::world w;
    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) {
        const float a = i * 0.01f;
        w.entity()
         .set<Transform>({{std::cos(a)*20.f, 0.f, std::sin(a)*20.f}, {0,0,0,1}, {1,1,1}})
         .set<Vel>({std::sin(a), 0.f, std::cos(a)});
    }
    auto t1 = Clock::now();
    std::printf("        spawned %d entities in %.1f ms (%.2f us/ent)\n",
                N, ms(t0,t1), ms(t0,t1)*1000.0/N);
    CHECK(w.count<Transform>() == N, "all %d entities live", N);

    // ── 2. Move system over the whole swarm ─────────────────────────────────
    auto moveQ = w.query_builder<Transform, const Vel>().build();
    constexpr int TICKS = 120;
    const float dt = 1.f/60.f;
    auto t2 = Clock::now();
    for (int k = 0; k < TICKS; ++k)
        moveQ.each([dt](Transform& t, const Vel& v) {
            t.position.x += v.x*dt; t.position.y += v.y*dt; t.position.z += v.z*dt;
        });
    auto t3 = Clock::now();
    std::printf("        move %d ents x %d ticks: %.3f ms/tick (%.1f fps-equiv)\n",
                N, TICKS, ms(t2,t3)/TICKS, 1000.0/(ms(t2,t3)/TICKS));

    // ── 3. Concurrent allocator contention ──────────────────────────────────
    // Every worker allocs+frees a burst on the same tag; if the TagHeap lock
    // serializes, wall time ~ single-thread time x work, not / cores.
    auto allocBurst = [](uint32_t begin, uint32_t end) {
        void* p[64];
        for (uint32_t i = begin; i < end; ++i) {
            for (int j = 0; j < 64; ++j) p[j] = mem::alloc(64 + (j*37)%512, 16, mem::Tag::ECS);
            for (int j = 0; j < 64; ++j) mem::free(p[j]);
        }
    };
    constexpr uint32_t ROUNDS = 40000;
    auto s0 = Clock::now(); allocBurst(0, ROUNDS); auto s1 = Clock::now();       // 1 thread
    jobs::parallelFor("alloc", ROUNDS, ROUNDS/(jobs::workerCount()*4+1), allocBurst);
    auto s2 = Clock::now();                                                       // all threads
    const double serial = ms(s0,s1), par = ms(s1,s2);
    std::printf("        alloc burst: 1-thread %.1f ms | %u-thread %.1f ms | speedup %.2fx\n",
                serial, jobs::workerCount(), par, serial/par);

    // ── Invariant: the swarm must not have leaked mappings to the OS ─────────
    // (steady state churn returns memory; a small residual for the live world
    // is fine, but it must not scale with the work we did.)
    CHECK(mem::mapEventCount() < maps0 + 4096,
          "no runaway OS mapping under 50k ents + 2.6M allocs (%llu new maps)",
          (unsigned long long)(mem::mapEventCount() - maps0));

    jobs::shutdown();
    if (g_failures) { std::printf("stress_swarm: FAIL — %d\n", g_failures); return 1; }
    std::printf("stress_swarm: PASS — swarm survived; see numbers above\n");
    return 0;
}
