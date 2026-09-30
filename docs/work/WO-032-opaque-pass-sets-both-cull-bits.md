---
status: plan
id: WO-032
title: The opaque pass sets both cull bits, which is undefined on D3D11 and Vulkan
program: renderer
priority: P1
size: S
state: todo
touches:
  - src/render/pipeline/opaque_pass.cpp
  - src/render/pipeline/shadow_pass.cpp
source: found 2026-09-30 while pinning the import winding convention (WO-010)
---
## Why
`opaque_pass.cpp` uses `BGFX_STATE_DEFAULT | BGFX_STATE_CULL_CCW`. `DEFAULT` already contains `BGFX_STATE_CULL_CW`, so both cull bits are set and bgfx decodes cull mode **3**.

`renderer_d3d11.cpp` and `renderer_vk.cpp` index a three-entry `s_cullMode[]` (none, front, back) with it. That's an out-of-bounds read, so Windows and Linux cull by whatever lies past the array. It "works" only by luck.

## Done when
- [ ] the opaque state names exactly one cull mode. The intent is `CULL_CW`: glTF, Assimp and `ImportedScene` all make counter-clockwise the front face, and `CULL_CW` removes back faces.
- [ ] a test asserts that every render state the pipeline builds has at most one cull bit set, so the combination cannot come back
- [ ] the shadow pass's `CULL_CCW` (it culls *front* faces, a standard shadow-acne trick) is checked on purpose and commented as such, rather than assumed
- [ ] a scene renders the same on macOS (Metal maps cull mode 3 to "none" today, so it may currently draw back faces; compare before and after)

## Not in scope
Double-sided materials. Their state already sets no cull bit.
