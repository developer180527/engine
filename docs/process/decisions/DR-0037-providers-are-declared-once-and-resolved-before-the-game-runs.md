---
status: decided
id: DR-0037
title: Providers are declared once and resolved before the game runs
date: 2026-11-26
source:
  - docs/plans/provider-selection.md
  - docs/plans/provider-selection-research.md
---

## Decided
Each provider declares its kind, name, contract, platforms (an allow list),
platform needs, SDKs and required services in one manifest beside its source.
`project.json` chooses a provider per kind, optionally per target (platform and
flavour). One resolver checks that choice against the manifests and the target's
platform table. It runs in `engine_build` and at development startup, and it
refuses an impossible choice by name before anything runs. A shipped game links
exactly the chosen providers, registered from a generated list, and contains no
resolver. Providers depend on engine services, never on each other. Graphics APIs
are librhi backends, not providers. The only runtime fallback is librhi's declared
backend order. Device capabilities are the only thing checked at a shipped game's
startup.

## Rejected
- **Choosing at startup from an ini or setting, falling back to a dummy when the
  name is unknown.** That is Unreal, Godot, O3DE and CryEngine. All of them find
  a bad choice late.
- **Quietly turning off or stubbing a provider whose SDK or platform is missing,
  so the build passes.** CryEngine and O3DE do this.
- **Deny lists, and self-registration through static initialisers.**
- **One provider per graphics API.**

## Why
The owner (2026-11-26), after the research in `provider-selection-research.md`.
Every engine studied uses the same basic mechanism. Their documented pain is all
in when a wrong choice surfaces and in platform metadata that nothing enforces.
A build-time refusal costs nothing per frame. It also extends DR-0035 (refuse by
name) to the point where it is cheapest to act on.

## What would reverse it
A platform where the provider must be picked on the device (for example, by GPU
vendor among two renderers). Even then, the choice would be between providers
already resolved and linked, so the rest stands.
