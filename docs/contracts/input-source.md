---
status: as-built
contract: input-source
kind: interface
state: provisional
owner: src/runtime/input
header: src/runtime/input/input_sources.h
implementations:
  - real: src/runtime/input/input_sources.h#HidSource
  - real: src/runtime/input/input_sources.h#WindowSource
  - fake: src/runtime/input/input_sources.h#ReplaySource
tests:
  - tests/sim_replay_test.cpp
  - tests/input_test.cpp
covers:
  - src/runtime/input/input_sources.h
verified: 2026-09-27
---

# input-source — where raw input events come from

`IInputSource`: poll timestamped HID events and enumerate devices. The raw HID
backend, the window fallback, and replay (recorded events fed back in — the
fake that makes input-driven simulation testable and deterministic).

## Nothing
Not yet written.

## Ownership
Not yet written.

## Threading
Not yet written.

## Timing
Not yet written.

## Errors
Not yet written.
