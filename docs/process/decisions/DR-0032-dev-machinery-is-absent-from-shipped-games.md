---
status: decided
id: DR-0032
title: Development machinery is absent from a shipped game, not switched off
date: 2026-11-06
source:
  - ARCHITECTURE.md
---

## Decided
A shipped game contains the runtime, its providers, its kits, its cooked
assets and crash reporting. The editor, tools, importers, cookers, shader
compilers, hot reload, runtime module loading, validation, debug consoles and
the profiler UI are absent from its binary, and the runtime builds nothing
(WO-068). It is checked, not trusted: a symbol check of the shipped player and
a check of the exported folder (WO-066).

## Rejected
Switching development features off at runtime (today's `Reload::Never`,
`quietForShipping`): a flag can be flipped, and the code still costs size and
attack surface.

## Why
Absent code cannot run, leak or be enabled by accident; it also keeps
shipped binaries small and load times predictable.

## What would reverse it
None expected.
