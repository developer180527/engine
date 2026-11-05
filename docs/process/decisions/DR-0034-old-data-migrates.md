---
status: decided
id: DR-0034
title: Old data migrates, never breaks; unknown components are kept
date: 2026-11-06
source:
  - ARCHITECTURE.md
---

## Decided
A scene or project saved by any earlier version loads, including across
major versions: each component revision registers a migration, run at load and
tested against data saved before the change. A component the running engine
does not know (a provider that is absent) is loaded as opaque data and saved
back unchanged (`docs/plans/engine-architecture.md` §4).

## Rejected
Refusing old data, or dropping unknown components on load: both destroy a
user's work.

## Why
A user's scenes outlive engine versions and provider choices. `kAbiRevision`
(WO-051) refuses old kits, which is right for code; data must instead be
carried forward.

## What would reverse it
None expected.
