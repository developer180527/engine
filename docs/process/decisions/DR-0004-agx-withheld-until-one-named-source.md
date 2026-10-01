---
status: decided
id: DR-0004
title: AgX is withheld until its constants come from one named source
date: 2026-09-17
source:
  - docs/plans/colour-pipeline.md
  - src/core/display_transform.h
---

## Decided
The engine ships two tone mappers: Khronos PBR Neutral (the default) and None.
`ToneMapper` has room for AgX and nothing more.

## Rejected
Shipping AgX as the filmic alternative now, as the colour plan recommended,
from whichever implementation is to hand.

## Why
The references disagree: Filament's AgX takes Rec.2020 input with an 8-term
contrast fit, three.js and the "minimal AgX" it credits take Rec.709 through a
conversion with a 7-term fit. Two constant sets for one name, and a subtly
wrong tone curve is an error no test here can see. PBR Neutral, by contrast,
is copied constant for constant from `KhronosGroup/ToneMapping`.

## What would reverse it
AgX's constants copied from ONE named source file, with a CPU reference and
tests like PBR Neutral's. This is a "not yet", not a "no".
