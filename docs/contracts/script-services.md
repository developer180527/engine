---
status: as-built
contract: script-services
kind: interface
state: provisional
owner: src/runtime/scripting
header: src/runtime/scripting/script_services.h
implementations:
  - real: src/plugins/jolt_plugin.h#JoltPlugin
  - real: src/runtime/services/anim_service.h#AnimService
  - real: src/plugins/audio_plugin.h#AudioPlugin
  - stub: src/runtime/scripting/script_host.h#m_warnedPhysics
covers:
  - src/runtime/scripting/script_services.h
verified: 2026-09-27
---

# script-services — what scripts may call, before the backend exists

`IPhysicsService`, `IAnimService`, `IAudioService`: the calls scripts make
(`Physics.applyImpulse`, `Audio.play`, …), declared ahead of their backends so
the scripting contract is stable. The file's own words: the calls "safely no-op
until the backing service exists, and 'just work' once it does — no contract
change." This is the model the registry generalises.

## Nothing
Until a backend registers, the script host holds null and each call no-ops,
warning once (`m_warnedPhysics` / `m_warnedAudio`) — a stub that says so, not
silence. `teleport` returns false when the entity has no body this backend
owns; character-controller calls default to no-ops for backends without one.

## Ownership
Services take an explicit `(world&, entity_t)` rather than a `flecs::entity`,
so a backend never depends on which world an entity was wrapped against.
Backends are owned by their plugin/runtime; the host holds a non-owning pointer.

## Threading
Not yet written.

## Timing
Not yet written.

## Errors
Not yet written.
