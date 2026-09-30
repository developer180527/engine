---
status: reference
---
# The view is no longer mirrored: what a kit may need to flip

> **For kit authors after WO-033 (2026-09-30).** `Kits/` and `fps_shooter/` are
> not in this repository, so this note is how the change reaches them. Nothing
> here changes the ABI; it changes what you *see*, and any sign you tuned by eye.

## What changed

The world has always been right-handed: +Y up, and a camera looks down its
local −Z. But every camera built its view with bx's *left-handed* default, and
that mirrored the image: **world +X appeared on the left of the screen.**

Every view and projection is now built in one place, `src/render/view_math.h`,
right-handed. The image is no longer mirrored:

| | before | after |
|---|---|---|
| world +X in front of the camera | left of the screen | **right** |
| text in a texture | reads backwards | reads correctly |
| a character's left hand | drawn on the right | drawn on the left |
| back-face culling | `CULL_CCW` | `CULL_CW` |

## What in a kit may now be wrong

Anything tuned *by eye* against the mirror is now backwards. The engine's own
editor camera had two such signs, and both are fixed. Look for the same two in
your kit:

1. **Mouse look turns the wrong way.** With `CameraLook`, `yaw` turns toward
   −X as it grows (`forward = (−sin yaw, …, −cos yaw)`), so pointer-right must
   *decrease* it:

   ```cpp
   look.yaw -= pointerDx * sensitivity;   // pointer right → turn right
   ```

   If your kit has `look.yaw += pointerDx * …` and turning felt right before
   this change, that `+` was compensating for the mirror. Flip it.

   **Known case: `Kits/FPSPlayerControllerKit/include/fps_controller_kit.h`**,
   `onFrame`, has `m_yaw += dx * kSens`. It is quoted in
   `presentation-split-kit-migration.md` §1. After this change, pointer-right
   turns the FPS player *left* until it becomes `m_yaw -= dx * kSens`.

2. **Strafe goes the wrong way.** `cameraLookBasis` returns the true right
   vector (+X at yaw 0). "Move right" is `+right`:

   ```cpp
   if (moveRight) contribution += right * speed;
   ```

   A `-right` for the right key, or keys swapped, was the same compensation.

3. **Your own screen-space maths**: a crosshair offset, a world-to-screen label,
   a minimap. If it builds its own projection or flips x to match what it saw,
   it now disagrees with the engine. Use the engine's view/projection, or
   `viewmath::` from `render/view_math.h`.

4. **Content mirrored to compensate**: a texture flipped so its text read
   correctly, or a model modelled mirror-image. It now shows mirrored the
   other way.

Pitch (vertical look) and up/down movement are unaffected. The mirror was
left-to-right only.

## How to check

Point the camera at something asymmetric (text on a texture is best), press
play, and check that:

- it reads correctly
- pointer-right turns right
- "right" moves right
