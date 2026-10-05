---
status: plan
id: WO-056
title: RHI G2 prerequisites — the reading and decisions the first triangle needs
program: renderer
priority: P1
size: M
state: todo
depends: []
touches:
  - docs/rhi/open-decisions.md
  - docs/rhi/studies/README.md
  - docs/rhi/design-api.md
  - docs/rhi/design-axioms.md
  - docs/rhi/toolchain-shaders.md
source: DR-0012; study 007 (2026-10-05)
---
## Why
G2 writes the API every later phase and every other consumer inherits. Its
shape is still open in writing: study 007 is a first pass with unverified
claims, and open decisions 6 and 7 are unanswered. Writing the backend first
would decide them by accident.

## Done when
- [ ] study 007's second pass: every *verify* claim checked against its source, cited by section, verdict landed in `design-axioms.md` "Tiers" (the ONE landing doc, workflow §3; `design-api.md`, open decision 3 and `phases.md` link to it). The two reads that decide the most come first: the WebGPU bindless timeline, and Dawn versus wgpu-native (size, build, `webgpu.h` ABI stability)
- [ ] study 008 concluded: the shader source language (HLSL or Slang) and whether its SPIR-V reaches WGSL for tier H
- [ ] study 001 concluded (bindless-only or binding sets), answering open decision 7
- [ ] study 002 concluded (the RHI takes shader bytes or compiles), answering open decision 6
- [ ] the validation story written: what the wrapper device checks, on by default in dev builds
- [ ] WO-024's runtime backend choice is written into `design-api.md` as a requirement
- [ ] the minimum spec (open decision 3) answered as tiers, by linking to `design-axioms.md` "Tiers" rather than restating it
