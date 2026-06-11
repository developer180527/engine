#pragma once
#include <cstring>
#include <flecs.h>
#include <bx/math.h>
#include <bgfx/bgfx.h>

#include "components/camera.h"
#include "core/transform.h"
#include "core/transform_utils.h"
#include "runtime/world_query_cache.h"

// ── PrimaryCameraFinder ─────────────────────────────────────────────────────
// Finds the primary Camera entity in ANY world (game or editor) and produces
// its view/projection matrices + clear color. Runs every frame on the render
// path, so the underlying flecs query is cached per world (WorldQueryCache).
// Call reset() when a world you've queried gets destroyed (sim stop) — the
// runtime does this for its own instance; the editor does it in onStop().
class PrimaryCameraFinder {
public:
    bool find(flecs::world& world,
              float view[16], float proj[16],
              float aspect, float clearColor[4]) {
        bool found = false;
        m_query.get(world).each(
            [&](flecs::entity e, const Transform& t, const Camera& c) {
                if (!c.isPrimary || found) return;
                found = true;
                // Use world matrix so parented cameras inherit parent
                // transform. Row-major bgfx layout:
                //   row0=[Rx,Ry,Rz,0]  row1=[Ux,Uy,Uz,0]
                //   row2=[Bx,By,Bz,0]  row3=[Tx,Ty,Tz,1]
                // Camera looks along -Z locally, so world forward = -row2.
                float wm[16];
                getWorldMatrix(e, wm);
                bx::Vec3 pos = {wm[12], wm[13], wm[14]};
                bx::Vec3 fwd = bx::normalize({-wm[8], -wm[9], -wm[10]});
                bx::Vec3 up  = bx::normalize({ wm[4],  wm[5],  wm[6]});
                bx::mtxLookAt(view, pos, bx::add(pos, fwd), up);
                const bool rhNdc = bgfx::getCaps()->homogeneousDepth;
                if (c.projection == ProjectionType::Perspective)
                    bx::mtxProj(proj, c.fov, aspect, c.nearPlane, c.farPlane, rhNdc);
                else {
                    const float h = c.orthoSize, w = h * aspect;
                    bx::mtxOrtho(proj, -w, w, -h, h,
                                 c.nearPlane, c.farPlane, 0.0f, rhNdc);
                }
                std::memcpy(clearColor, c.clearColor, 16);
            });
        return found;
    }

    void reset() { m_query.reset(); }

private:
    WorldQueryCache<const Transform, const Camera> m_query;
};
