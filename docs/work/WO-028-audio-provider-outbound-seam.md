---
status: plan
id: WO-028
title: Finish the audio provider's outbound seam (B) before any physics ABI
program: providers
priority: P3
size: M
state: todo
contracts: [audio-provider]
touches:
  - include/engine/engine_audio_provider.h
  - docs/architecture/provider-abi.md
source: conversation 2026-09-2x — provider seams A/B/C
---
## Why
Seam B (the outbound provider) exists for audio as a header only.

Finishing it on the smaller subsystem teaches us what a physics provider ABI would cost, before we freeze anything bigger.

## Done when
- [ ] the header has a loaded implementation and passes the existing Rust conformance suite
- [ ] `provider-abi.md` records what seam B cost and what a physics seam would need

## Contract
Nothing: **null** audio provider. The engine runs silent and says so once, which matches what a server does today.
