// ── camera_look_test — aiming a camera without touching simulation state ────
//
// Stage 4 of the command architecture: the presentation split.
//
// A first-person controller latches the mouse in `onFrame` — it must, or look
// lags the frame rate and the game feels broken — and then wrote the result
// into `Transform.rotation`. `Transform` is a hashed SimState component, so
// that is a RENDER-RATE WRITE TO SIMULATION STATE: BUG-0053's defect class,
// surviving in the tree only because no determinism-gate tier drives input and
// nothing measures it.
//
// What this file pins, in order of how much depends on it:
//
//   1. Writing CameraLook does NOT change the world hash, and writing the same
//      aim into Transform.rotation DOES. That is the entire point, and it is
//      the assertion a future edit is most likely to break by classifying the
//      component as SimState "for completeness".
//   2. The renderer actually composes it — a component nothing reads would
//      satisfy (1) by doing nothing at all, which is the BUG-0049 shape.
//   3. The composed basis MATCHES the quaternion path it replaces, so this is
//      a migration and not a different camera.
#include <cmath>
#include <cstdio>
#include <cstring>

#include <flecs.h>
#include <bx/math.h>

#include "components/camera.h"
#include "components/camera_look.h"
#include "components/meta_registry.h"
#include "components/name.h"
#include "core/transform.h"
#include "core/transform_utils.h"
#include "runtime/camera_util.h"
#include "runtime/sim_classification.h"
#include "runtime/sim_hash.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

// The quaternion the kit built from yaw/pitch, reproduced here so section 3 can
// compare the new path against the one it replaces rather than against a
// hand-computed expectation.
static bx::Quaternion basisQuat(const bx::Vec3& fwd, const bx::Vec3& rgt,
                                const bx::Vec3& up) {
    const float basis[16] = {
        rgt.x,  rgt.y,  rgt.z,  0.0f,
        up.x,   up.y,   up.z,   0.0f,
       -fwd.x, -fwd.y, -fwd.z,  0.0f,
        0.0f,   0.0f,   0.0f,   1.0f };
    return quatFromMatrix(basis);
}

static bool nearv(const bx::Vec3& a, const bx::Vec3& b, float eps = 1e-4f) {
    return std::fabs(a.x-b.x) < eps && std::fabs(a.y-b.y) < eps
        && std::fabs(a.z-b.z) < eps;
}

// The forward vector a view matrix looks along, read back OUT of the matrix
// rather than trusted from the input — the whole question here is whether the
// renderer used the aim, so taking it from the aim would prove nothing.
//
// NOT negated. bx::mtxLookAt builds a WORLD->VIEW matrix, the inverse of the
// camera's world pose, so its THIRD COLUMN is already the world-space forward.
// The first version of this helper negated it, the way one would read a camera
// pose, and got exactly -forward for every case — which section 3 could not
// see, because both of its sides went through the same wrong helper. A helper
// used on both sides of a comparison can be wrong invisibly.
static bx::Vec3 forwardOf(const float view[16]) {
    return bx::normalize(bx::Vec3{ view[2], view[6], view[10] });
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("camera_look_test — aiming without writing simulation state\n");

    const CameraLook aim{ 0.9f, -0.35f };

    // ── 1. THE PROPERTY: a render-rate aim does not reach the world hash ────
    {
        std::printf("\n-- 1. the aim is not simulation state --\n");
        flecs::world w;
        MetaRegistry::registerAll(w);
        simhash::registerClassification(w);

        Transform t{}; t.rotation = {0,0,0,1}; t.scale = {1,1,1};
        flecs::entity cam = w.entity()
            .set<Transform>(t).set<Name>({"cam"})
            .set<Camera>({}).set<CameraLook>({});

        // Every instantiated component must be classified, or the gate is
        // blind to it. A new component that nobody classifies is the failure
        // this audit exists for, so it is checked before anything else.
        std::vector<std::string> unclassified;
        CHECK(simhash::auditCoverage(w, unclassified),
              "CameraLook is classified — an unclassified component is "
              "INVISIBLE to hashWorld, which is worse than being hashed");

        const uint64_t before = simhash::hashWorld(w);
        for (int frame = 0; frame < 12; ++frame)
            cam.set<CameraLook>({ aim.yaw + (float)frame * 0.01f, aim.pitch });
        CHECK(simhash::hashWorld(w) == before,
              "twelve render-rate aim changes leave the world hash UNCHANGED "
              "— the same content at 1 and 2 frames per tick now simulates "
              "identically no matter how the player moved the mouse");

        // The control. Writing the SAME aim the old way does change it, which
        // is what makes the assertion above mean something rather than being
        // satisfied by a world nothing touched.
        bx::Vec3 f = bx::InitZero, r = bx::InitZero, u = bx::InitZero;
        cameraLookBasis(aim, f, r, u);
        Transform rotated = t;
        rotated.rotation = basisQuat(f, r, u);
        cam.set<Transform>(rotated);
        CHECK(simhash::hashWorld(w) != before,
              "...while writing that same aim into Transform.rotation — what "
              "the controller used to do every frame — DOES change it");
    }

    // ── 2. The renderer composes it ────────────────────────────────────────
    // A component nothing reads would pass section 1 by doing nothing.
    {
        std::printf("\n-- 2. the renderer reads it --\n");
        flecs::world w;
        Transform t{}; t.position = {5.0f, 2.0f, -3.0f};
        t.rotation = {0,0,0,1}; t.scale = {1,1,1};
        flecs::entity cam = w.entity().set<Transform>(t).set<Camera>({});

        PrimaryCameraFinder finder;
        float view[16], proj[16], clear[4];
        CHECK(finder.find(w, view, proj, 1.6f, clear, false), "the camera is found");
        const bx::Vec3 identityFwd = forwardOf(view);
        CHECK(nearv(identityFwd, {0.0f, 0.0f, -1.0f}),
              "with no CameraLook it looks along the Transform's -Z (%.3f, "
              "%.3f, %.3f)", (double)identityFwd.x, (double)identityFwd.y,
              (double)identityFwd.z);

        cam.set<CameraLook>(aim);
        CHECK(finder.find(w, view, proj, 1.6f, clear, false), "still found");
        const bx::Vec3 aimed = forwardOf(view);
        bx::Vec3 f = bx::InitZero, r = bx::InitZero, u = bx::InitZero;
        cameraLookBasis(aim, f, r, u);
        CHECK(nearv(aimed, f),
              "with one, the view looks along the COMPOSED basis (%.3f, %.3f, "
              "%.3f vs %.3f, %.3f, %.3f)",
              (double)aimed.x, (double)aimed.y, (double)aimed.z,
              (double)f.x, (double)f.y, (double)f.z);
    }

    // ── 3. It is the same camera, not a different one ──────────────────────
    // A migration has to land where the thing it replaces landed, or every
    // scene authored against the old path is silently re-aimed.
    {
        std::printf("\n-- 3. same aim as the path it replaces --\n");
        bx::Vec3 f = bx::InitZero, r = bx::InitZero, u = bx::InitZero;
        cameraLookBasis(aim, f, r, u);

        flecs::world w;
        Transform t{}; t.scale = {1,1,1};
        t.rotation = basisQuat(f, r, u);      // the OLD path: aim in Transform
        w.entity().set<Transform>(t).set<Camera>({});

        PrimaryCameraFinder finder;
        float view[16], proj[16], clear[4];
        finder.find(w, view, proj, 1.6f, clear, false);
        const bx::Vec3 oldWay = forwardOf(view);

        flecs::world w2;
        Transform t2{}; t2.rotation = {0,0,0,1}; t2.scale = {1,1,1};
        w2.entity().set<Transform>(t2).set<Camera>({}).set<CameraLook>(aim);
        PrimaryCameraFinder f2;
        float view2[16];
        f2.find(w2, view2, proj, 1.6f, clear, false);
        const bx::Vec3 newWay = forwardOf(view2);

        CHECK(nearv(oldWay, newWay, 1e-3f),
              "both paths aim the same way (%.4f,%.4f,%.4f vs %.4f,%.4f,%.4f)",
              (double)oldWay.x, (double)oldWay.y, (double)oldWay.z,
              (double)newWay.x, (double)newWay.y, (double)newWay.z);
    }

    // ── 4. Position still comes from the hierarchy ─────────────────────────
    // CameraLook takes over ORIENTATION only. The eye-height migration the
    // presentation split calls for — camera as a CHILD of the character with a
    // constant local offset, replacing a non-idempotent `position.y +=` on a
    // physics-owned field — depends on a parented camera still being carried by
    // its parent while aiming independently of it.
    {
        std::printf("\n-- 4. a parented camera --\n");
        flecs::world w;
        Transform pt{}; pt.position = {10.0f, 0.0f, 0.0f};
        pt.rotation = {0,0,0,1}; pt.scale = {1,1,1};
        flecs::entity body = w.entity().set<Transform>(pt);

        Transform ct{}; ct.position = {0.0f, 1.7f, 0.0f};   // the eye offset
        ct.rotation = {0,0,0,1}; ct.scale = {1,1,1};
        flecs::entity cam = w.entity().set<Transform>(ct)
                                     .set<Camera>({}).set<CameraLook>(aim);
        cam.child_of(body);

        PrimaryCameraFinder finder;
        float view[16], proj[16], clear[4];
        finder.find(w, view, proj, 1.6f, clear, false);

        // The eye is at parent + local offset, and the offset is CONSTANT —
        // which is the point. The old code added kEyeHeight to a field physics
        // rewrote every step, so it was only ever correct because something
        // else happened to reset it first.
        bx::Vec3 f = bx::InitZero, r = bx::InitZero, u = bx::InitZero;
        cameraLookBasis(aim, f, r, u);
        CHECK(nearv(forwardOf(view), f),
              "a parented camera still aims by its own CameraLook");

        const Transform& still = cam.get<Transform>();
        CHECK(still.position.y == 1.7f && still.position.x == 0.0f,
              "and its local offset is untouched by rendering (%.2f) — a "
              "constant, not a value re-added every frame",
              (double)still.position.y);
    }

    if (g_failures) {
        std::printf("\ncamera_look_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\ncamera_look_test: ALL PASS\n");
    return 0;
}
