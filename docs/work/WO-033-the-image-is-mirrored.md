---
status: plan
id: WO-033
title: Every camera renders the world mirrored left-to-right
program: renderer
priority: P1
size: M
state: todo
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
- [ ] every camera builds a right-handed view and projection: `PrimaryCameraFinder`, the editor fly camera, the shadow pass's light
- [ ] `passstate::kCullBackFaces` becomes `BGFX_STATE_CULL_CW`. `cull_mode_test` §2 fails until it does, because it derives the bit from the camera path, and §3 fails if only some cameras change.
- [ ] a test pins "world +X in front of the camera lands on the RIGHT of the screen", replacing `cull_mode_test`'s note
- [ ] every compensation is found and removed: the fly camera's `-step` on right, its yaw and pitch signs, `CameraLook`'s basis, the gizmo, picking/unprojection, and the shadow map's texel lookup (`sy` in `shadow_pass.cpp`), each checked, not assumed
- [ ] an asymmetric asset (text on a texture) reads correctly in the editor and the player, checked by eye by the user before closing
- [ ] **kit-visible**: `Kits/` and `fps_shooter/` are untracked, and their look and strafe signs may carry the same compensation. `docs/guides/` gets a migration note listing what flips, since that code cannot be edited from here.

## Not in scope
Changing the world's handedness. The world is right-handed and stays so. Only the view is wrong.
