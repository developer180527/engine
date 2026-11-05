---
status: decided
id: DR-0028
title: One C API is the engine's only public boundary; every language binds to it
date: 2026-11-06
source:
  - ARCHITECTURE.md
  - docs/plans/engine-api.md
---

## Decided
The engine's public surface is one C API (`eng*`, drafted in
`docs/plans/engine-api.md`). Programs, kits, the editor, the tools and every
language package (C++, Python, Swift, JS/WASM, Rust) use it. The C++ layer is a
header-only wrapper over it, as librhi's is. Library users and editor users
stand on the same API, so there is one contract, not a source API for one and
a kit ABI for the other. From 1.0 it carries the promise in `engine-api.md` §2
(source, binary, behaviour, data), checked in CI.

## Rejected
A C++ API as the boundary, for library users with the kit ABI beside it for
editor users (the 2026-10-29 plan): two contracts to keep stable, and C++ binds
to no other language and to no other compiler's standard library. Also
rejected: a C++ API with generated C bindings, which makes C the afterthought
it must not be.

## Why
The owner (2026-11-06): the API must be C-based for stability, so that any
language can connect to it. C is the one ABI every language and platform can
call, stable across compilers on a platform. librhi made the same choice
(its DR-0015) and has kept it through 51 ABI revisions.

## What would reverse it
A consumer that needs a C++-only feature at the boundary and cannot use the
wrapper. None is known.
