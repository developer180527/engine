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
// facing away. passstate::kCullBackFaces must equal it.
//
// WO-033 made every view right-handed (render/view_math.h). Before it, the
// image was mirrored (world +X on the LEFT of the screen) and the bit was
// CULL_CCW; §2 now also pins the un-mirrored screen, §3 the frustum built from
// the new matrices, and §4 that the editor camera moves the way it looks.
#include <cmath>
#include <cstdio>
#include <string>

#include <flecs.h>
#include <bx/math.h>

#include "components/camera.h"
#include "core/transform.h"
#include "render/pipeline/pass_states.h"
#include "runtime/camera_util.h"
#include "render/world/frustum.h"
#include "editor/fly_camera.h"

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
            // The camera's forward, found by UNPROJECTING the centre of the screen
            // at two depths: it assumes no handedness (reading a column of the
            // view matrix did, and broke the day the view changed hands). "Right"
            // is the right-handed WORLD's: forward x up. It is not read from the
            // screen, so "lands on the right" below is a real check, not a tautology.
            float inv[16];
            bx::mtxInverse(inv, vp);
            auto unproject = [&](float z) {
                const float in[4] = {0.0f, 0.0f, z, 1.0f};
                float r[4]; bx::vec4MulMtx(r, in, inv);
                return bx::Vec3{r[0] / r[3], r[1] / r[3], r[2] / r[3]};
            };
            const bx::Vec3 up{0, 1, 0};                                    // no pose has roll or pitch
            const bx::Vec3 fwd   = bx::normalize(bx::sub(unproject(0.9f), unproject(0.5f)));
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

            // Not mirrored: the camera's right (forward x up) lands on the RIGHT.
            {
                const bx::Vec3 q = bx::add(o, right);
                const float in[4] = {q.x, q.y, q.z, 1.0f}, oc[4] = {o.x, o.y, o.z, 1.0f};
                float r[4], ro[4]; bx::vec4MulMtx(r, in, vp); bx::vec4MulMtx(ro, oc, vp);
                CHECK(r[0] / r[3] > ro[0] / ro[3],
                      "%s%s: a point to the camera's right lands to the RIGHT on screen (not mirrored)",
                      ps.what, homogeneous ? " (homogeneous depth)" : "");
            }
        }

    // ── 3. The frustum built from the new matrices still contains what is in view
    // Culling reads planes extracted from view*proj. Handedness changes the
    // matrices; this shows the planes still keep what is ahead and drop what
    // is behind, to the side, and past the far plane.
    std::printf("3. frustum planes from the right-handed matrices\n");
    {
        flecs::world w;
        Transform t{}; t.rotation = {0, 0, 0, 1}; t.scale = {1, 1, 1};
        Camera cam{}; cam.farPlane = 100.0f;
        w.entity().set<Transform>(t).set<Camera>(cam);
        PrimaryCameraFinder finder;
        float view[16], proj[16], clear[4], vp[16], planes[6][4];
        finder.find(w, view, proj, 1.5f, clear, false);
        bx::mtxMul(vp, view, proj);
        rworld::extractFrustumPlanes(vp, planes);
        auto inside = [&](bx::Vec3 p) {
            for (auto& pl : planes) if (pl[0] * p.x + pl[1] * p.y + pl[2] * p.z + pl[3] < 0) return false;
            return true;
        };
        CHECK(inside({0, 0, -10}),   "a point 10 m ahead is inside");
        CHECK(inside({2, 1, -10}),   "a point ahead, up and to the right is inside");
        CHECK(!inside({0, 0, 10}),   "a point behind the camera is outside");
        CHECK(!inside({50, 0, -10}), "a point far to the side is outside");
        CHECK(!inside({0, 0, -150}), "a point past the far plane is outside");
    }

    // ── 4. The editor camera moves the way it looks ─────────────────────────
    // Its strafe keys and yaw sign used to be flipped to hide the mirror. Each
    // is checked on SCREEN: after the move, where did a point ahead go?
    std::printf("4. the editor fly camera\n");
    {
        auto screenX = [](const EditorCamera& c, bx::Vec3 p) {
            float view[16], proj[16], vp[16];
            c.getViewMatrix(view);
            viewmath::perspective(proj, 60.0f, 1.5f, 0.1f, 1000.0f, false);
            bx::mtxMul(vp, view, proj);
            const float in[4] = {p.x, p.y, p.z, 1.0f};
            float r[4]; bx::vec4MulMtx(r, in, vp);
            return r[0] / r[3];
        };
        EditorCamera cam; cam.position = {0, 0, 0};
        const bx::Vec3 ahead{0, 0, -10};

        EditorCamera moved = cam; FlyInput right; right.right = true;
        applyFly(moved, right, 0.1f);
        CHECK(screenX(moved, ahead) < screenX(cam, ahead) - 1e-4f,
              "the 'right' key moves the camera right: what was ahead slides LEFT on screen");
        EditorCamera movedL = cam; FlyInput left; left.left = true;
        applyFly(movedL, left, 0.1f);
        CHECK(screenX(movedL, ahead) > screenX(cam, ahead) + 1e-4f,
              "the 'left' key moves it left: what was ahead slides RIGHT");

        EditorCamera turned = cam; FlyInput drag; drag.lookDx = 40.0f;
        applyFly(turned, drag, 0.016f);
        CHECK(screenX(turned, ahead) < screenX(cam, ahead) - 1e-4f,
              "dragging the pointer right turns the view right: what was ahead slides LEFT");

        const bx::Vec3 r = cam.right();
        CHECK(screenX(cam, bx::add(ahead, r)) > screenX(cam, ahead),
              "EditorCamera::right() points to the right of the screen");
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
