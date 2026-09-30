// ── cull_mode_test — which faces the pipeline removes, derived, not assumed ────
//
// WO-032. The opaque pass set BOTH cull bits (BGFX_STATE_DEFAULT already carries
// CULL_CW, and CULL_CCW was OR'd on top): cull mode 3, which Metal treats as "no
// culling" and D3D11/Vulkan read past the end of a three-entry table with.
//
// Which bit is RIGHT is not a matter of taste: it follows from how the camera
// projects. So this test does not hard-code the answer. It runs the engine's
// real camera path (PrimaryCameraFinder), projects a triangle that faces the
// camera and one that faces away, and derives the bit that removes the one
// facing away. passstate::kCullBackFaces must equal it. When the projection
// changes (WO-033 makes it right-handed), this fails until the bit follows.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <flecs.h>
#include <bx/math.h>

#include "components/camera.h"
#include "core/transform.h"
#include "render/pipeline/pass_states.h"
#include "runtime/camera_util.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL  " __VA_ARGS__); std::printf("\n"); ++g_failures; } \
                           else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } } while (0)

// Signed area of a projected triangle in NDC (y up): > 0 is counter-clockwise,
// the space bgfx's BGFX_STATE_CULL_CW / _CCW are defined in.
static float ndcArea(const float vp[16], bx::Vec3 a, bx::Vec3 b, bx::Vec3 c) {
    auto project = [&](bx::Vec3 p, float out[2]) {
        const float in[4] = {p.x, p.y, p.z, 1.0f};
        float r[4];
        bx::vec4MulMtx(r, in, vp);
        out[0] = r[0] / r[3]; out[1] = r[1] / r[3];
    };
    float pa[2], pb[2], pc[2];
    project(a, pa); project(b, pb); project(c, pc);
    return (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pb[1] - pa[1]) * (pc[0] - pa[0]);
}

static std::string slurp(const char* rel) {
    std::ifstream f(std::string(ENGINE_SOURCE_DIR) + "/" + rel);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("cull_mode_test\n");
    using namespace passstate;

    // ── 1. Every pass state carries at most one cull bit ────────────────────
    std::printf("1. pass states\n");
    CHECK(!validCull(BGFX_STATE_DEFAULT | BGFX_STATE_CULL_CCW),
          "validCull rejects the WO-032 state (DEFAULT | CULL_CCW = both bits)");
    CHECK(validCull(opaque(false)) && (opaque(false) & BGFX_STATE_CULL_MASK) == kCullBackFaces,
          "opaque, single-sided: exactly the back-face bit");
    CHECK((opaque(true) & BGFX_STATE_CULL_MASK) == 0, "opaque, double-sided: no culling");
    CHECK(opaque(true) == (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                           BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA),
          "double-sided is unchanged from the state it replaced, flag for flag");
    CHECK(validCull(shadowCaster()) && (shadowCaster() & BGFX_STATE_CULL_MASK) == kCullBackFaces,
          "shadow casters: exactly the back-face bit");

    // ── 2. The camera path decides which bit that is ────────────────────────
    std::printf("2. the bit follows the real camera path\n");
    struct Pose { const char* what; bx::Vec3 pos; float yawDeg; };
    const Pose poses[] = {{"at the origin, looking down -Z", {0, 0, 0}, 0},
                          {"moved and turned 90 degrees",   {4, 1, -2}, 90},
                          {"turned 200 degrees",            {0, 3, 0}, 200}};
    for (const bool homogeneous : {false, true})
        for (const Pose& ps : poses) {
            flecs::world w;
            Transform t{};
            t.position = ps.pos;
            const float h = bx::toRad(ps.yawDeg) * 0.5f;
            t.rotation = {0, std::sin(h), 0, std::cos(h)};
            t.scale = {1, 1, 1};
            w.entity().set<Transform>(t).set<Camera>({});
            PrimaryCameraFinder finder;
            float view[16], proj[16], clear[4], vp[16];
            if (!finder.find(w, view, proj, 1.5f, clear, homogeneous)) { CHECK(false, "camera found"); continue; }
            bx::mtxMul(vp, view, proj);

            // A triangle 5 m in front of the camera, counter-clockwise as seen
            // FROM the camera (its front face, in the engine's CCW-front
            // convention), and the same triangle wound the other way (a back face).
            // The camera's basis, read from the view the finder built (bx::mtxLookAt
            // stores the look direction in column 2 and up in column 1), so the
            // triangle is placed by the ENGINE's rotation convention, not the
            // test's. "Right" is the right-handed world's: forward x up.
            const bx::Vec3 fwd{view[2], view[6], view[10]}, up{view[1], view[5], view[9]};
            const bx::Vec3 right = bx::cross(fwd, up);
            const bx::Vec3 o = bx::add(ps.pos, bx::mul(fwd, 5.0f));
            const bx::Vec3 a = o, b = bx::add(o, right), d = bx::add(o, up);
            {   // the triangle must be IN FRONT of the camera, or its winding means nothing
                const float in[4] = {o.x, o.y, o.z, 1.0f};
                float r[4]; bx::vec4MulMtx(r, in, vp);
                if (!(r[3] > 0.0f && std::fabs(r[0] / r[3]) < 1.0f)) {
                    CHECK(false, "%s: the test triangle is not in view (w=%g)", ps.what, r[3]);
                    continue;
                }
            }
            const float front = ndcArea(vp, a, b, d);
            const float back  = ndcArea(vp, a, d, b);
            // CULL_CW removes triangles clockwise on screen; CULL_CCW, counter-clockwise ones.
            const uint64_t removesBack = back < 0 ? BGFX_STATE_CULL_CW : BGFX_STATE_CULL_CCW;
            CHECK(front * back < 0 && kCullBackFaces == removesBack,
                  "%s%s: the back face is %s on screen, so the back-face bit is CULL_%s",
                  ps.what, homogeneous ? " (homogeneous depth)" : "",
                  back < 0 ? "clockwise" : "counter-clockwise", back < 0 ? "CW" : "CCW");

            if (ps.yawDeg == 0 && !homogeneous) {
                const float p[4] = {1, 0, -5, 1};
                float r[4]; bx::vec4MulMtx(r, p, vp);
                std::printf("        note: world +X in front of this camera lands on the %s of the screen%s\n",
                            r[0] > 0 ? "RIGHT" : "LEFT", r[0] > 0 ? "" : " (the image is mirrored: WO-033)");
            }
        }

    // ── 3. Every camera builds its view with the SAME handedness ────────────
    // One back-face bit can only be right for every pass if every view has the
    // same handedness. The shadow pass and the editor's fly camera cannot run
    // here without a GPU or an editor, so their source is read instead: none of
    // the three may choose a handedness unless all three do.
    std::printf("3. one handedness for every camera\n");
    const char* cameras[] = {"src/runtime/camera_util.h", "src/editor/fly_camera.h",
                             "src/render/pipeline/shadow_pass.cpp"};
    int explicitRight = 0, readable = 0;
    for (const char* f : cameras) {
        const std::string src = slurp(f);
        if (src.find("mtxLookAt") == std::string::npos) continue;
        ++readable;
        if (src.find("Handedness::Right") != std::string::npos) ++explicitRight;
    }
    CHECK(readable == 3, "all three camera sources were read (%d)", readable);
    CHECK(explicitRight == 0 || explicitRight == 3,
          "every camera uses the same handedness (%d of 3 explicitly right-handed)", explicitRight);

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
