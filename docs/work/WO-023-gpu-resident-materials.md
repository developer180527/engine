---
status: plan
id: WO-023
title: Material data GPU-resident — the CPU binds indices, never walks contents
program: renderer
priority: P3
size: L
state: todo
depends: [WO-019]
contracts: [render-pipeline]
touches:
  - src/render/pipeline/opaque_pass.cpp
  - scripts/engine_audit.py
source: conversation 2026-09-22 — "assume all material data is in VRAM"
---
## Why
Every material bind in `opaque_pass.cpp` has the CPU walk `mat->blocks` and `mat->textureBinds`, resolve textures and issue uniforms.

Under a GPU-based RHI that data already lives in VRAM, and reading it back to describe it to the GPU is exactly the wrong shape.

## Done when
- [ ] material parameters live in one GPU buffer, uploaded at load or author time and never per frame
- [ ] each object row carries a material index; the shader fetches its own parameters
- [ ] textures go through a descriptor array, and rows carry slot numbers
- [ ] the cooker pins a fixed parameter stride per shader. Check first that the `.material` format allows this.
- [ ] audit rule **RHI-02**: the submit path may not dereference `Material*`/`Texture*`
- [ ] draws with different materials but the same mesh batch into one draw, and the draw count before and after is measured

## Contract
Nothing: **null** material index 0 is a defined default material (magenta in development builds), never garbage.
