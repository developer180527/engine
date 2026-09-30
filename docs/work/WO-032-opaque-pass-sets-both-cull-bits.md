---
status: plan
id: WO-032
title: The opaque pass sets both cull bits, which is undefined on D3D11 and Vulkan
program: renderer
priority: P1
size: S
state: active
touches:
  - src/render/pipeline/opaque_pass.cpp
  - src/render/pipeline/shadow_pass.cpp
source: found 2026-09-30 while pinning the import winding convention (WO-010)
---
## Why
`opaque_pass.cpp` uses `BGFX_STATE_DEFAULT | BGFX_STATE_CULL_CCW`. `DEFAULT` already contains `BGFX_STATE_CULL_CW`, so both cull bits are set and bgfx decodes cull mode **3**.

`renderer_d3d11.cpp` and `renderer_vk.cpp` index a three-entry `s_cullMode[]` (none, front, back) with it. That's an out-of-bounds read, so Windows and Linux cull by whatever lies past the array. It "works" only by luck.

## Done when
- [x] the opaque state names exactly one cull mode: **`CULL_CCW`**, not the `CULL_CW` this order first assumed. It is derived from the camera path, which mirrors the image (WO-033), so back faces reach the screen counter-clockwise.
- [x] a test asserts that every render state the pipeline builds has at most one cull bit set, so the combination cannot come back (a `static_assert` in `pass_states.h`, plus `cull_mode_test`)
- [x] the shadow pass's `CULL_CCW` is checked on purpose rather than assumed. **It is NOT the front-face shadow-acne trick this order guessed**: the light's view has the same mirror, so it removes back faces, and it now uses the same constant.
- [ ] **checked by eye on macOS, by the user.** It will NOT render identically, and that is correct. Metal mapped cull mode 3 to "none", so macOS culled **nothing** until now. Closed meshes should look the same. A single-sided open surface (a plane, a card, a mesh with a hole) seen from behind now disappears, as it always should have. Anything that vanishes but should not is a mesh that needs `doubleSided` or has flipped winding.

## Not in scope
Double-sided materials. Their state already sets no cull bit.

## Log
- 2026-09-30: **This order's premise was half wrong, and the test is what
  showed it.** The both-bits bug was real. But the bit to KEEP is `CULL_CCW`,
  not `CULL_CW`, because every camera builds a left-handed view over a
  right-handed world. That mirrors the image (world +X lands on the left), so
  back faces reach the screen counter-clockwise. Measured by running
  `PrimaryCameraFinder` and projecting, not by algebra. Corroborated by the
  editor fly camera, whose "right" key moves along −right. The mirror is now
  WO-033.
- `src/render/pipeline/pass_states.h` builds both passes' states in one place.
  A cull bit is replaced, never OR'd. A `static_assert` refuses any pass state
  with both bits, and the WO-032 state no longer compiles.
- `cull_mode_test`:
  - §1: every pass state has at most one cull bit, and double-sided is
    unchanged flag for flag.
  - §2: the back-face bit is DERIVED from the real camera path, at six poses,
    with and without homogeneous depth.
  - §3: all three cameras agree on handedness.
  - Its own guard caught a mistake of mine. My assumed yaw convention put the
    test triangle *behind* the camera at 90°, and the 200° pose passed by
    luck. The test now reads the camera's basis from the view the finder
    built.
- Mutations: the both-bits state is refused at compile time; the other bit is
  red at 6 poses; a half-done WO-033 (only the game camera right-handed) is
  red on §3.
- Stays **active**: the visual check on macOS is the user's, and the result is
  expected to differ (see the last item).
