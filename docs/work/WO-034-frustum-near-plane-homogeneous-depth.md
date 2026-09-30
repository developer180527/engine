---
status: plan
id: WO-034
title: Frustum near plane is wrong under homogeneous depth (OpenGL)
program: renderer
priority: P3
size: S
state: todo
touches:
  - src/render/world/frustum.cpp
source: found 2026-09-30 while checking frustum extraction for WO-033
---
## Why
`extractFrustumPlanes` takes the near plane from row 2 alone. That is the near plane for 0..1 depth (Metal, D3D, Vulkan). With homogeneous −1..1 depth (OpenGL), row 2 alone is a plane partway into the frustum, so things between the camera and it would be culled.

It's latent: the engine never selects an OpenGL backend today (`device.cpp`). WO-024 (backend chosen at runtime) could make it live.

## Done when
- [ ] `extractFrustumPlanes` takes whether depth is homogeneous, and uses row 3 + row 2 for the near plane when it is
- [ ] `cull_mode_test` §3 runs with homogeneous depth too: a point just past the near plane is inside
