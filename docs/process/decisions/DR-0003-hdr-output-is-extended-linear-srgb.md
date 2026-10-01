---
status: decided
id: DR-0003
title: macOS HDR output uses extended linear sRGB, not Display P3
date: 2026-09-17
source:
  - docs/plans/colour-pipeline.md
---

## Decided
The EDR layer's colour space is `kCGColorSpaceExtendedLinearSRGB`, on an
`RGBA16Float` surface.

## Rejected
Extended linear **Display P3**, which the plan first specified and Apple's
EDR samples commonly use.

## Why
The engine shades in linear Rec.709/sRGB primaries. Handing those values to a
P3 colour space reinterprets them as P3 primaries, which silently
over-saturates everything on screen: the plan's P3 was a gamut error, not a
choice. A spike confirmed bgfx does not clobber the layer's EDR state.

## What would reverse it
Shading in a wider gamut (Rec.2020 or P3 working space). Then the output
colour space should follow the working space, and this becomes P3 or Rec.2020
deliberately.
