---
status: decided
id: DR-0035
title: Refuse by name; never fall back silently
date: 2026-11-06
source:
  - ARCHITECTURE.md
---

## Decided
A missing or mismatched provider, kit, service, asset or extension is an
error that names it and what was available, at the earliest point it is known.
The engine never silently substitutes something else.

## Rejected
Logging and falling back to a default (today's `stock_plugins.h`: an unknown
physics name runs Jolt).

## Why
A silent fallback makes a typo run a different engine than the one asked
for, and hides the failure until it shows up as wrong behaviour. The kit ABI and
librhi already refuse by name.

## What would reverse it
None expected; a deliberate fallback is a named, declared choice (an
extension's declared fallback), not a silent one.
