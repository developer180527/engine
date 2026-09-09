---
status: as-built
tier: working
verified: 2026-09-09
covers:
  - src/components/camera_look.h
tests:
  - tests/camera_look_test.cpp
---
# Migrating a look controller to the presentation split

Stage 4 of the command architecture. The engine side is built; **the two
changes below are in `Kits/` and `fps_shooter/`, which are gitignored**, so
they are written down with their destination rather than made. Nothing here is
speculative — both defects were read out of the current
`Kits/FPSPlayerControllerKit/include/fps_controller_kit.h`.

## 1. The render-rate write to a hashed component

`onFrame` latches the mouse — correctly, since a look latched in the fixed step
lags the frame rate and the game feels broken — and then writes the result into
the camera's `Transform.rotation`:

```cpp
// fps_controller_kit.h, onFrame  — BEFORE
m_yaw   += dx * kSens;
m_pitch -= dy * kSens;
bx::Vec3 fwd, rgt, up; lookBasis(fwd, rgt, up);
const bx::Quaternion rot = basisQuat(fwd, rgt, up);
m_camQ.each([&](flecs::entity, Transform& t, const Camera& c,
                const CharacterController&) {
    t.rotation = rot;                     // <- hashed SimState, at render rate
});
```

`Transform` is a hashed `SimState` component. A render-rate write to it means
the same content at 1 frame/tick and 2 frames/tick produces different world
state — **BUG-0053's defect class**, alive in the tree today only because no
determinism-gate tier drives input, so nothing measures it.

```cpp
// AFTER — the aim stops being simulation state
m_yaw   += dx * kSens;
m_pitch -= dy * kSens;
m_camQ.each([&](flecs::entity e, Transform&, const Camera& c,
                const CharacterController&) {
    if (!c.isPrimary) return;
    e.set<CameraLook>({ m_yaw, m_pitch });   // SimExempt; composed by the renderer
});
```

`PrimaryCameraFinder` composes `CameraLook` into the view when the camera
carries one, and leaves `Transform.rotation` alone. **Delete `lookBasis` and
`basisQuat` from the kit** and call `cameraLookBasis` (`components/camera_look.h`)
for the movement direction and the firing ray — the renderer uses that same
function, and two copies of the arithmetic is how the shot ends up going
somewhere the crosshair is not.

**This is not a determinism fix, and should not be described as one.** Yaw and
pitch still accumulate at frame rate and still feed `charMove` and the firing
raycast, which are simulation. What is removed is the write reaching a hashed
component. Closing the rest needs the **input layer** named in
`runtime/sim_command.h`: intent sampled per tick rather than per frame.

## 2. The eye-height lift

```cpp
// fps_controller_kit.h, after the physics step — BEFORE
m_camQ.each([&](flecs::entity, Transform& t, const Camera& c,
                const CharacterController&) {
    t.position.y += kEyeHeight;           // feet -> eye
});
```

Two problems in one line. It is a **non-idempotent read-modify-write** — it
adds to whatever is there, so it is only ever correct because
`writeBackCharacters` happens to have reset the value first that step; run it
twice and the camera climbs. And it writes `position` on an entity with a
`CharacterController`, which physics owns, so **the stage-3b authority watcher
now reports it** by name, field and phase.

The fix is structural: make the camera a **child** of the character with a
constant local offset.

```cpp
// AFTER — one entity for the body, one for the eye
flecs::entity eye = w.entity("Player Eye")
    .set<Transform>({ {0.0f, kEyeHeight, 0.0f}, {0,0,0,1}, {1,1,1} })
    .set<Camera>(cam)
    .set<CameraLook>({});
eye.child_of(playerBody);                 // carried by the character
```

The offset is then a constant nothing rewrites, the character keeps sole
ownership of its position, and the watcher goes quiet because there is no
longer a write to be quiet about. `camera_look_test.cpp` §4 pins the property
this depends on: a parented camera is carried by its parent while aiming by its
own `CameraLook`.

Splitting the entity in two also removes the `RigidBody`-plus-`CharacterController`
hazard by construction, and it is what makes a spring arm, a third-person
camera or a vehicle mount possible later without touching the controller.
