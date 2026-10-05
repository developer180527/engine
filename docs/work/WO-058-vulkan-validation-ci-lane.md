---
status: plan
id: WO-058
title: A Vulkan CI lane — software driver plus validation layers, before the first backend commit
program: renderer
priority: P1
size: S
state: todo
depends: []
touches:
  - .github/workflows/ci.yml
source: DR-0012 (2026-10-05)
---
## Why
The Vulkan backend must be tested from its first commit, and CI runners have
no GPU. Mesa's lavapipe is a conformant software Vulkan driver, and with the
Khronos validation layers it turns API misuse into a red build.

## Done when
- [ ] a Linux CI job installs lavapipe and the validation layers and runs a Vulkan test under them
- [ ] any validation message fails the job, mutation-checked with a deliberately wrong call
- [ ] the job is in the push subset, not only the full matrix
