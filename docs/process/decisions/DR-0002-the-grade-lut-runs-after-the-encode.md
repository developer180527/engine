---
status: decided
id: DR-0002
title: The colour-grading LUT runs after the sRGB encode, on display-referred values
date: 2026-09-17
source:
  - docs/plans/colour-pipeline.md
---

## Decided
The output pass runs exposure → tone map → sRGB encode → **grade**. A `.cube`
LUT therefore sees display-referred, sRGB-encoded values in [0,1], and
`LutLibrary` refuses one that declares a domain outside [0,1].

## Rejected
Grading in scene-linear light before the tone map (where some pipelines put
it), and accepting LUTs built for scene-linear or log input.

## Why
A LUT is only correct for the domain it was built for, and the engine's LUT
is a display-referred sRGB [0,1] artefact by construction. Fed the wrong
domain it "looks cursed" with no error (the stage B review's words), and the
only guard used to be prose. `colour_test` §8 pins the order (a squaring LUT
gives 0.25, not 0.235) and reads `fs_output.sc` to require it.

## What would reverse it
A grading workflow that authors in scene-linear or log (an HDR grade, stage D
of the colour plan). That would add a second LUT slot with its own domain, not
move this one: an HDR surface already skips this grade (C, `colour-pipeline.md`).
