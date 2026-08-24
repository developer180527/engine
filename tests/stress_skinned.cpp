// ── stress_skinned — the skinned path under horde load ──────────────────────
// The skinned data path had NO load coverage. stress_swarm, stress_churn and
// stress_physics all use static meshes, so the one component in the tree that
// is measured in KILOBYTES per entity was never exercised at scale.
//
// It matters because of what SkinnedMesh costs to ITERATE, not to compute. The
// renderer's extraction query takes SkinnedMesh as a term and reads five bytes
// of it per entity (`skeleton`, `hasSkinMatrices`) — but the stride between
// those reads is the whole component. Every byte the component carries is a
// byte of stride the renderer pays for and never looks at.
//
// So this measures the two access patterns the engine really has, at horde
// scale, against the real component:
//
//   READ  — what Renderer::extract does: touch the handle and the flag, skip
//           the palette entirely. Sensitive to component SIZE and nothing else.
//   WRITE — what AnimatorSystem does: fill the whole bone palette. Touches the
//           same bytes whatever the layout, so it isolates alignment.
//
// Hermetic like the rest of the lane: real flecs, real component, no assets,
// no ozz, no GPU. Numbers are for humans to read (a perf cliff is a judgement
// call); it hard-fails only on the invariant that the read pass must not cost
// more than the write pass, which would mean iteration alone outweighs doing
// the actual work.
#include <chrono>
#include <cstdio>
#include <vector>

#include <flecs.h>

#include "components/skinned_mesh.h"
#include "animation/skin_palette.h"
#include "components/animator.h"
#include "core/transform.h"
#include "components/mesh_renderer.h"
#include <cstdlib>

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int N = argc > 1 ? std::atoi(argv[1]) : 4000;
    constexpr int kPasses = 60;

    std::printf("stress_skinned: the skinned path at horde scale (N=%d)\n", N);
    std::printf("  sizeof(SkinnedMesh) = %zu B  -> %.1f KB per entity, "
                "%.1f MB for %d\n",
                sizeof(SkinnedMesh), sizeof(SkinnedMesh) / 1024.0,
                (double)sizeof(SkinnedMesh) * N / (1024.0 * 1024.0), N);
    std::printf("  (palettes live in anim::skinPalettes(), out of the component)\n");

    flecs::world w;
    for (int i = 0; i < N; ++i) {
        flecs::entity e = w.entity();
        e.set<Transform>({});
        e.set<MeshRenderer>({});
        e.set<SkinnedMesh>({});
        e.set<Animator>({});
    }

    // The renderer's shape: SkinnedMesh alongside the components extraction
    // already walks, so the table layout matches what the frame really sees.
    auto readQ  = w.query<const Transform, const MeshRenderer, const SkinnedMesh>();
    auto writeQ = w.query<SkinnedMesh>();

    // ── READ: the extraction pattern ────────────────────────────────────────
    volatile unsigned sink = 0;
    auto t0 = Clock::now();
    for (int p = 0; p < kPasses; ++p) {
        unsigned acc = 0;
        readQ.each([&](const Transform&, const MeshRenderer&, const SkinnedMesh& s) {
            // Exactly what extract.cpp consumes: the skeleton handle, the
            // populated flag, and the ADDRESS of the palette (never its
            // contents — the GPU upload reads it later).
            acc += s.skeleton.id + (unsigned)s.hasSkinMatrices + s.paletteSlot;
        });
        sink = acc;
    }
    const double readMs = ms(t0, Clock::now()) / kPasses;

    // ── WRITE: the animator pattern ─────────────────────────────────────────
    t0 = Clock::now();
    for (int p = 0; p < kPasses; ++p) {
        writeQ.each([&](SkinnedMesh& s) {
            if (s.paletteSlot == SkinnedMesh::kNoSlot)
                s.paletteSlot = anim::skinPalettes().acquire();
            float* m = anim::skinPalettes().at(s.paletteSlot);
            for (int i = 0; i < SkinnedMesh::kMatrixSize; ++i) m[i] = (float)i;
            s.hasSkinMatrices = true;
        });
    }
    const double writeMs = ms(t0, Clock::now()) / kPasses;

    std::printf("\n  extract read pass   %8.4f ms   (%6.1f ns/entity)\n",
                readMs, readMs * 1e6 / N);
    std::printf("  animator write pass %8.4f ms   (%6.1f ns/entity)\n",
                writeMs, writeMs * 1e6 / N);
    std::printf("  read/write ratio    %8.2f\n\n", readMs / (writeMs > 0 ? writeMs : 1e-9));

    // The invariant. Reading five bytes per entity must not approach the cost
    // of writing eight kilobytes per entity — if it does, the component's SIZE
    // is what the frame is paying for, not the animation.
    CHECK(readMs < writeMs,
          "reading the handle costs less than writing the palette "
          "(%.4f ms vs %.4f ms)", readMs, writeMs);

    // A frame budget is 16.6 ms at 60 Hz; extraction is one of many phases and
    // has no business spending a milli just walking components.
    CHECK(readMs < 1.0,
          "the extract read pass stays under 1 ms at N=%d (%.4f ms)", N, readMs);

    (void)sink;
    if (g_failures) {
        std::printf("\nstress_skinned: FAIL — %d\n", g_failures);
        return 1;
    }
    std::printf("\nstress_skinned: PASS\n");
    return 0;
}
