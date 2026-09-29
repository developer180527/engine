---
status: as-built
contract: audio-provider
kind: c-abi
state: frozen
owner: include/engine
header: include/engine/engine_audio_provider.h
implementations:
  - real: src/audio/miniaudio_provider.cpp#engineAudioProviderV1
  - null: none
tests:
  - tests/audio_conformance
  - tests/audio_provider_asan_test.cpp
covers:
  - include/engine/engine_audio_provider.h
  - src/audio/miniaudio_provider.cpp
verified: 2026-09-27
---

# audio-provider — audio, replaceable down to the device

`EngineAudioProviderV1`: the table a replacement audio engine implements
(miniaudio today; FMOD/Wwise adapters or a Rust engine are the intended
others). The provider calls back ONLY through services the engine hands it.
`tests/audio_conformance` is an executable specification run against the real
miniaudio provider.

## Nothing
No null provider: "no output device" is handled INSIDE the contract — a host
with no device is normal, and `create` returns `E_NO_DEVICE` rather than
failing. Playback returns `NO_VOICE` for an invalid sound or an exhausted voice
budget.

## Ownership
The provider owns the device, the real-time thread and the mixer. Sound data:
the provider owns the result of `createSound`; the engine may free the input
bytes the moment the call returns. A stream source's `read` and `userData` must
stay valid until `destroySound`. The host services pointer is never null and
outlives the instance.

## Threading
"Game thread" calls only; nothing is called from the audio thread, and the
provider must never block the audio thread. Stream reads happen on the
provider's streaming worker, never the real-time thread. Diagnostics may
overlap other calls. Parallel work uses the host's pool (`parallelFor`), not
the provider's own threads.

## Timing
Control crosses at frame rate: about one batched call per frame for emitters
and listener. The mixer runs at audio rate and crosses the ABI zero times.

## Errors
Status codes on every call that can fail; a device failure in a shipped game
must stay visible. No unwinding across the boundary.
