---
status: decided
id: DR-0031
title: Cooked files are in the engine's own formats; a provider's form is a rebuildable cache
date: 2026-11-06
source:
  - ARCHITECTURE.md
---

## Decided
Everything a game ships is in a format the engine defines, documents in
`docs/reference/`, versions and migrates: meshes, scenes, materials, shaders,
animation and audio (WO-069). A provider's own form (ozz's runtime animation, a
GPU texture format, decoded audio) is a derived cache built from it: it can be
deleted and rebuilt, and is never the only copy. The one exception is
middleware banks (Wwise, FMOD), which a project declares as requiring that
provider.

## Rejected
Shipping each provider's own serialisation (today's ozz archives inside
cooked clips): upgrading or replacing the provider invalidates every cooked
file.

## Why
Swappable providers (DR-0029) mean nothing if the data a game ships only one
provider can read. Renderers already work this way (texture formats are a
per-renderer cook profile).

## What would reverse it
None expected; a provider that cannot be fed from an engine format is
treated as middleware (declared, required by name).
