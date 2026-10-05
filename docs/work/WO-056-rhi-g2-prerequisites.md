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
source: DR-0012; study 007 (2026-10-05)
---
## Why
G2 writes the API every later phase and every other consumer inherits. Its
shape is still open in writing: study 007 is a first pass with unverified
claims, and open decisions 6 and 7 are unanswered. Writing the backend first
would decide them by accident.

## Done when
- [ ] study 007's second pass: every *verify* claim checked against its source, cited by section, verdict landed in `design-api.md`
- [ ] study 001 concluded (bindless-only or binding sets), answering open decision 7
- [ ] study 002 concluded (the RHI takes shader bytes or compiles), answering open decision 6
- [ ] the validation story written: what the wrapper device checks, on by default in dev builds
- [ ] WO-024's runtime backend choice is written into `design-api.md` as a requirement
- [ ] the minimum spec (open decision 3) answered as tiers: portable (webgpu.h) and fast (Metal 4 / Vulkan 1.3)
