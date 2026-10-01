---
status: decided
id: DR-0009
title: The runtime reads cooked content only; a missing cook is a job
date: 2026-10-01
source:
  - docs/contracts/cook-request.md
---

## Decided
No runtime parses a source format (glTF, FBX, a source image). An asset
asked for by its source path with no Ready cook is a cook request to an
`ICookRequests`: the editor's `CookService` cooks it while a placeholder
shows, and a build with no cooker reports "not cooked".

## Rejected
Parsing the source in-process when nothing is cooked (the AsyncLoader's
Assimp path and the runtime glTF importer, deleted in WO-018), including the
"quick preview" form of it.

## Why
That was a second parser beside the cook's, so the same asset looked
different depending on whether it had been cooked yet: a pipeline bug that
looks like a rendering bug. It also put Assimp and every encoder in every
dev-build runtime (WO-017: the player shrank 33.6 → 24.4 MB without them).

## What would reverse it
Nothing short of giving up the cook as the single source of truth. A faster
first view of an uncooked asset is a faster COOK (priority, the DDC), not a
second parser.
