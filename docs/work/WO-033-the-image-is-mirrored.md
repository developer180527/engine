---
status: plan
id: WO-033
title: Every camera renders the world mirrored left-to-right
program: renderer
priority: P1
size: M
state: done
done: 2026-10-06
evidence: checked by eye by the owner 2026-10-06, editor and player screenshots of a text-textured single-sided quad (READ ME, LEFT, RIGHT +X): text reads, +X on the right, back face culled, gizmo and fly camera move the way they point
touches:
  - src/runtime/camera_util.h
  - src/editor/fly_camera.h
  - src/render/pipeline/shadow_pass.cpp
  - src/render/pipeline/pass_states.h
  - tests/cull_mode_test.cpp
source: found 2026-09-30 while fixing WO-032; measured with the engine's own camera path
---
## Why
The world is right-handed (glTF, the camera looks down local −Z, `ImportedScene`). Every view is built with `bx::mtxLookAt`'s default **left-handed** convention, and every projection with bx's left-handed default. The result is a mirror: **world +X lands on the left of the screen.** `cull_mode_test` prints it from `PrimaryCameraFinder` directly.

The engine has been working around it rather than fixing it:
- **The editor fly camera's "move right" moves along −right** (`fly_camera.h`: `bx::mul(rt, -step)`), because on screen −X *is* right. Its yaw sign (`yaw -= lookDx`) may be the same compensation.
- **Back faces reach the screen counter-clockwise**, so the pipeline culls CCW. That's why WO-032's fix keeps `CULL_CCW`, and why the old code wrote it at all.

What a mirror costs, even when everything "looks fine":
- text in a texture reads backwards
- a character's left hand is drawn on the right
- an asset modelled against a reference photo looks flipped
- any maths done in screen space (picking, gizmos, UI anchored to world points) has to carry the same hidden sign

## Done when
- [x] every camera builds a right-handed view and projection: `PrimaryCameraFinder`, the editor fly camera, the shadow pass's light
- [x] `passstate::kCullBackFaces` becomes `BGFX_STATE_CULL_CW`. `cull_mode_test` §2 fails until it does, because it derives the bit from the camera path, and §3 fails if only some cameras change.
- [x] a test pins "world +X in front of the camera lands on the RIGHT of the screen", replacing `cull_mode_test`'s note
- [x] every compensation is found and removed: the fly camera's `-step` on right, its yaw and pitch signs, `CameraLook`'s basis, the gizmo, picking/unprojection, and the shadow map's texel lookup (`sy` in `shadow_pass.cpp`), each checked, not assumed
- [x] an asymmetric asset (text on a texture) reads correctly in the editor and the player, checked by eye by the user before closing. **This one check also covers WO-032**: back faces are culled on macOS for the first time, so a single-sided open surface seen from behind now disappears (correct), closed meshes look the same, and the editor gizmo (ImGuizmo, whose own demo uses right-handed matrices) should now rotate and translate the way it points.
- [x] **kit-visible**: `Kits/` and `fps_shooter/` are untracked, and their look and strafe signs may carry the same compensation. `docs/guides/` gets a migration note listing what flips, since that code cannot be edited from here.

## Not in scope
Changing the world's handedness. The world is right-handed and stays so. Only the view is wrong.

## Log
- 2026-09-30: `src/render/view_math.h` (`viewmath::lookAt`, `perspective`,
  `orthographic`, all right-handed) is the one place a view or projection is
  built. The game camera, the editor camera and projection, and the shadow
  light go through it, and audit **CAM-01** forbids `bx::mtxLookAt`/`mtxProj`/
  `mtxOrtho` anywhere else. `kCullBackFaces` is `CULL_CW`.
- **Compensations, each checked:**
  - Fly-camera strafe: **was swapped, fixed.**
  - Fly-camera yaw: **was `-=`, fixed to `+=`.** Pointer-right now turns
    right on screen.
  - Pitch: vertical, unaffected.
  - `cameraLookBasis`: already right-handed (+X right at yaw 0); unchanged.
  - Picking: none exists; the editor's "pick" code is file pickers.
  - Shaders: only `u_viewProj`, with no view-space depth; unaffected.
  - Frustum extraction: Gribb–Hartmann, independent of handedness, so
    unchanged. Shown still correct by `cull_mode_test` §3.
  - Shadow `sy`: maps NDC to shadow-map UV, independent of handedness.
  - ImGuizmo: can't be exercised headless; it's in the visual check.
- **Two tests encoded the old handedness and failed once it changed.** My
  own `cull_mode_test` read forward from view column 2, and so did
  `camera_look_test`'s `forwardOf`. That reading is only valid for a
  left-handed view. `cull_mode_test` now unprojects the screen centre, which
  assumes no handedness; `forwardOf` takes its sign from
  `viewmath::kHandedness`, and its comment is corrected.
- Mutations, each red:
  - left-handed views again: 16 checks
  - the cull bit left at CCW: 6
  - fly strafe swapped back: 1
  - fly yaw flipped back: 1
  - a direct `bx::mtxLookAt`: CAM-01
- **Kit-visible, confirmed not guessed.** The FPS kit's look code is quoted in
  `presentation-split-kit-migration.md` as `m_yaw += dx * kSens`. With
  `CameraLook`'s convention, that turned toward −X, which was screen-right
  only because of the mirror. After this change, pointer-right turns the FPS
  player LEFT until it is flipped. `view-handedness-kit-migration.md` names
  it, and the other guide's recommended AFTER snippet now uses `-=`.
- Found on the way: the frustum near plane is wrong under homogeneous depth
  (latent, since no OpenGL backend is ever selected). Filed as WO-034.
- Stays **active** until the user's visual check, which also covers WO-032's.
- 2026-10-06, the visual check, by the owner: a one-sided 4 x 2 m quad textured "READ ME", LEFT on the left and "RIGHT +X" on its +X edge, in a fresh project. Editor and player: the text reads, RIGHT +X is on the right of the screen. Seen from behind the quad disappears (WO-032). The X gizmo moves it toward +X and the fly camera strafes and turns the way it is pushed. Closed.
