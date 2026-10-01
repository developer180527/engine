---
status: decided
id: DR-0005
title: bx's math types stay inside the frozen kit ABI until a deliberate break
date: 2026-09-18
source:
  - docs/rhi/evidence-coupling.md
---

## Decided
`bx::Vec3` and `bx::Quaternion` remain the members of `Transform`,
`PrevTransform` and `RigidBody`, components hashed into
`engine_abi::componentLayoutHash()`. Replacing bx is deferred, and audit
`RHI-01` stops its use spreading (new files including `<bx/…>` are findings).

## Rejected
Sweeping bx out as part of the bgfx/RHI migration, as a find-and-replace.

## Why
bx is a maths library, not a graphics dependency, and it survives bgfx's
removal untouched. But it is inside the frozen kit ABI: swapping it changes
those component layouts, so every built kit refuses to load and the number
`component_abi_test` pins changes. That is a change to make once,
deliberately, as an ABI break, not during a backend migration.

## What would reverse it
A planned kit-ABI version bump for other reasons (the break would ride on
it), or bx itself becoming a liability (unmaintained, or blocking a platform).
